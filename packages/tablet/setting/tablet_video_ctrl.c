// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Video control widget for camera preview window implementation.
 *
 * QR/settings preview: preview stream NV12 (LCD_HOR x LCD_VER, see app_globle_config.h),
 * stream_reg_client(1) + broadcast thread callback memcpy's NV12 into a stable buffer,
 * GUI timer only lv_image_set_src (no RGB888 conversion).
 */
#define LOG_TAG "TABLET_VIDEO_CTRL"

/*
 * QR 扫码依赖第三方动态库 /usr/lib/libzxing.so 提供的 zxing_scan_qrcode。
 * 这里不再提供 weak stub：否则链接器会优先使用本地定义，导致永远返回 -1（表现为一直识别不到）。
 */

/*********************
 *      INCLUDES
 *********************/
/*
 * 注意：
 * `LV_DEMO_CAMERA` 是“相机 Demo 应用”的开关，但设置/二维码配网同样需要相机预览。
 * 因此 `tablet_video_ctrl` 的 NV12 预览实现不应被 `LV_DEMO_CAMERA` 裁掉。
 *
 * 如需在极小裁剪固件里去掉预览，可通过编译宏 -DTABLET_VIDEO_CTRL_ENABLE_PREVIEW=0 关闭。
 */
#ifndef TABLET_VIDEO_CTRL_ENABLE_PREVIEW
#define TABLET_VIDEO_CTRL_ENABLE_PREVIEW  1
#endif

#if TABLET_VIDEO_CTRL_ENABLE_PREVIEW
#include "tablet_camera.h"
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr.h"
#include "src/misc/lv_types.h"
#include "app_main.h"
#include "multimedia.h"
#include "system_time.h"
#include "bitmap.h"

/* Set while closing NV12 preview: broadcast thread must not touch image_buffer */
static volatile int s_nv12_preview_closing;

/*
 * `video_device.h` 在部分工程/裁剪配置下不存在（仅 IPC camera app 使用）。
 * 本文件的 NV12 预览路径不依赖 video_device；仅历史路径 close 时会调用 video_device_stop。
 * 因此这里做可选声明，避免 setting/qr 预览因缺头文件而无法编译。
 */
#if defined(__has_include)
#  if __has_include("video_device.h")
#    include "video_device.h"
#    define TABLET_VIDEO_CTRL_HAS_VIDEO_DEVICE 1
#  else
#    define TABLET_VIDEO_CTRL_HAS_VIDEO_DEVICE 0
#  endif
#else
#  define TABLET_VIDEO_CTRL_HAS_VIDEO_DEVICE 0
#endif

#if !TABLET_VIDEO_CTRL_HAS_VIDEO_DEVICE
static inline void video_device_stop(int h) { (void)h; }
#endif
#include "log.h"
#include "app_globle_config.h"

#include "tablet_common.h"
#include "tablet_font.h"
#include "dlfcn.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
/*********************
 *      DEFINES
 *********************/

#define INVALID_HANDLE              (-1)
#define CONFIG_IMAGE_WIDGET         1

/* Stream 1 preview: match panel (ST7262 800x480 on ActionCam) */
#define PREVIEW_NV12_W   LCD_HOR_RES_MAX
#define PREVIEW_NV12_H   LCD_VER_RES_MAX

/**********************
 *      TYPEDEFS
 **********************/
enum play_status {
    LV_DEMO_PLAY_STATUS_UNREADY    = 0,
    LV_DEMO_PLAY_STATUS_STOP,
    LV_DEMO_PLAY_STATUS_PAUSE,
    LV_DEMO_PLAY_STATUS_PLAYING,
    LV_DEMO_PLAY_STATUS_CLOSE,
    LV_DEMO_PLAY_STATUS_MAX
};

typedef struct {
    lv_obj_t *main;
    lv_obj_t *image;
    lv_image_dsc_t image_dsc;
    void *image_buffer;

    lv_timer_t *timer;
    int state;
    int VideoHandle;
    int stream_id;           /* resolved preview stream index */
    int reg_client_handle;
    uint32_t nv12_w;         /* expected NV12 width from video_cfg preview channel */
    uint32_t nv12_h;
    int af_triggered;
    nova_af_focus_ui_t af_focus_ui;
} context_t;


/**********************
 *  EXTERNAL FUNCTIONS
 **********************/
static int client_handle = 0;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void timing_task_timer(lv_timer_t *timer);
static int get_stream_callback(int nHandle, media_frame_info_t *pframe, void *pDataCbUser);
/** Same NV12 path as tablet_camera.c apply_frame_to_ctx + callback */
static int nv12_stream1_preview_callback(int handle, media_frame_info_t *frame, void *user_data);
static void video_ctrl_preview_click_cb(lv_event_t *e);

/**********************
 *  STATIC VARIABLES
 **********************/

static context_t context = {
    .VideoHandle = -1,
    .reg_client_handle = -1,
};
static context_t *pContext = &context;

static multimedia_t *video_stream = NULL;

static volatile int has_call_data = 0;
static volatile int has_nv12_frame = 0;
static unsigned s_nv12_cb_log_cnt = 0;
static pthread_mutex_t s_nv12_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_nv12_seq;

/**
 * Apply one camera NV12/NV21 frame into ctx->image_buffer (semiplane).
 * Update ctx->image_dsc.header.cf/w/h each time to match frame code_type.
 */
static int apply_nv12_frame_to_preview(context_t *ctx, media_frame_info_t *frame)
{
    if (frame == NULL || ctx == NULL || ctx->image_buffer == NULL)
        return 0;

    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t nv_size = src_w * src_h * 3u / 2u;

    if ((frame->code_type != MEDIA_CODEC_VIDEO_NV12 && frame->code_type != 3) ||
        frame->data == NULL || frame->data_len < nv_size) {
        static unsigned s_skip_cnt;
        if ((s_skip_cnt % 30u) == 0u)
            log_warn("[DBG_QR_PREVIEW] skip frame code_type=%d len=%u need>=%u w=%u h=%u (cnt=%u)",
                     frame->code_type, (unsigned)frame->data_len, (unsigned)nv_size,
                     (unsigned)src_w, (unsigned)src_h, s_skip_cnt);
        s_skip_cnt++;
        return 0;
    }

    /* QR/preview stream pixel layout is NV12 (Y + interleaved UV). */
    lv_color_format_t cf = LV_COLOR_FORMAT_NV12;

    pthread_mutex_lock(&s_nv12_lock);

    /* Realloc preview buffer if resolution changed */
    if (ctx->nv12_w != src_w || ctx->nv12_h != src_h) {
        bitmap_buffer_free(ctx->image_buffer);
        ctx->image_buffer = bitmap_buffer_alloc(nv_size);
        if (ctx->image_buffer == NULL) {
            pthread_mutex_unlock(&s_nv12_lock);
            return 0;
        }
        ctx->nv12_w = src_w;
        ctx->nv12_h = src_h;
    }

    memcpy(ctx->image_buffer, frame->data, nv_size);

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    image_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    image_dsc->header.cf     = cf;
    image_dsc->header.w      = (uint16_t)src_w;
    image_dsc->header.h      = (uint16_t)src_h;
    /* LVGL stride for semiplanar formats is bytes-per-row of Y plane (not total bytes/row for Y+UV). */
    image_dsc->header.stride = (uint16_t)src_w;
    image_dsc->data          = (const uint8_t *)ctx->image_buffer;
    image_dsc->data_size     = nv_size;

    s_nv12_seq++;
    pthread_mutex_unlock(&s_nv12_lock);

    return 1;
}

int tablet_video_ctrl_copy_latest_nv12(uint8_t *dst, size_t dst_len,
                                  int *out_w, int *out_h, uint32_t *inout_seq)
{
    if (!dst || dst_len == 0 || !inout_seq)
        return -1;

    context_t *ctx = pContext;
    if (!ctx || !ctx->image_buffer || ctx->nv12_w == 0 || ctx->nv12_h == 0)
        return -1;

    pthread_mutex_lock(&s_nv12_lock);
    uint32_t cur = s_nv12_seq;
    if (cur == *inout_seq) {
        pthread_mutex_unlock(&s_nv12_lock);
        return 0;
    }

    size_t need = (size_t)ctx->nv12_w * (size_t)ctx->nv12_h * 3u / 2u;
    if (need > dst_len) {
        pthread_mutex_unlock(&s_nv12_lock);
        return -1;
    }
    memcpy(dst, ctx->image_buffer, need);
    *inout_seq = cur;
    if (out_w) *out_w = (int)ctx->nv12_w;
    if (out_h) *out_h = (int)ctx->nv12_h;
    pthread_mutex_unlock(&s_nv12_lock);
    return 1;
}

static void video_ctrl_preview_click_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);

    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !ctx) {
        return;
    }
    if (ctx->state != LV_DEMO_PLAY_STATUS_PLAYING) {
        return;
    }
    if (!ctx->image || !lv_obj_is_valid(ctx->image)) {
        return;
    }
    (void)nova_af_handle_preview_tap(video_stream ? video_stream : app_main_get_multimedia(),
                                     ctx->image, &ctx->af_focus_ui);
}

static int nv12_stream1_preview_callback(int handle, media_frame_info_t *frame, void *user_data)
{
    (void)handle;
    if (s_nv12_preview_closing)
        return 0;
    if (frame == NULL || user_data == NULL)
        return -1;
    context_t *ctx = (context_t *)user_data;
    if (s_nv12_cb_log_cnt < 5) {
        s_nv12_cb_log_cnt++;
        log_info("[DBG_QR_PREVIEW] cb#%u code_type=%d frame=%ux%u len=%u expect=%ux%u buf=%p",
                 s_nv12_cb_log_cnt, frame->code_type,
                 (unsigned)frame->width, (unsigned)frame->height,
                 (unsigned)frame->data_len,
                 (unsigned)ctx->nv12_w, (unsigned)ctx->nv12_h, ctx->image_buffer);
    }
    if (apply_nv12_frame_to_preview(ctx, frame)) {
        has_nv12_frame = 1;
        if (!ctx->af_triggered) {
            multimedia_t *mm = video_stream ? video_stream :
                               (multimedia_t *)app_main_get_multimedia();
            if (nova_af_trigger_center_once(mm, &ctx->af_triggered) == 0) {
                log_info("video_ctrl: trigger AF once after first preview frame");
            }
        }
    }
    return 0;
}

static int get_stream_callback(int nHandle, media_frame_info_t *pframe, void* pDataCbUser)
{
    if (pframe == NULL)
        return -1;
    if (video_stream == NULL) {
        log_debug("video_stream null, videotime=%llu", (unsigned long long)pframe->timestamp);
        return -1;
    }

    context_t *ctx = (context_t *)(pDataCbUser);
    if (ctx == NULL)
        return -1;

    if (NULL == ctx->image_buffer) {
        LV_LOG_ERROR("image buffer is null, please check\n");
        return -1;
    }

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);

    if (0 == image_dsc->header.w) {
        uint32_t width = lv_obj_get_content_width(ctx->image);
        uint32_t height = lv_obj_get_content_height(ctx->image);
        if (width == 0 || height == 0) {
            width = lv_obj_get_content_width(ctx->main);
            height = lv_obj_get_content_height(ctx->main);
        }
        if (width == 0 || height == 0)
            width = 640, height = 384;
        image_dsc->header.w = width;
        image_dsc->header.h = height;
        image_dsc->header.stride = (uint16_t)image_dsc->header.w;
        image_dsc->header.cf = LV_COLOR_FORMAT_NV12;
        image_dsc->data = ctx->image_buffer;
        image_dsc->data_size = width * height * 3u / 2u;
    }

    has_call_data = 1;
    return 0;
}

/**
 * @brief   create camera window.
 */
lv_result_t tablet_video_ctrl_create(lv_obj_t * main)
{
    context_t *ctx = pContext;
    void *pv = pContext->image_buffer;
    memset(ctx, 0, sizeof(context_t));
    pContext->image_buffer = pv;
    ctx->main = main;
    ctx->VideoHandle = -1;
    ctx->reg_client_handle = -1;
    ctx->stream_id = 0;

    lv_obj_t * image = lv_image_create(main);
    lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
    lv_obj_center(image);
    ctx->image = image;

    video_stream = (multimedia_t *)app_main_get_multimedia();
    ctx->state = LV_DEMO_PLAY_STATUS_PLAYING;
    if (video_stream && video_stream->camera && video_stream->camera->stream_add_callback)
        video_stream->camera->stream_add_callback(0,&client_handle,get_stream_callback,(void *)ctx);
    ctx->timer = lv_timer_create(timing_task_timer,1000/15, (void *)ctx);
    lv_timer_ready(ctx->timer);

    return LV_RESULT_OK;
}

/**
 * @brief QR / settings: same as tablet_camera — stream 1 NV12 -> RGB in broadcast callback.
 */
lv_result_t tablet_video_ctrl_create_for_stream(lv_obj_t *main, int stream_id)
{
    (void)stream_id;
    log_info("video_ctrl_create_for_stream enter: enable_preview=%d", (int)TABLET_VIDEO_CTRL_ENABLE_PREVIEW);
    s_nv12_preview_closing = 0;
    has_nv12_frame = 0;
    has_call_data = 0;
    s_nv12_cb_log_cnt = 0;
    context_t *ctx = pContext;
    void *pv = pContext->image_buffer;
    memset(ctx, 0, sizeof(context_t));
    pContext->image_buffer = pv;
    ctx->main = main;
    ctx->VideoHandle = -1;
    ctx->reg_client_handle = -1;
    ctx->nv12_w = (uint32_t)PREVIEW_NV12_W;
    ctx->nv12_h = (uint32_t)PREVIEW_NV12_H;
    ctx->stream_id = 1;

    lv_obj_t *image = lv_image_create(main);
    lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
    lv_obj_center(image);
    lv_obj_set_style_border_width(image, 0, 0);
    lv_obj_set_style_outline_width(image, 0, 0);
    lv_obj_set_style_shadow_width(image, 0, 0);
    ctx->image = image;

    video_stream = (multimedia_t *)app_main_get_multimedia();
    if (video_stream) {
        tablet_camera_streams_t cs;
        memset(&cs, 0, sizeof(cs));
        cs.idx_preview = -1;
        if (tablet_camera_streams_resolve(video_stream, &cs) == 0) {
            if (cs.preview_w > 0 && cs.preview_h > 0) {
                ctx->nv12_w = (uint32_t)cs.preview_w;
                ctx->nv12_h = (uint32_t)cs.preview_h;
            }
            if (cs.idx_preview >= 0)
                ctx->stream_id = cs.idx_preview;
        }
    }
    log_info("video_ctrl: resolved preview stream=%d size=%ux%u",
             ctx->stream_id, (unsigned)ctx->nv12_w, (unsigned)ctx->nv12_h);

    uint32_t nv_size = ctx->nv12_w * ctx->nv12_h * 3u / 2u;
    if (ctx->image_buffer)
        bitmap_buffer_free(ctx->image_buffer);
    ctx->image_buffer = bitmap_buffer_alloc(nv_size);
    if (!ctx->image_buffer) {
        log_error("video_ctrl: alloc preview buffer failed");
        lv_obj_delete(image);
        return LV_RESULT_INVALID;
    }
    /* Initialize to neutral black in NV12: Y=0, UV=128 (avoid green/blue tint on first frame). */
    memset(ctx->image_buffer, 0, (size_t)ctx->nv12_w * (size_t)ctx->nv12_h);
    memset((uint8_t *)ctx->image_buffer + (size_t)ctx->nv12_w * (size_t)ctx->nv12_h, 128,
           (size_t)ctx->nv12_w * (size_t)ctx->nv12_h / 2u);

    ctx->image_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    ctx->image_dsc.header.cf     = LV_COLOR_FORMAT_NV12;
    ctx->image_dsc.header.w      = (uint16_t)ctx->nv12_w;
    ctx->image_dsc.header.h      = (uint16_t)ctx->nv12_h;
    ctx->image_dsc.header.stride = (uint16_t)ctx->nv12_w;
    ctx->image_dsc.data          = (const uint8_t *)ctx->image_buffer;
    ctx->image_dsc.data_size     = nv_size;
    lv_image_set_src(image, &ctx->image_dsc);
    lv_obj_add_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(image, video_ctrl_preview_click_cb, LV_EVENT_CLICKED, (void *)ctx);

    if (!video_stream)
        video_stream = (multimedia_t *)app_main_get_multimedia();
    ctx->state = LV_DEMO_PLAY_STATUS_PLAYING;
    ctx->af_triggered = 0;
    if (!video_stream || !video_stream->camera || !video_stream->camera->stream_reg_client) {
        log_error("video_ctrl: camera stream_reg_client null");
        bitmap_buffer_free(ctx->image_buffer);
        ctx->image_buffer = NULL;
        lv_obj_delete(image);
        return LV_RESULT_INVALID;
    }

    int ret = video_stream->camera->stream_reg_client(ctx->stream_id, &ctx->reg_client_handle,
                                                      nv12_stream1_preview_callback, (void *)ctx);
    if (ret != 0) {
        log_error("video_ctrl: stream_reg_client(%d) failed %d", ctx->stream_id, ret);
        bitmap_buffer_free(ctx->image_buffer);
        ctx->image_buffer = NULL;
        lv_obj_delete(image);
        return LV_RESULT_INVALID;
    }
    log_info("[DBG_QR_PREVIEW] stream_reg_client(%d) ok handle=0x%x buf=%p %ux%u NV12",
             ctx->stream_id, ctx->reg_client_handle, ctx->image_buffer,
             (unsigned)ctx->nv12_w, (unsigned)ctx->nv12_h);

    ctx->timer = lv_timer_create(timing_task_timer, 1000 / 15, (void *)ctx);
    has_nv12_frame = 0;
    lv_obj_invalidate(ctx->main);
    lv_refr_now(lv_display_get_default());
    return LV_RESULT_OK;
}

static void timing_task_timer(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL || LV_DEMO_PLAY_STATUS_PLAYING != ctx->state)
        return;

    /* NV12/NV21 preview: refresh image when callback pushes new frame */
    if (has_nv12_frame != 0) {
        has_nv12_frame = 0;
        if (ctx->image && ctx->image_buffer)
            lv_image_set_src(ctx->image, &ctx->image_dsc);
        if (ctx->main)
            lv_obj_invalidate(ctx->main);
        lv_refr_now(lv_display_get_default());
        return;
    }

    if (has_call_data == 0)
        return;
    has_call_data = 0;

    lv_obj_t *image = ctx->image;
    uint32_t width = lv_obj_get_content_width(image);
    uint32_t height = lv_obj_get_content_height(image);
    if (width == 0 || height == 0) {
        lv_obj_t *main = ctx->main;
        if (main) {
            width = lv_obj_get_content_width(main);
            height = lv_obj_get_content_height(main);
        }
        if (width == 0 || height == 0)
            width = 640, height = 384;
    }
    uint32_t size = width * height * 3u / 2u;

    if (NULL == ctx->image_buffer) {
        if (size == 0)
            return;
        ctx->image_buffer = bitmap_buffer_alloc(size);
        if (NULL == ctx->image_buffer)
            return;
    }

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    if (0 == image_dsc->header.w) {
        image_dsc->header.w = width;
        image_dsc->header.h = height;
        image_dsc->header.stride = (uint16_t)image_dsc->header.w;
        image_dsc->header.cf = LV_COLOR_FORMAT_NV12;
        image_dsc->data = ctx->image_buffer;
        image_dsc->data_size = size;
    }

    lv_image_set_src(image, image_dsc);
    if (ctx->main)
        lv_obj_invalidate(ctx->main);
    lv_refr_now(lv_display_get_default());
}

lv_result_t tablet_video_ctrl_close()
{
    context_t *ctx = pContext;
    if (ctx == NULL) {
        log_warn("video_ctrl_close ctx is null");
        return LV_RESULT_INVALID;
    }
    /* Never opened preview (e.g. back from Settings without opening QR): static ctx had
     * reg_client_handle==0 from zero-init — stream_unreg_client(0) corrupts camera. */
    if (ctx->state != LV_DEMO_PLAY_STATUS_PLAYING && ctx->state != LV_DEMO_PLAY_STATUS_PAUSE) {
        s_nv12_preview_closing = 0;
        nova_af_focus_ui_hide(&ctx->af_focus_ui);
        if (ctx->timer != NULL) {
            lv_timer_delete(ctx->timer);
            ctx->timer = NULL;
        }
        /* Orphan broadcast client (e.g. failed mid-init): still release handle */
        if (ctx->reg_client_handle >= 0) {
            multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
            if (mm && mm->camera)
                mm->camera->stream_unreg_client(ctx->reg_client_handle);
            ctx->reg_client_handle = -1;
        }
        if (ctx->image_buffer != NULL) {
            bitmap_buffer_free(ctx->image_buffer);
            ctx->image_buffer = NULL;
        }
        video_stream = NULL;
        ctx->state = LV_DEMO_PLAY_STATUS_CLOSE;
        return LV_RESULT_OK;
    }

    s_nv12_preview_closing = 1;
    ctx->state = LV_DEMO_PLAY_STATUS_CLOSE;
    has_nv12_frame = 0;
    nova_af_focus_ui_hide(&ctx->af_focus_ui);

    /* Stop LVGL refresh before unreg so draw never sees a dying dsc/buffer */
    if (NULL != ctx->timer) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }

    {
        multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
        if (mm && mm->camera) {
            if (ctx->reg_client_handle >= 0) {
                mm->camera->stream_unreg_client(ctx->reg_client_handle);
                ctx->reg_client_handle = -1;
            } else if (ctx->stream_id == 0 && mm->camera->stream_add_callback) {
                /* Legacy stream_add_callback path: never call stream_unreg_client(0). */
                mm->camera->stream_add_callback(0, NULL, NULL, NULL);
            }
        }
        video_stream = NULL;
    }

    if (ctx->VideoHandle >= 0)
        video_device_stop(ctx->VideoHandle);

    if (NULL != ctx->image_buffer) {
        /* Broadcast client thread may still drain one frame after unreg */
        system_sleep_ms(50);
        bitmap_buffer_free(ctx->image_buffer);
        ctx->image_buffer = NULL;
    }
    s_nv12_preview_closing = 0;

    return LV_RESULT_OK;
}

#else /* !TABLET_VIDEO_CTRL_ENABLE_PREVIEW — stubs so libsetting_app.so still links */

#include "tablet_common.h"

lv_result_t tablet_video_ctrl_create(lv_obj_t *main)
{
    (void)main;
    return LV_RESULT_INVALID;
}

lv_result_t tablet_video_ctrl_create_for_stream(lv_obj_t *main, int stream_id)
{
    (void)main;
    (void)stream_id;
    return LV_RESULT_INVALID;
}

lv_result_t tablet_video_ctrl_close(void)
{
    return LV_RESULT_OK;
}

#endif /* TABLET_VIDEO_CTRL_ENABLE_PREVIEW */
