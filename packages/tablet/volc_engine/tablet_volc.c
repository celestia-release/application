// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * LVGL Volc engine UI, Doubao-style chat page (docs/design/nova/anypi_nova_volc_doubao.md):
 *   - default TEXT mode: centered conversation (dim user line + large bot line), no preview
 *   - VIDEO mode (camera toggle): fullscreen NV12 preview (same pattern as camera/tablet_camera.c)
 *     + translucent caption bar; stream registration follows the mode (epoch + bg thread)
 *   - one-way voice: push-to-talk round button (icon + text), uplink gated to G.711 silence
 *   - camera uplink follows the video mode (engine video_enable re-applied after init)
 *   - latest user/bot text on 4 Hz queue poll, both mode label sets
 *
 * Preserved from the previous version: background volc_engine_init() thread with
 * join-on-exit, RTC .so preloading, network wait, ak-volc-cmd console injection.
 *
 * NOTE: console_cmd_register is looked up via dlsym at runtime (not a direct call) so
 * that the SO can be dlopen'd even when console.o was not pulled into the main binary.
 */
#define LOG_TAG "TABLET_VOLC"

#include "tablet_volc.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include "tablet_common.h"
#include "tablet_font.h"
#include "app_main.h"
#include "log.h"
#include "volc_engine_app.h"
#include "notification.h"
#include "console.h"
#include "network.h"
#include "config.h"
#include "multimedia.h"
#include "bitmap.h"
#include "system_time.h"
#include "system_thread.h"

#ifndef CONFIG_IMAGE_WIDGET
#define CONFIG_IMAGE_WIDGET 1
#endif

#define VOLC_INVALID_HANDLE         (-1)
#define VOLC_PREVIEW_TIMER_MS       (1000 / 15)
#define VOLC_UI_TIMER_MS            250

/* Colors (Doubao dark scheme). */
#define VOLC_COLOR_BG               0x12141A
#define VOLC_COLOR_WHITE            0xFFFFFF
#define VOLC_COLOR_DIM              0x9AA3AD
#define VOLC_COLOR_BTN_BG           0x2C2C2E
#define VOLC_COLOR_BTN_ON           0x34C759
#define VOLC_COLOR_END              0xE5484D
#define VOLC_FLOAT_OPA              LV_OPA_60

/* volc_engine_init() 异步初始化状态：
 * 0=进行中，1=成功，-1=失败，-2=未联网（仅用于UI线程提示） */
enum {
    VOLC_INIT_IN_PROGRESS = 0,
    VOLC_INIT_OK = 1,
    VOLC_INIT_FAIL = -1,
    VOLC_INIT_NO_NETWORK = -2,
};

typedef struct {
    lv_obj_t *main;
    lv_obj_t *window;
    int state;
    int init_note_shown;   /* 1: init-failure msgbox already shown once */
    int closing;           /* 1: exit sequence started; gate stream callbacks */
    int ptt_active;        /* 1: click-toggle talk in progress */
    int video_mode;        /* 0: text conversation (default), 1: fullscreen video */
    int mode_synced;       /* 1: engine video_enable re-applied after init OK */
    pthread_t init_tid;
    int init_join_pending; /* 1: join init_tid on exit (joinable thread, no detach) */

#if CONFIG_IMAGE_WIDGET
    lv_obj_t *image;             /* NV12 preview widget */
    void *preview_buf;           /* bitmap buffer, w*h*3/2 */
    uint32_t preview_w;
    uint32_t preview_h;
    lv_image_dsc_t image_dsc;
#endif

    lv_obj_t *status_label;      /* state pill text */
    lv_obj_t *chat_panel;        /* conversation text area (shared by both modes) */
    lv_obj_t *chat_user;
    lv_obj_t *chat_bot;
    lv_obj_t *btn_ptt;           /* talk button (click to start/stop) */
    lv_obj_t *btn_ptt_icon;      /* runtime-drawn mic icon container */
    lv_obj_t *btn_ptt_label;
    lv_obj_t *btn_cam;           /* video mode toggle */
    lv_obj_t *btn_cam_label;

    lv_timer_t *timer;           /* 15 Hz preview refresh */
    lv_timer_t *ui_timer;        /* 4 Hz captions/status/init note */
} context_t;

static context_t context;
static context_t *pContext = &context;

static multimedia_t *g_multimedia = NULL;
static int s_client_handle = VOLC_INVALID_HANDLE;
static pthread_mutex_t s_stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_preview_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_preview_epoch;
static volatile int s_has_call_data;

#if CONFIG_IMAGE_WIDGET
static int volc_preview_frame_cb(int handle, media_frame_info_t *frame, void *user_data);
#endif

/* 最新一条字幕（UI 线程写，label 只在变化时重绘） */
static char s_user_text[512];
static char s_bot_text[512];

static notification_t *get_notification(void)
{
    return (notification_t *)app_main_get_notification();
}

/* ------------------------------------------------------------------ */
/* Console text injection (debug), unchanged from the previous version */
/* ------------------------------------------------------------------ */

int volc_cmd(const command_pkt *pParams, char *pcAck)
{
    (void)pcAck;
    notification_t *notification = get_notification();
    if (notification == NULL || notification->queue == NULL)
        return -1;

    if (strlen(pParams->p[0]) > 0) {
        log_debug("volc user: %s", pParams->p[0]);
        int handle = notification->queue->create("volc2ui_user");
        if (handle > 0)
            notification->queue->write(handle, (char *)pParams->p[0], (int)strlen(pParams->p[0]));
    }

    if (strlen(pParams->p[1]) > 0) {
        log_debug("volc bot: %s", pParams->p[1]);
        int handle1 = notification->queue->create("volc2ui_bot");
        if (handle1 > 0)
            notification->queue->write(handle1, (char *)pParams->p[1], (int)strlen(pParams->p[1]));
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* NV12 preview (pattern from camera/tablet_camera.c)                  */
/* ------------------------------------------------------------------ */

/* NV12 neutral black: Y=0, UV=128 (memset entire buffer to 0 tints green). */
static void volc_preview_fill_nv12_black(void *buf, uint32_t w, uint32_t h)
{
    size_t y_size = (size_t)w * (size_t)h;

    if (!buf || w == 0 || h == 0)
        return;
    memset(buf, 0, y_size);
    memset((uint8_t *)buf + y_size, 128, y_size / 2u);
}

static int volc_preview_buffer_ensure(context_t *ctx, uint32_t w, uint32_t h)
{
    uint32_t size = w * h * 3u / 2u;

    if (!ctx || w == 0 || h == 0)
        return -1;
    if (ctx->preview_buf == NULL) {
        ctx->preview_buf = bitmap_buffer_alloc(size);
        if (ctx->preview_buf == NULL)
            return -1;
    }
    volc_preview_fill_nv12_black(ctx->preview_buf, w, h);
    ctx->preview_w = w;
    ctx->preview_h = h;
    return 0;
}

static void volc_stream_unreg_locked(void)
{
    if (g_multimedia && g_multimedia->camera && s_client_handle != VOLC_INVALID_HANDLE)
        g_multimedia->camera->stream_unreg_client(s_client_handle);
    s_client_handle = VOLC_INVALID_HANDLE;
}

static void *volc_preview_reg_thread(void *arg)
{
    int epoch = (int)(intptr_t)arg;
    tablet_camera_streams_t streams;
    int idx = -1;
    int ret = -1;

    (void)thread_set_current_name("volc_preview");

    memset(&streams, 0, sizeof(streams));
    streams.idx_preview = -1;
    streams.idx_main = -1;
    if (g_multimedia)
        (void)tablet_camera_streams_resolve(g_multimedia, &streams);
    if (streams.idx_preview >= 0)
        idx = streams.idx_preview;
    else if (streams.idx_main >= 0)
        idx = streams.idx_main;

    pthread_mutex_lock(&s_stream_lock);
    if (epoch == s_preview_epoch && pContext && !pContext->closing &&
        g_multimedia && g_multimedia->camera && idx >= 0) {
        ret = g_multimedia->camera->stream_reg_client(idx, &s_client_handle,
#if CONFIG_IMAGE_WIDGET
                                                      volc_preview_frame_cb,
#else
                                                      NULL,
#endif
                                                      (void *)pContext);
        if (ret != 0 || s_client_handle == VOLC_INVALID_HANDLE) {
            log_warn("volc preview: stream_reg_client(%d) failed ret=%d", idx, ret);
            s_client_handle = VOLC_INVALID_HANDLE;
        } else {
            log_info("volc preview: registered stream=%d handle=0x%x", idx, s_client_handle);
        }
    }
    pthread_mutex_unlock(&s_stream_lock);
    return NULL;
}

#if CONFIG_IMAGE_WIDGET
static int volc_preview_frame_cb(int handle, media_frame_info_t *frame, void *user_data)
{
    (void)handle;
    context_t *ctx = (context_t *)user_data;

    if (frame == NULL || ctx == NULL || ctx->closing)
        return 0;

    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t nv_size = src_w * src_h * 3u / 2u;

    if ((frame->code_type != MEDIA_CODEC_VIDEO_NV12 && frame->code_type != 3) ||
        frame->data == NULL || src_w == 0 || src_h == 0 || frame->data_len != nv_size)
        return 0;

    pthread_mutex_lock(&s_preview_lock);
    if (ctx->closing || ctx->preview_buf == NULL) {
        pthread_mutex_unlock(&s_preview_lock);
        return 0;
    }
    if (ctx->preview_w != src_w || ctx->preview_h != src_h) {
        bitmap_buffer_free(ctx->preview_buf);
        ctx->preview_buf = bitmap_buffer_alloc(nv_size);
        if (ctx->preview_buf == NULL) {
            ctx->preview_w = 0;
            ctx->preview_h = 0;
            pthread_mutex_unlock(&s_preview_lock);
            return 0;
        }
        ctx->preview_w = src_w;
        ctx->preview_h = src_h;
    }
    memcpy(ctx->preview_buf, frame->data, nv_size);

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    image_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    image_dsc->header.cf     = LV_COLOR_FORMAT_NV12;
    image_dsc->header.w      = (uint16_t)src_w;
    image_dsc->header.h      = (uint16_t)src_h;
    image_dsc->header.stride = (uint16_t)src_w;
    image_dsc->data          = (const uint8_t *)ctx->preview_buf;
    image_dsc->data_size     = nv_size;
    pthread_mutex_unlock(&s_preview_lock);

    s_has_call_data = 1;
    return 0;
}
#endif

static void volc_preview_timer_cb(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);
#if CONFIG_IMAGE_WIDGET
    lv_image_dsc_t *image_dsc;
#endif

    if (ctx == NULL || ctx->closing || ctx->window == NULL)
        return;
    if (s_has_call_data == 0)
        return;
    s_has_call_data = 0;
#if CONFIG_IMAGE_WIDGET
    image_dsc = &(ctx->image_dsc);
    if (ctx->image && image_dsc->data && image_dsc->header.w > 0 && image_dsc->header.h > 0)
        lv_image_set_src(ctx->image, image_dsc);
#endif
}

/* ------------------------------------------------------------------ */
/* Status pill + captions + bottom bar                                 */
/* ------------------------------------------------------------------ */

static const char *volc_tr(const char *en, const char *zh)
{
    return (get_lan_index() == 1) ? zh : en;
}

static const char *volc_status_text(context_t *ctx)
{
    switch (ctx->state) {
        case VOLC_INIT_IN_PROGRESS:
            return volc_tr("Connecting", "连接中");
        case VOLC_INIT_NO_NETWORK:
            return volc_tr("Offline", "未联网");
        case VOLC_INIT_FAIL:
            return volc_tr("Connection failed", "连接失败");
        default:
            break;
    }
    if (volc_engine_bot_speaking())
        return volc_tr("Xiaokai is speaking", "小凯在说");
    if (ctx->ptt_active)
        return volc_tr("Speaking", "说话中");
    return volc_tr("Ready", "待说话");
}

static lv_obj_t *volc_create_float_pill(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *pill = lv_obj_create(parent);

    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(pill, w, h);
    lv_obj_set_style_bg_color(pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pill, VOLC_FLOAT_OPA, 0);
    lv_obj_set_style_radius(pill, h / 2, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    return pill;
}

/* 运行时绘制麦克风图标（LVGL 内置符号集无麦克风）：拾音头胶囊 + U 型支架 + 杆和底座。 */
static lv_obj_t *volc_create_mic_icon(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_t *part;

    if (box == NULL)
        return NULL;
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                            LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, GUI_SCALE(36), GUI_SCALE(40));

    /* 拾音头（胶囊） */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(part, GUI_SCALE(12), GUI_SCALE(20));
        lv_obj_align(part, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(part, GUI_SCALE(6), 0);
        lv_obj_set_style_bg_color(part, lv_color_hex(VOLC_COLOR_WHITE), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    }

    /* U 型支架（半圆弧，开口向上包住胶囊下半部） */
    part = lv_arc_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(part, GUI_SCALE(26), GUI_SCALE(26));
        lv_obj_align(part, LV_ALIGN_TOP_MID, 0, GUI_SCALE(6));
        lv_obj_set_style_arc_color(part, lv_color_hex(VOLC_COLOR_WHITE), LV_PART_MAIN);
        lv_obj_set_style_arc_width(part, GUI_SCALE(3), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(part, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(part, 0, LV_PART_INDICATOR);
        lv_arc_set_bg_angles(part, 0, 180);
    }

    /* 音柱杆 */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(part, GUI_SCALE(3), GUI_SCALE(5));
        lv_obj_align(part, LV_ALIGN_TOP_MID, 0, GUI_SCALE(32));
        lv_obj_set_style_bg_color(part, lv_color_hex(VOLC_COLOR_WHITE), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    }

    /* 底座 */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(part, GUI_SCALE(16), GUI_SCALE(3));
        lv_obj_align(part, LV_ALIGN_TOP_MID, 0, GUI_SCALE(37));
        lv_obj_set_style_bg_color(part, lv_color_hex(VOLC_COLOR_WHITE), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    }

    return box;
}

static void volc_ptt_visual(context_t *ctx, int active)
{
    if (ctx->btn_ptt == NULL)
        return;
    lv_obj_set_style_bg_color(ctx->btn_ptt, lv_color_hex(active ? VOLC_COLOR_BTN_ON : VOLC_COLOR_BTN_BG), 0);
    if (ctx->btn_ptt_label)
        lv_label_set_text(ctx->btn_ptt_label,
                          active ? volc_tr("Speaking", "说话中")
                                 : volc_tr("Hold to talk", "按住说话"));
}

/* 点击切换：按一下开始采样（说话中、变绿），再按一下结束并发送。 */
static void volc_ptt_toggle_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);

    if (ctx == NULL || ctx->closing)
        return;
    if (!ctx->ptt_active) {
        if (ctx->state != VOLC_INIT_OK) {
            show_toast_note(volc_tr("Connecting, please wait", "连接中，请稍候"), 1500, 1);
            return;
        }
        if (volc_engine_bot_speaking())
            show_toast_note(volc_tr("Xiaokai is speaking, please wait", "小凯在说，请稍后"), 1500, 0);

        ctx->ptt_active = 1;
        (void)volc_engine_set_voice_enable(1);
        log_info("volc ptt: start");
    } else {
        ctx->ptt_active = 0;
        (void)volc_engine_set_voice_enable(0);
        log_info("volc ptt: stop");
    }
    volc_ptt_visual(ctx, ctx->ptt_active);
}

/* 单一文本区两模式共用：只换位置/背景/字体，label 不重建。
 * 文本模式=左上角透明大区（豆包对话页），视频模式=底部半透明字幕浮条。 */
static void volc_chat_style_apply(context_t *ctx, int video)
{
    if (ctx->chat_panel == NULL)
        return;
    if (video) {
        lv_obj_set_size(ctx->chat_panel, LV_PCT(94), LV_SIZE_CONTENT);
        if (ctx->btn_ptt)
            lv_obj_align_to(ctx->chat_panel, ctx->btn_ptt, LV_ALIGN_OUT_TOP_MID, 0, -GUI_SCALE(12));
        else
            lv_obj_align(ctx->chat_panel, LV_ALIGN_BOTTOM_MID, 0,
                         -(TABLET_HOME_BAR_H + GUI_SCALE(104) + GUI_SCALE(12)));
        lv_obj_set_style_bg_color(ctx->chat_panel, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ctx->chat_panel, VOLC_FLOAT_OPA, 0);
        lv_obj_set_style_radius(ctx->chat_panel, GUI_SCALE(12), 0);
        lv_obj_set_style_pad_all(ctx->chat_panel, GUI_SCALE(8), 0);
        lv_obj_set_style_pad_gap(ctx->chat_panel, 2, 0);
    } else {
        lv_obj_set_size(ctx->chat_panel, LV_PCT(88), LV_PCT(56));
        lv_obj_align(ctx->chat_panel, LV_ALIGN_TOP_LEFT, GUI_SCALE(12), GUI_SCALE_VER(96));
        lv_obj_set_style_bg_opa(ctx->chat_panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(ctx->chat_panel, 0, 0);
        lv_obj_set_style_pad_all(ctx->chat_panel, 0, 0);
        lv_obj_set_style_pad_gap(ctx->chat_panel, GUI_SCALE(10), 0);
    }
    if (ctx->chat_user) {
        lv_obj_set_style_text_font(ctx->chat_user,
                                   tablet_font_get_freetype_font(video ? GUI_SCALE(20) : GUI_SCALE(16)), 0);
        lv_obj_set_style_text_color(ctx->chat_user, lv_color_hex(VOLC_COLOR_DIM), 0);
    }
    if (ctx->chat_bot) {
        lv_obj_set_style_text_font(ctx->chat_bot,
                                   tablet_font_get_freetype_font(video ? GUI_SCALE(20) : GUI_SCALE(22)), 0);
        lv_obj_set_style_text_color(ctx->chat_bot, lv_color_hex(VOLC_COLOR_WHITE), 0);
    }
}

/* 视频模式切换：显示通路（预览/对话区）与上行 H.264 一起切换，
 * 流注册跟随模式（epoch 失效旧注册线程，切视频再后台注册）。 */
static void volc_mode_apply(context_t *ctx)
{
    int video;
    int epoch;

    if (ctx == NULL || ctx->closing || ctx->window == NULL)
        return;
    video = ctx->video_mode;

    (void)volc_engine_set_video_enable(video);

#if CONFIG_IMAGE_WIDGET
    if (ctx->image) {
        if (video)
            lv_obj_remove_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    /* 对话区两模式共用一个 panel，仅切换位置/样式（修复双文本同显） */
    if (ctx->chat_panel) {
        lv_obj_remove_flag(ctx->chat_panel, LV_OBJ_FLAG_HIDDEN);
        volc_chat_style_apply(ctx, video);
    }
    if (ctx->btn_cam && ctx->btn_cam_label) {
        lv_obj_set_style_bg_color(ctx->btn_cam,
                                  lv_color_hex(video ? VOLC_COLOR_BTN_BG : 0x1C1C1E), 0);
        lv_obj_set_style_text_color(ctx->btn_cam_label,
                                    lv_color_hex(video ? VOLC_COLOR_WHITE : VOLC_COLOR_DIM), 0);
    }

    pthread_mutex_lock(&s_stream_lock);
    s_preview_epoch++;
    volc_stream_unreg_locked();
    epoch = s_preview_epoch;
    pthread_mutex_unlock(&s_stream_lock);

    if (!video)
        s_has_call_data = 0;

    if (video) {
        pthread_t tid;

        if (pthread_create(&tid, NULL, volc_preview_reg_thread, (void *)(intptr_t)epoch) == 0)
            pthread_detach(tid);
        else
            log_warn("volc preview register thread create failed");
    }
    lv_obj_invalidate(ctx->window);
}

static void volc_mode_toggle_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);

    if (ctx == NULL || ctx->closing)
        return;
    ctx->video_mode = ctx->video_mode ? 0 : 1;
    log_info("volc mode -> %s", ctx->video_mode ? "video" : "text");
    volc_mode_apply(ctx);
}

static void volc_end_session(context_t *ctx);
static void volc_request_end(context_t *ctx);

/* ------------------------------------------------------------------ */
/* UI timers                                                           */
/* ------------------------------------------------------------------ */

static void volc_ui_timer_cb(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);
    notification_t *notification;
    const char *status;

    if (ctx == NULL || ctx->closing || ctx->window == NULL)
        return;

    /* 若后台初始化失败，在UI线程只弹一次提示。 */
    if (!ctx->init_note_shown) {
        if (ctx->state == VOLC_INIT_NO_NETWORK) {
            ctx->init_note_shown = 1;
            show_msgbox_note(volc_tr("Device is offline. Connect to a network first.",
                                     "设备未联网，请先连接网络"));
        } else if (ctx->state == VOLC_INIT_FAIL) {
            ctx->init_note_shown = 1;
            show_msgbox_note(volc_tr("Volc engine init failed. Check the network and retry.",
                                     "Volc 引擎初始化失败，请确认网络后重试"));
        }
    }

    /* 状态胶囊只在文本变化时重绘 */
    status = volc_status_text(ctx);
    if (ctx->status_label && strcmp(lv_label_get_text(ctx->status_label), status) != 0)
        lv_label_set_text(ctx->status_label, status);

    /* 连接成功后按当前模式 re-apply 引擎画面开关（覆盖 init 复位竞态，一次性） */
    if (!ctx->mode_synced && ctx->state == VOLC_INIT_OK) {
        ctx->mode_synced = 1;
        (void)volc_engine_set_video_enable(ctx->video_mode);
    }

    /* 字幕：读到一条替换一行，无新消息不重绘 */
    notification = get_notification();
    if (notification == NULL || notification->queue == NULL)
        return;

    {
        int handle = notification->queue->create("volc2ui_user");
        long long msg_time;
        char buf[sizeof(s_user_text)] = {0};
        int size = (int)sizeof(buf) - 1;

        if (handle > 0 &&
            notification->queue->read(handle, buf, &size, &msg_time) == 0 &&
            strcmp(buf, s_user_text) != 0) {
            snprintf(s_user_text, sizeof(s_user_text), "%s", buf);
            if (ctx->chat_user)
                lv_label_set_text(ctx->chat_user, s_user_text);
        }
    }
    {
        int handle = notification->queue->create("volc2ui_bot");
        long long msg_time;
        char buf[sizeof(s_bot_text)] = {0};
        int size = (int)sizeof(buf) - 1;

        if (handle > 0 &&
            notification->queue->read(handle, buf, &size, &msg_time) == 0 &&
            strcmp(buf, s_bot_text) != 0) {
            snprintf(s_bot_text, sizeof(s_bot_text), "%s", buf);
            if (ctx->chat_bot)
                lv_label_set_text(ctx->chat_bot, s_bot_text);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define VOLC_PTT_SZ         GUI_SCALE(104)

/* 对话区：两模式共用（位置/字体由 volc_chat_style_apply 按模式设置）。 */
static void create_chat_panel(context_t *ctx, lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);

    lv_obj_remove_style_all(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    ctx->chat_panel = panel;

    ctx->chat_user = lv_label_create(panel);
    if (ctx->chat_user) {
        lv_obj_set_style_text_color(ctx->chat_user, lv_color_hex(VOLC_COLOR_DIM), 0);
        lv_obj_set_width(ctx->chat_user, LV_PCT(100));
        lv_label_set_long_mode(ctx->chat_user, LV_LABEL_LONG_WRAP);
        lv_label_set_text(ctx->chat_user, "");
    }

    ctx->chat_bot = lv_label_create(panel);
    if (ctx->chat_bot) {
        lv_obj_set_style_text_color(ctx->chat_bot, lv_color_hex(VOLC_COLOR_WHITE), 0);
        lv_obj_set_width(ctx->chat_bot, LV_PCT(100));
        lv_label_set_long_mode(ctx->chat_bot, LV_LABEL_LONG_WRAP);
        lv_label_set_text(ctx->chat_bot,
                          volc_tr("Hi, I'm Xiaokai\nHold the button below and talk to me",
                                  "你好呀，我是小凯\n按住下方按钮和我说话"));
    }
}

/* 说话圆钮：独立浮层（无底栏容器），mic 图标+文字，点击切换开始/结束。 */
static void create_ptt_button(context_t *ctx, lv_obj_t *parent)
{
    ctx->btn_ptt = lv_button_create(parent);
    if (ctx->btn_ptt) {
        lv_obj_remove_style_all(ctx->btn_ptt);
        lv_obj_remove_flag(ctx->btn_ptt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_set_size(ctx->btn_ptt, VOLC_PTT_SZ, VOLC_PTT_SZ);
        /* Reserve the bottom strip for the launcher home indicator. */
        lv_obj_align(ctx->btn_ptt, LV_ALIGN_BOTTOM_MID, 0, -(TABLET_HOME_BAR_H + GUI_SCALE_VER(12)));
        lv_obj_set_style_bg_color(ctx->btn_ptt, lv_color_hex(VOLC_COLOR_BTN_BG), 0);
        lv_obj_set_style_bg_opa(ctx->btn_ptt, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ctx->btn_ptt, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(ctx->btn_ptt, 0, 0);
        lv_obj_set_layout(ctx->btn_ptt, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(ctx->btn_ptt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ctx->btn_ptt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ctx->btn_ptt, 0, 0);
        lv_obj_add_event_cb(ctx->btn_ptt, volc_ptt_toggle_cb, LV_EVENT_CLICKED, (void *)ctx);

        ctx->btn_ptt_icon = volc_create_mic_icon(ctx->btn_ptt);

        ctx->btn_ptt_label = lv_label_create(ctx->btn_ptt);
        if (ctx->btn_ptt_label) {
            lv_font_t *text = tablet_font_get_freetype_font(GUI_SCALE(20));

            if (text == NULL)
                text = tablet_font_get_freetype_font(20);
            lv_obj_set_style_text_font(ctx->btn_ptt_label, text, 0);
            lv_obj_set_style_text_color(ctx->btn_ptt_label, lv_color_hex(VOLC_COLOR_WHITE), 0);
            lv_label_set_text(ctx->btn_ptt_label, volc_tr("Hold to talk", "按住说话"));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Engine init thread + RTC preload (unchanged from previous version)  */
/* ------------------------------------------------------------------ */

static int volc_dlopen_one(const char *const *candidates, unsigned n, const char *what)
{
    unsigned i;
    const char *err = NULL;

    for (i = 0; i < n; i++) {
        (void)dlerror();
        void *no = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (no) {
            log_info("volc: %s already loaded (%s)", what, candidates[i]);
            dlclose(no);
            return 0;
        }
        (void)dlerror();
        void *h = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            log_info("volc: dlopen(%s) ok (%s)", candidates[i], what);
            return 0;
        }
        err = dlerror();
        log_warn("volc: dlopen(%s) failed: %s", candidates[i], err ? err : "(null)");
    }
    return -1;
}

/* Load RTC libraries after the UI is up. */
static void volc_engine_preload_svc_so(void)
{
    static const char *rtc_candidates[] = {
        "libVolcEngineRTCLite.so",
        "/usr/lib/libVolcEngineRTCLite.so",
        "/mnt/install/lib/libVolcEngineRTCLite.so",
        "/mnt/install/apps/volc_engine/lib/libVolcEngineRTCLite.so",
    };
    /* SD (/mnt/install/lib) 优先：引擎库可不重烧 usr.sqsh4 直接从卡更新 */
    static const char *svc_candidates[] = {
        "/mnt/install/lib/libvolc_engine_svc.so",
        "libvolc_engine_svc.so",
        "/usr/lib/libvolc_engine_svc.so",
    };

    (void)dlerror();
    if (volc_dlopen_one(rtc_candidates,
                        sizeof(rtc_candidates) / sizeof(rtc_candidates[0]),
                        "VolcEngineRTCLite") != 0)
        log_warn("volc: libVolcEngineRTCLite.so not loaded; svc may fail to relocate");

    if (volc_dlopen_one(svc_candidates,
                        sizeof(svc_candidates) / sizeof(svc_candidates[0]),
                        "volc_engine_svc") != 0)
        log_warn("volc: libvolc_engine_svc.so not loaded; volc_engine_init may fail");
}

static int volc_ipv4_usable(const char *ip)
{
    return ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

static int volc_network_ipv4_ready(network_t *net, char *ip_out, size_t ip_sz)
{
    if (!net || !ip_out || ip_sz == 0)
        return 0;

    ip_out[0] = '\0';

    if (net->wifi && net->wifi->station) {
        network_wifi_station_state_e state;
        if (net->wifi->station->get_state(&state) == NETWORK_OK) {
            if (state == NETWORK_WIFI_STATION_STATE_CONNECTED &&
                net->wifi->station->get_ip_address(ip_out, ip_sz) == NETWORK_OK &&
                volc_ipv4_usable(ip_out)) {
                return 1;
            }
        }
    }

    if (net->ethernet) {
        if (net->ethernet->get_ip &&
            net->ethernet->get_ip(ip_out) == NETWORK_OK &&
            volc_ipv4_usable(ip_out)) {
            return 1;
        }
        if (net->ethernet->get_ip_by_name) {
            static const char *ifaces[] = {"wlan0", "eth0", "eth1", "net0", "enp0s0"};
            for (unsigned i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
                memset(ip_out, 0, ip_sz);
                if (net->ethernet->get_ip_by_name(ifaces[i], ip_out, ip_sz) == NETWORK_OK &&
                    volc_ipv4_usable(ip_out)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int volc_wait_network_ready(int timeout_sec)
{
#if CONFIG_NETWORK_SUPPORTED
    const int step_ms = 200;
    int tries = (timeout_sec > 0) ? (timeout_sec * 1000 / step_ms) : 0;

    for (int i = 0; i < tries; i++) {
        network_t *network = (network_t *)app_main_get_network();
        char ip[64] = {0};
        if (volc_network_ipv4_ready(network, ip, sizeof(ip))) {
            log_info("volc: network ready ip=%s (wait=%dms)", ip, i * step_ms);
            return 0;
        }
        if (i == 0 || (i % 5) == 0) {
            network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
            if (network && network->wifi && network->wifi->station)
                (void)network->wifi->station->get_state(&state);
            log_info("volc: waiting network state=%d try=%d/%d", (int)state, i + 1, tries);
        }
        usleep((useconds_t)step_ms * 1000);
    }
    log_warn("volc: network not ready after %ds", timeout_sec);
    return -1;
#else
    (void)timeout_sec;
    return 0;
#endif
}

static void *volc_engine_init_thread(void *arg)
{
    context_t *ctx = (context_t *)arg;
    if (!ctx)
        return NULL;

    ctx->state = VOLC_INIT_IN_PROGRESS;
    volc_engine_preload_svc_so();

    if (volc_wait_network_ready(5) != 0) {
        log_warn("volc_engine_init_thread: network not ready, skip init");
        ctx->state = VOLC_INIT_NO_NETWORK;
        return NULL;
    }

    int ret = volc_engine_init();
    if (ret == VOLC_ENGINE_ERR_NETWORK) {
        ctx->state = VOLC_INIT_NO_NETWORK;
    } else {
        ctx->state = (ret == 0) ? VOLC_INIT_OK : VOLC_INIT_FAIL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Exit path                                                           */
/* ------------------------------------------------------------------ */

static void volc_end_session(context_t *ctx)
{
    lv_display_t *disp;
    lv_obj_t *screen;

    if (ctx == NULL || !ctx->closing)
        return;
    ctx->closing = 1;
    ctx->ptt_active = 0;
    (void)volc_engine_set_voice_enable(0);

    if (ctx->timer) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
    if (ctx->ui_timer) {
        lv_timer_delete(ctx->ui_timer);
        ctx->ui_timer = NULL;
    }

    /* 先露出桌面再收流，与相机页退出顺序一致 */
    disp = lv_display_get_default();
    screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (screen)
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    if (ctx->window)
        lv_obj_add_flag(ctx->window, LV_OBJ_FLAG_HIDDEN);
    if (ctx->main) {
        lv_obj_invalidate(ctx->main);
        lv_obj_remove_flag(ctx->main, LV_OBJ_FLAG_HIDDEN);
    }
    if (disp)
        lv_refr_now(disp);

    /* Home is visible. Join init before deinit. */
    if (ctx->init_join_pending) {
        (void)pthread_join(ctx->init_tid, NULL);
        ctx->init_join_pending = 0;
    }
    (void)volc_engine_deinit();

    /* 注销预览客户端并释放缓冲；frame 回调以 closing 门控 */
    pthread_mutex_lock(&s_stream_lock);
    s_preview_epoch++;
    volc_stream_unreg_locked();
    pthread_mutex_unlock(&s_stream_lock);

    pthread_mutex_lock(&s_preview_lock);
    if (ctx->preview_buf) {
        bitmap_buffer_free(ctx->preview_buf);
        ctx->preview_buf = NULL;
    }
    pthread_mutex_unlock(&s_preview_lock);

    if (ctx->window) {
        lv_obj_delete(ctx->window);
        ctx->window = NULL;
    }

    memset((void *)ctx, 0, sizeof(context_t));
}

/* One-shot timer runs the real exit outside the button event, so the window
 * (and the button inside it) is never deleted while its event is dispatching. */
static void volc_end_defer_cb(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);

    lv_timer_del(timer);
    volc_end_session(ctx);
}

static void volc_request_end(context_t *ctx)
{
    if (ctx == NULL || ctx->closing)
        return;
    ctx->closing = 1; /* gate stream callbacks / interactions immediately */
    (void)lv_timer_create(volc_end_defer_cb, 0, (void *)ctx);
}

static void event_handler_btn_back(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);

    if (ctx == NULL) {
        log_warn("volc btn_back ctx is null");
        return;
    }
    volc_request_end(ctx);
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int tablet_volc_init(void)
{
    static int s_console_registered;

    if (s_console_registered)
        return 0;
    /* Resolve console_cmd_register at runtime via dlsym so that the SO can be
     * dlopen'd even when console.o was not extracted into the main binary
     * (happens when no other code in the main binary directly calls it).      */
    typedef int (*console_cmd_register_fn_t)(const char *, FN_COMMAND, const char *);
    console_cmd_register_fn_t reg_fn =
        (console_cmd_register_fn_t)dlsym(RTLD_DEFAULT, "console_cmd_register");
    if (reg_fn)
        (void)reg_fn("ak-volc-cmd", volc_cmd, "volc inject user/bot text to UI queues");
    else
        log_warn("console_cmd_register not found, ak-volc-cmd registration skipped");
    s_console_registered = 1;
    return 0;
}

lv_result_t tablet_volc_create(lv_obj_t *main)
{
    context_t *ctx = pContext;
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *window = NULL;
    lv_font_t *font;

    memset(ctx, 0, sizeof(context_t));
    ctx->main = main;
    ctx->video_mode = 0; /* 默认文本对话模式 */
    g_multimedia = (multimedia_t *)app_main_get_multimedia();

    disp = lv_display_get_default();
    screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (!screen) {
        LV_LOG_ERROR("no active screen, volc create failed");
        lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);
        return LV_RESULT_INVALID;
    }

    /* 异步启动：不要阻塞界面绘制（HTTP + 音视频初始化都在 volc_engine_init 里）。 */
    ctx->state = VOLC_INIT_IN_PROGRESS;

    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, LV_PART_MAIN);

    window = lv_obj_create(screen);
    if (window == NULL) {
        LV_LOG_ERROR("failed to create window");
        goto ERROR;
    }
    ctx->window = window;
    lv_obj_remove_style_all(window);
    lv_obj_remove_flag(window, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_bg_color(window, lv_color_hex(VOLC_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(window, LV_HOR_RES, LV_VER_RES);

#if CONFIG_IMAGE_WIDGET
    /* 预览缓冲按 video_cfg.json 的 preview 尺寸申请，解析失败退化为屏大小 */
    {
        tablet_camera_streams_t streams;
        uint32_t w = (uint32_t)LV_HOR_RES;
        uint32_t h = (uint32_t)LV_VER_RES;

        memset(&streams, 0, sizeof(streams));
        streams.idx_preview = -1;
        streams.idx_main = -1;
        if (g_multimedia && tablet_camera_streams_resolve(g_multimedia, &streams) == 0 &&
            streams.preview_w > 0 && streams.preview_h > 0) {
            w = (uint32_t)streams.preview_w;
            h = (uint32_t)streams.preview_h;
        }
        if (volc_preview_buffer_ensure(ctx, w, h) != 0) {
            LV_LOG_ERROR("volc preview buffer alloc failed");
            goto ERROR;
        }

        memset(&ctx->image_dsc, 0, sizeof(ctx->image_dsc));
        ctx->image_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        ctx->image_dsc.header.cf     = LV_COLOR_FORMAT_NV12;
        ctx->image_dsc.header.w      = (uint16_t)w;
        ctx->image_dsc.header.h      = (uint16_t)h;
        ctx->image_dsc.header.stride = (uint16_t)w;
        ctx->image_dsc.data          = (const uint8_t *)ctx->preview_buf;
        ctx->image_dsc.data_size     = w * h * 3u / 2u;

        ctx->image = lv_image_create(window);
        if (ctx->image == NULL)
            goto ERROR;
        lv_obj_remove_style_all(ctx->image);
        lv_obj_remove_flag(ctx->image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_set_size(ctx->image, LV_PCT(100), LV_PCT(100));
        lv_obj_center(ctx->image);
        lv_image_set_src(ctx->image, &ctx->image_dsc);
        /* 默认文本模式：预览隐藏，流注册跟随视频模式（volc_mode_apply） */
        lv_obj_add_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    if (tablet_create_top(window, volc_tr("Xiaokai", "小凯"), event_handler_btn_back, (void *)ctx) == NULL) {
        LV_LOG_ERROR("failed to create top");
        goto ERROR;
    }

    /* 状态胶囊：顶栏下方左侧（顶栏右上是返回键、中间是标题） */
    {
        lv_obj_t *pill = volc_create_float_pill(window, GUI_SCALE(96), GUI_SCALE_VER(36));

        if (pill) {
            font = tablet_font_get_freetype_font(GUI_SCALE(20));
            if (font == NULL)
                font = tablet_font_get_freetype_font(20);
            lv_obj_align(pill, LV_ALIGN_TOP_LEFT, GUI_SCALE(12), GUI_SCALE_VER(52));
            ctx->status_label = lv_label_create(pill);
            if (ctx->status_label) {
                lv_obj_set_style_text_font(ctx->status_label, font, 0);
                lv_obj_set_style_text_color(ctx->status_label,
                                            lv_color_hex(VOLC_COLOR_WHITE), 0);
                lv_obj_center(ctx->status_label);
                lv_label_set_text(ctx->status_label, volc_tr("Connecting", "连接中"));
            }
        }
    }

    /* 视频切换键：右上角（顶栏下方，与左侧状态胶囊对称），高亮=视频模式 */
    {
        lv_font_t *font;
        ctx->btn_cam = lv_button_create(window);

        if (ctx->btn_cam) {
            lv_obj_remove_style_all(ctx->btn_cam);
            lv_obj_remove_flag(ctx->btn_cam, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_set_size(ctx->btn_cam, GUI_SCALE(104), GUI_SCALE_VER(36));
            lv_obj_align(ctx->btn_cam, LV_ALIGN_TOP_RIGHT, -GUI_SCALE(12), GUI_SCALE_VER(52));
            lv_obj_set_style_bg_color(ctx->btn_cam, lv_color_hex(0x1C1C1E), 0);
            lv_obj_set_style_bg_opa(ctx->btn_cam, VOLC_FLOAT_OPA, 0);
            lv_obj_set_style_radius(ctx->btn_cam, GUI_SCALE_VER(18), 0);
            lv_obj_add_event_cb(ctx->btn_cam, volc_mode_toggle_cb, LV_EVENT_CLICKED, (void *)ctx);

            font = tablet_font_get_freetype_font(GUI_SCALE(20));
            if (font == NULL)
                font = tablet_font_get_freetype_font(20);
            ctx->btn_cam_label = lv_label_create(ctx->btn_cam);
            if (ctx->btn_cam_label) {
                lv_obj_set_style_text_font(ctx->btn_cam_label, font, 0);
                lv_obj_set_style_text_color(ctx->btn_cam_label, lv_color_hex(VOLC_COLOR_DIM), 0);
                lv_obj_center(ctx->btn_cam_label);
                lv_label_set_text(ctx->btn_cam_label,
                                  volc_tr(LV_SYMBOL_VIDEO " Video", LV_SYMBOL_VIDEO " 视频"));
            }
        }
    }

    create_chat_panel(ctx, window);
    create_ptt_button(ctx, window);

    /* 右下角署名：小字灰色，常驻（Home 条上方） */
    {
        lv_obj_t *brand = lv_label_create(window);

        if (brand) {
            font = tablet_font_get_freetype_font(GUI_SCALE(12));
            if (font == NULL)
                font = tablet_font_get_freetype_font(12);
            lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_text_font(brand, font, 0);
            lv_obj_set_style_text_color(brand, lv_color_hex(VOLC_COLOR_DIM), 0);
            lv_label_set_text(brand, volc_tr("powered by Volcengine", "power by 火山引擎"));
            lv_obj_align(brand, LV_ALIGN_BOTTOM_RIGHT,
                         -GUI_SCALE(12), -(TABLET_HOME_BAR_H + GUI_SCALE(6)));
        }
    }

    ctx->timer = lv_timer_create(volc_preview_timer_cb, VOLC_PREVIEW_TIMER_MS, (void *)ctx);
    ctx->ui_timer = lv_timer_create(volc_ui_timer_cb, VOLC_UI_TIMER_MS, (void *)ctx);
    if (ctx->timer == NULL || ctx->ui_timer == NULL)
        goto ERROR;
    lv_timer_ready(ctx->ui_timer);

    /* 应用默认模式（文本）：隐藏预览、对话区排左上角、上行画面关、不注册预览流 */
    volc_mode_apply(ctx);
    lv_obj_invalidate(window);
    lv_refr_now(lv_display_get_default());

    /* RTC/HTTP init in background thread (joinable: joined on back to avoid deinit racing init). */
    if (pthread_create(&ctx->init_tid, NULL, volc_engine_init_thread, (void *)ctx) == 0) {
        ctx->init_join_pending = 1;
    } else {
        log_error("volc_engine_init_thread pthread_create failed");
        ctx->state = VOLC_INIT_FAIL;
        ctx->init_join_pending = 0;
    }

    return LV_RESULT_OK;

ERROR:
    show_msgbox_note("Volc Engine start failed");

    if (screen)
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);

    if (ctx->timer) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
    if (ctx->ui_timer) {
        lv_timer_delete(ctx->ui_timer);
        ctx->ui_timer = NULL;
    }
    pthread_mutex_lock(&s_stream_lock);
    s_preview_epoch++;
    volc_stream_unreg_locked();
    pthread_mutex_unlock(&s_stream_lock);
    pthread_mutex_lock(&s_preview_lock);
    if (ctx->preview_buf) {
        bitmap_buffer_free(ctx->preview_buf);
        ctx->preview_buf = NULL;
    }
    pthread_mutex_unlock(&s_preview_lock);

    if (window)
        lv_obj_delete(window);

    memset((void *)ctx, 0, sizeof(context_t));
    return LV_RESULT_INVALID;
}
