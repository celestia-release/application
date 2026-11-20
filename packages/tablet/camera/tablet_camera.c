// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet_lite.
 *
 * LVGL camera preview screen: NV12 broadcast + tap AF, with photo/record/gallery.
 *
 * Paint first. Stream register and unregister run on a worker.
 * NV12 placeholder is Y=0/UV=128.
 */
#define LOG_TAG "TABLET_CAMERA"

/*********************
 *      INCLUDES
 *********************/
#include "tablet_camera.h"

#include "config.h"

#ifdef CONFIG_APP_TABLET_CAMERA
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr.h"
#include "src/misc/lv_types.h"
#include "app_main.h"
#include "multimedia.h"
#include <string.h>
#include <stdlib.h>

#include "log.h"
#include "bitmap.h"
#include "system_error.h"
#include "system_memory.h"
#include "system_time.h"
#include "system_thread.h"
#include "app_globle_config.h"

#include "tablet_common.h"
#include "tablet_font.h"
#include "tablet_camera_capture.h"
#include "tablet_camera_gallery.h"
#include "tablet_camera_storage.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define INVALID_HANDLE              (-1)
#define CONFIG_IMAGE_WIDGET         1
#define CAMERA_AF_ROI_FRAC          (1.0f / 8.0f)
#define CAMERA_AF_FOCUS_BOX_MS      500
#define CAM_BOTTOM_H                GUI_SCALE_VER(88)
#define CAM_SHUTTER_SZ              GUI_SCALE(72)
#define CAM_SHUTTER_INNER_SZ        GUI_SCALE(56)
#define CAM_SHUTTER_BORDER          3
#define CAM_MODE_TOGGLE_GRACE_MS    400
#define CAM_COLOR_ACTIVE            0xffcc00
#define CAM_COLOR_RECORD            0xff3b30
#define CAM_COLOR_RECORDING         0x34c759
#define CAM_COLOR_BUSY              0x8e8e93

enum play_status {
    TABLET_PLAY_STATUS_UNREADY    = 0,
    TABLET_PLAY_STATUS_STOP,
    TABLET_PLAY_STATUS_PAUSE,
    TABLET_PLAY_STATUS_PLAYING,
    TABLET_PLAY_STATUS_CLOSE,
    TABLET_PLAY_STATUS_MAX
};

typedef enum { TABLET_CAM_MODE_PHOTO = 0, TABLET_CAM_MODE_RECORD = 1 } tablet_cam_mode_t;

typedef struct {
    lv_obj_t *main;
    lv_obj_t *window;
#if CONFIG_IMAGE_WIDGET
    lv_obj_t *image;
    lv_image_dsc_t image_dsc;
    void *image_buffer;
#endif
    lv_obj_t *bottom_bar;
    lv_obj_t *btn_gallery;
    lv_obj_t *btn_shutter;
    lv_obj_t *shutter_disc_white;
    lv_obj_t *shutter_disc_red;
    lv_obj_t *shutter_disc_green;
    lv_obj_t *shutter_disc_gray;
    lv_obj_t *btn_mode;
    lv_obj_t *lb_mode;
    lv_obj_t *record_dot;
    lv_obj_t *lb_record;
    lv_timer_t *timer;
    lv_obj_t *focus_box;
    lv_timer_t *focus_box_timer;
    int state;
    uint32_t preview_w;
    uint32_t preview_h;
    tablet_cam_mode_t mode;
    int gallery_open;
} context_t;

static int client_handle;
static multimedia_t *g_multimedia = NULL;
static int s_camera_af_triggered = 0;
static pthread_mutex_t s_preview_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_stream_lock = PTHREAD_MUTEX_INITIALIZER;
/* Enter/exit generation. A worker only releases its own session. */
static volatile int s_preview_epoch;
static volatile int s_stream_epoch;
static volatile int s_preview_ready;
static volatile int s_stream_fail_note;

static void event_handler_btn_back(lv_event_t *e);
static void timing_task_timer(lv_timer_t *timer);
static int apply_frame_to_ctx(context_t *ctx, media_frame_info_t *frame);
static int get_stream_callback(int handle, media_frame_info_t *frame, void *user_data);
static int camera_stream_register(context_t *ctx, const tablet_camera_streams_t *cam_streams);
static void camera_trigger_af_once(void);
static void camera_focus_box_hide(context_t *ctx);
static void camera_focus_box_timer_cb(lv_timer_t *timer);
static void camera_show_focus_box(context_t *ctx, lv_obj_t *parent,
                                  lv_coord_t cx, lv_coord_t cy,
                                  lv_coord_t box_w, lv_coord_t box_h);
static void camera_preview_click_cb(lv_event_t *e);
static void camera_raise_chrome(context_t *ctx);
static void refresh_shutter_discs(void);
static void refresh_mode_label_text(void);
static void refresh_record_status(void);
static void camera_create_bottom_bar(context_t *ctx);

/* NV12 neutral black: Y=0, UV=128 (memset entire buffer to 0 tints green). */
static void camera_preview_fill_nv12_black(void *buf, uint32_t w, uint32_t h)
{
    size_t y_size;
    size_t uv_size;

    if (!buf || w == 0 || h == 0)
        return;
    y_size = (size_t)w * (size_t)h;
    uv_size = y_size / 2u;
    memset(buf, 0, y_size);
    memset((uint8_t *)buf + y_size, 128, uv_size);
}

static int camera_preview_buffer_ensure(context_t *ctx, uint32_t w, uint32_t h)
{
    uint32_t size;

    if (!ctx || w == 0 || h == 0)
        return -1;
    size = w * h * 3u / 2u;
    if (ctx->image_buffer == NULL) {
        ctx->image_buffer = bitmap_buffer_alloc(size);
        if (ctx->image_buffer == NULL)
            return -1;
    }
    camera_preview_fill_nv12_black(ctx->image_buffer, w, h);
    ctx->preview_w = w;
    ctx->preview_h = h;
    return 0;
}

static void camera_stop_preview_ui(context_t *ctx)
{
    if (ctx)
        tablet_camera_gallery_close();
    if (tablet_camera_record_is_active())
        tablet_camera_record_stop();
    s_camera_af_triggered = 0;
    camera_focus_box_hide(ctx);
    if (ctx && ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
}

static void camera_window_delete_event_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    if (ctx == NULL)
        return;
    ctx->state = TABLET_PLAY_STATUS_CLOSE;
    camera_stop_preview_ui(ctx);
}

static int camera_stream_register(context_t *ctx, const tablet_camera_streams_t *cam_streams)
{
    int ret = -1;
    int idx_preview = cam_streams ? cam_streams->idx_preview : -1;
    int idx_main = cam_streams ? cam_streams->idx_main : -1;

    if (!g_multimedia || !g_multimedia->camera || ctx == NULL)
        return -1;

    client_handle = INVALID_HANDLE;
    if (idx_preview >= 0) {
        ret = g_multimedia->camera->stream_reg_client(idx_preview, &client_handle,
                                                      get_stream_callback, (void *)ctx);
    }
    if (ret != 0 && idx_main >= 0) {
        ret = g_multimedia->camera->stream_reg_client(idx_main, &client_handle,
                                                      get_stream_callback, (void *)ctx);
    }
    if (ret != 0)
        ret = g_multimedia->camera->stream_reg_client(1, &client_handle, get_stream_callback, (void *)ctx);
    if (ret != 0)
        ret = g_multimedia->camera->stream_reg_client(0, &client_handle, get_stream_callback, (void *)ctx);
    return ret;
}

/* Online CPU count. sysconf follows this thread's affinity. */
static int camera_read_online_cpus(cpu_set_t *set)
{
    FILE *fp;
    char buf[128];
    char *p;
    int n = 0;

    CPU_ZERO(set);
    fp = fopen("/sys/devices/system/cpu/online", "r");
    if (!fp)
        return 0;
    if (!fgets(buf, (int)sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    p = buf;
    while (*p) {
        int lo;
        int hi;
        int c;

        while (*p && (*p < '0' || *p > '9'))
            p++;
        if (!*p)
            break;
        lo = 0;
        while (*p >= '0' && *p <= '9') {
            lo = lo * 10 + (*p - '0');
            p++;
        }
        hi = lo;
        if (*p == '-') {
            p++;
            hi = 0;
            while (*p >= '0' && *p <= '9') {
                hi = hi * 10 + (*p - '0');
                p++;
            }
        }
        if (hi < lo)
            hi = lo;
        for (c = lo; c <= hi && c < CPU_SETSIZE; c++) {
            CPU_SET(c, set);
            n++;
        }
    }
    return n;
}

/* Widen affinity only while creating the worker, then restore the caller. */
static void camera_thread_create_detached(void *(*fn)(void *), void *arg)
{
    pthread_t tid;
    pthread_attr_t attr;
    cpu_set_t online;
    cpu_set_t saved;
    int n;
    int widened = 0;

    n = camera_read_online_cpus(&online);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (n > 1 &&
        pthread_getaffinity_np(pthread_self(), sizeof(saved), &saved) == 0 &&
        pthread_setaffinity_np(pthread_self(), sizeof(online), &online) == 0)
        widened = 1;
    if (pthread_create(&tid, &attr, fn, arg) != 0)
        log_warn("camera: worker create failed");
    if (widened)
        (void)pthread_setaffinity_np(pthread_self(), sizeof(saved), &saved);
    pthread_attr_destroy(&attr);
}

static void camera_stream_unreg_locked(void)
{
    if (g_multimedia && g_multimedia->camera && g_multimedia->camera->stream_unreg_client &&
        client_handle != INVALID_HANDLE)
        g_multimedia->camera->stream_unreg_client(client_handle);
    client_handle = INVALID_HANDLE;
    s_stream_epoch = 0;
    s_preview_ready = 0;
}

static context_t context = {0};
static context_t *pContext = &context;

static void *camera_preview_start_thread(void *arg)
{
    int epoch = (int)(intptr_t)arg;
    tablet_camera_streams_t streams;
    int ret;

    memset(&streams, 0, sizeof(streams));
    streams.idx_preview = -1;
    streams.idx_main = -1;
    if (g_multimedia)
        (void)tablet_camera_streams_resolve(g_multimedia, &streams);

    pthread_mutex_lock(&s_stream_lock);
    if (epoch != s_preview_epoch || pContext->state == TABLET_PLAY_STATUS_CLOSE) {
        pthread_mutex_unlock(&s_stream_lock);
        return NULL;
    }
    if (client_handle != INVALID_HANDLE)
        camera_stream_unreg_locked();
    ret = camera_stream_register(pContext, &streams);
    if (ret != 0 || epoch != s_preview_epoch || pContext->state == TABLET_PLAY_STATUS_CLOSE) {
        if (client_handle != INVALID_HANDLE)
            camera_stream_unreg_locked();
        pthread_mutex_unlock(&s_stream_lock);
        if (ret != 0 && epoch == s_preview_epoch &&
            pContext->state != TABLET_PLAY_STATUS_CLOSE)
            s_stream_fail_note = 1;
        return NULL;
    }
    s_stream_epoch = epoch;
    s_preview_ready = 1;
    pthread_mutex_unlock(&s_stream_lock);
    return NULL;
}

static void *camera_preview_stop_thread(void *arg)
{
    int epoch = (int)(intptr_t)arg;

    pthread_mutex_lock(&s_stream_lock);
    if (s_stream_epoch == epoch)
        camera_stream_unreg_locked();
    pthread_mutex_unlock(&s_stream_lock);

    pthread_mutex_lock(&s_preview_lock);
    if (s_preview_epoch == epoch + 1 && pContext->image_buffer != NULL) {
        bitmap_buffer_free(pContext->image_buffer);
        pContext->image_buffer = NULL;
    }
    pthread_mutex_unlock(&s_preview_lock);
    return NULL;
}

static volatile int has_call_data = 0;

typedef enum { DEFER_MODE_TOGGLE, DEFER_SHUTTER, DEFER_GALLERY } defer_action_t;
static defer_action_t s_defer_action;
static lv_timer_t *s_defer_timer;
static char s_shutter_note[96];
static uint32_t s_shutter_note_ms;
static pthread_t s_worker_thread;
static volatile int s_worker_state; /* 0=idle 1=busy 2=done_ok 3=done_fail 4=no_storage 5=saving */
static char s_worker_path[320];
static uint32_t s_mode_toggle_tick;
static uint32_t s_record_start_tick;
static lv_timer_t *s_record_timer;

static int apply_frame_to_ctx(context_t *ctx, media_frame_info_t *frame)
{
    if (frame == NULL || ctx == NULL)
        return 0;
#if CONFIG_IMAGE_WIDGET
    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t nv_size = src_w * src_h * 3u / 2u;

    if ((frame->code_type != MEDIA_CODEC_VIDEO_NV12 && frame->code_type != 3) ||
        frame->data == NULL ||
        src_w == 0 || src_h == 0 ||
        frame->data_len != nv_size)
    {
        static unsigned log_skip_once;
        if (log_skip_once < 8u) {
            log_warn("[camera] frame skipped: code_type=%d w=%u h=%u need_len=%u got_len=%u data=%p",
                     frame->code_type, (unsigned)src_w, (unsigned)src_h,
                     (unsigned)nv_size, (unsigned)frame->data_len, (void *)frame->data);
            log_skip_once++;
        }
        return 0;
    }

    lv_color_format_t cf = LV_COLOR_FORMAT_NV12;

    pthread_mutex_lock(&s_preview_lock);
    if (ctx->image_buffer == NULL || ctx->state == TABLET_PLAY_STATUS_CLOSE) {
        pthread_mutex_unlock(&s_preview_lock);
        return 0;
    }

    if (ctx->preview_w != src_w || ctx->preview_h != src_h) {
        bitmap_buffer_free(ctx->image_buffer);
        ctx->image_buffer = bitmap_buffer_alloc(nv_size);
        if (ctx->image_buffer == NULL) {
            pthread_mutex_unlock(&s_preview_lock);
            return 0;
        }
        ctx->preview_w = src_w;
        ctx->preview_h = src_h;
    }

    memcpy(ctx->image_buffer, frame->data, nv_size);

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    image_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    image_dsc->header.cf     = cf;
    image_dsc->header.w      = (uint16_t)src_w;
    image_dsc->header.h      = (uint16_t)src_h;
    image_dsc->header.stride = (uint16_t)src_w;
    image_dsc->data          = (const uint8_t *)ctx->image_buffer;
    image_dsc->data_size     = nv_size;

    pthread_mutex_unlock(&s_preview_lock);
#endif
    return 1;
}

static int get_stream_callback(int handle, media_frame_info_t *frame, void* user_data)
{
    (void)handle;
    if (frame == NULL || user_data == NULL)
        return -1;
    context_t *ctx = (context_t *)(user_data);
    if (ctx->state == TABLET_PLAY_STATUS_CLOSE)
        return 0;
    /* The gallery covers the preview; leave the CPU to video decode. */
    if (ctx->gallery_open)
        return 0;
    if (apply_frame_to_ctx(ctx, frame))
        has_call_data = 1;
    return 0;
}

static void camera_trigger_af_once(void)
{
    if (s_camera_af_triggered) {
        return;
    }
    if (!g_multimedia || !g_multimedia->camera || !g_multimedia->camera->trigger_af) {
        return;
    }
    if (g_multimedia->camera->af_is_supported &&
        !g_multimedia->camera->af_is_supported(0)) {
        return;
    }
    if (g_multimedia->camera->trigger_af(0, NULL) == 0) {
        s_camera_af_triggered = 1;
        log_info("camera: trigger AF once after first frame");
    }
}

static void camera_focus_box_hide(context_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->focus_box_timer) {
        lv_timer_delete(ctx->focus_box_timer);
        ctx->focus_box_timer = NULL;
    }
    if (ctx->focus_box && lv_obj_is_valid(ctx->focus_box)) {
        lv_obj_delete(ctx->focus_box);
    }
    ctx->focus_box = NULL;
}

static void camera_focus_box_timer_cb(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);

    if (ctx) {
        ctx->focus_box_timer = NULL;
        if (ctx->focus_box && lv_obj_is_valid(ctx->focus_box)) {
            lv_obj_delete(ctx->focus_box);
        }
        ctx->focus_box = NULL;
    }
    /* repeat_count=1: LVGL deletes the timer after this callback. */
}

static void camera_raise_chrome(context_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->bottom_bar && lv_obj_is_valid(ctx->bottom_bar))
        lv_obj_move_foreground(ctx->bottom_bar);
    if (ctx->record_dot && lv_obj_is_valid(ctx->record_dot))
        lv_obj_move_foreground(ctx->record_dot);
}

static void camera_show_focus_box(context_t *ctx, lv_obj_t *parent,
                                  lv_coord_t cx, lv_coord_t cy,
                                  lv_coord_t box_w, lv_coord_t box_h)
{
    lv_coord_t x;
    lv_coord_t y;

    if (!ctx || !parent) {
        return;
    }
    camera_focus_box_hide(ctx);

    if (box_w < 24) {
        box_w = 24;
    }
    if (box_h < 24) {
        box_h = 24;
    }
    x = cx - box_w / 2;
    y = cy - box_h / 2;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    ctx->focus_box = lv_obj_create(parent);
    if (!ctx->focus_box) {
        return;
    }
    lv_obj_remove_style_all(ctx->focus_box);
    lv_obj_set_size(ctx->focus_box, box_w, box_h);
    lv_obj_set_pos(ctx->focus_box, x, y);
    lv_obj_set_style_bg_color(ctx->focus_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(ctx->focus_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ctx->focus_box, 2, 0);
    lv_obj_set_style_border_color(ctx->focus_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(ctx->focus_box, LV_OPA_80, 0);
    lv_obj_set_style_radius(ctx->focus_box, 4, 0);
    lv_obj_clear_flag(ctx->focus_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(ctx->focus_box);
    camera_raise_chrome(ctx);

    ctx->focus_box_timer = lv_timer_create(camera_focus_box_timer_cb,
                                           CAMERA_AF_FOCUS_BOX_MS, ctx);
    if (ctx->focus_box_timer) {
        lv_timer_set_repeat_count(ctx->focus_box_timer, 1);
    }
}

static void camera_preview_click_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    lv_obj_t *target;
    lv_point_t pt;
    lv_area_t coords;
    lv_coord_t w;
    lv_coord_t h;
    lv_coord_t lx;
    lv_coord_t ly;
    float nx;
    float ny;
    media_camera_af_roi_t roi;

    if (!ctx || ctx->state != TABLET_PLAY_STATUS_PLAYING || ctx->gallery_open) {
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_multimedia || !g_multimedia->camera || !g_multimedia->camera->trigger_af) {
        return;
    }
    if (g_multimedia->camera->af_is_supported &&
        !g_multimedia->camera->af_is_supported(0)) {
        return;
    }

#if CONFIG_IMAGE_WIDGET
    target = ctx->image ? ctx->image : ctx->window;
#else
    target = ctx->window;
#endif
    if (!target || !lv_obj_is_valid(target)) {
        return;
    }

    lv_indev_get_point(lv_indev_active(), &pt);
    lv_obj_get_coords(target, &coords);
    w = coords.x2 - coords.x1 + 1;
    h = coords.y2 - coords.y1 + 1;
    if (w <= 0 || h <= 0) {
        return;
    }
    lx = pt.x - coords.x1;
    ly = pt.y - coords.y1;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) {
        return;
    }

    nx = (float)lx / (float)w;
    ny = (float)ly / (float)h;
    roi.nw = CAMERA_AF_ROI_FRAC;
    roi.nh = CAMERA_AF_ROI_FRAC;
    roi.nx = nx - roi.nw * 0.5f;
    roi.ny = ny - roi.nh * 0.5f;
    if (roi.nx < 0.0f) {
        roi.nx = 0.0f;
    }
    if (roi.ny < 0.0f) {
        roi.ny = 0.0f;
    }
    if (roi.nx + roi.nw > 1.0f) {
        roi.nx = 1.0f - roi.nw;
    }
    if (roi.ny + roi.nh > 1.0f) {
        roi.ny = 1.0f - roi.nh;
    }

    camera_show_focus_box(ctx, target, lx, ly, (lv_coord_t)(w / 8), (lv_coord_t)(h / 8));

    if (g_multimedia->camera->trigger_af(0, &roi) == 0) {
        log_info("camera: tap AF roi=(%.3f,%.3f,%.3f,%.3f)",
                 (double)roi.nx, (double)roi.ny, (double)roi.nw, (double)roi.nh);
    } else {
        log_warn("camera: tap AF trigger failed");
    }
}

static lv_obj_t *create_shutter_disc(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *disc = lv_obj_create(parent);

    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, CAM_SHUTTER_INNER_SZ, CAM_SHUTTER_INNER_SZ);
    lv_obj_center(disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, color, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    return disc;
}

static void refresh_shutter_discs(void)
{
    enum { DISC_WHITE = 0, DISC_RED, DISC_GREEN, DISC_GRAY } which;
    lv_obj_t *discs[4];
    int i;
    context_t *ctx = pContext;

    if (s_worker_state == 1)
        which = DISC_GRAY;
    else if (tablet_camera_record_is_active())
        which = DISC_GREEN;
    else if (ctx->mode == TABLET_CAM_MODE_RECORD)
        which = DISC_RED;
    else
        which = DISC_WHITE;

    discs[DISC_WHITE] = ctx->shutter_disc_white;
    discs[DISC_RED] = ctx->shutter_disc_red;
    discs[DISC_GREEN] = ctx->shutter_disc_green;
    discs[DISC_GRAY] = ctx->shutter_disc_gray;

    for (i = 0; i < 4; i++) {
        if (!discs[i] || !lv_obj_is_valid(discs[i]))
            continue;
        if (i == (int)which)
            lv_obj_remove_flag(discs[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(discs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_mode_label_text(void)
{
    context_t *ctx = pContext;

    if (!ctx->lb_mode || !lv_obj_is_valid(ctx->lb_mode))
        return;
    lv_label_set_text(ctx->lb_mode,
                      ctx->mode == TABLET_CAM_MODE_PHOTO ? lan("Photo") : lan("Record"));
}

static void refresh_record_status(void)
{
    char buf[20];
    context_t *ctx = pContext;

    if (!ctx->record_dot || !lv_obj_is_valid(ctx->record_dot))
        return;

    if (!tablet_camera_record_is_active()) {
        lv_obj_add_flag(ctx->record_dot, LV_OBJ_FLAG_HIDDEN);
        s_record_start_tick = 0;
        return;
    }

    if (s_record_start_tick == 0)
        s_record_start_tick = lv_tick_get();

    lv_obj_remove_flag(ctx->record_dot, LV_OBJ_FLAG_HIDDEN);
    if (!ctx->lb_record || !lv_obj_is_valid(ctx->lb_record))
        return;

    {
        uint32_t elapsed_ms = lv_tick_get() - s_record_start_tick;
        int sec = (int)(elapsed_ms / 1000U);

        snprintf(buf, sizeof(buf), "REC %02d:%02d", sec / 60, sec % 60);
        lv_label_set_text(ctx->lb_record, buf);
    }
}

static void record_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_record_status();
}

static void show_shutter_note_timer(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_shutter_note[0] != '\0')
        show_toast_note(s_shutter_note, s_shutter_note_ms, s_shutter_note_ms >= 2000u);
    s_shutter_note[0] = '\0';
}

/* Failures use >= 2000 ms and get the red dot. */
static void queue_shutter_note(const char *text, uint32_t duration_ms)
{
    if (!text)
        return;
    snprintf(s_shutter_note, sizeof(s_shutter_note), "%s", text);
    s_shutter_note_ms = duration_ms;
}

static void async_mode_ui_refresh_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    refresh_mode_label_text();
    refresh_shutter_discs();
}

static void async_shutter_ui_refresh_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    refresh_mode_label_text();
    refresh_shutter_discs();
    refresh_record_status();
    if (s_shutter_note[0] != '\0')
        lv_timer_create(show_shutter_note_timer, 100, NULL);
}

static void *shutter_worker_thread(void *arg)
{
    context_t *ctx = pContext;

    (void)arg;
    (void)thread_set_current_name("cam_shutter");

    if (ctx->mode == TABLET_CAM_MODE_PHOTO) {
        int ret;

        if (!tablet_album_is_ready()) {
            s_worker_state = 4;
        } else {
            ret = tablet_camera_take_photo(s_worker_path, sizeof(s_worker_path));
            if (ret == TABLET_ALBUM_ERR_NOT_READY)
                s_worker_state = 4;
            else
                s_worker_state = (ret == 0) ? 2 : 3;
        }
    } else if (tablet_camera_record_is_active()) {
        s_worker_state = 5;  /* signal UI: NV12 append in progress */
        tablet_camera_record_stop();
        s_worker_state = 2;
    } else {
        int ret;

        if (!tablet_album_is_ready()) {
            s_worker_state = 4;
        } else {
            ret = tablet_camera_record_start();
            if (ret == TABLET_ALBUM_ERR_NOT_READY)
                s_worker_state = 4;
            else
                s_worker_state = (ret == 0) ? 2 : 3;
        }
    }
    log_info("worker: exit mode=%d state=%d", ctx->mode, s_worker_state);
    return NULL;
}

static void shutter_poll_cb(lv_timer_t *t)
{
    context_t *ctx = pContext;

    if (s_worker_state == 1)
        return;

    if (s_worker_state == 5) {
        /* NV12 trailer being appended: show non-closeable saving hint once, keep polling */
        show_saving_hint(lan("Saving..."));
        s_worker_state = 6;
        return;
    }

    if (s_worker_state == 6)
        return;  /* saving hint shown, waiting for record_stop to complete */

    lv_timer_delete(t);
    close_saving_hint();  /* dismiss "正在保存中..." before showing next message */
    if (s_worker_state == 2) {
        /* Recording start needs no note: the REC timer already shows it. */
        if (ctx->mode == TABLET_CAM_MODE_PHOTO)
            queue_shutter_note(lan("Photo saved"), 1200);
        else if (!tablet_camera_record_is_active())
            queue_shutter_note(lan("Video saved"), 1500);
    } else if (s_worker_state == 4) {
        tablet_album_show_missing_hint();
    } else {
        if (ctx->mode == TABLET_CAM_MODE_PHOTO)
            queue_shutter_note(lan("Capture failed"), 2500);
        else
            queue_shutter_note(lan("Failed to start recording"), 2500);
    }
    lv_timer_create(async_shutter_ui_refresh_cb, 16, NULL);
}

static void gallery_closed_cb(void *user_data)
{
    context_t *ctx = (context_t *)user_data;

    if (ctx)
        ctx->gallery_open = 0;
}

static void defer_cb(lv_timer_t *timer)
{
    context_t *ctx = pContext;

    s_defer_timer = NULL;
    lv_timer_delete(timer);

    if (s_defer_action == DEFER_MODE_TOGGLE) {
        if (tablet_camera_record_is_active())
            return;
        ctx->mode = (ctx->mode == TABLET_CAM_MODE_PHOTO) ?
                    TABLET_CAM_MODE_RECORD : TABLET_CAM_MODE_PHOTO;
        lv_timer_create(async_mode_ui_refresh_cb, 16, NULL);
        return;
    }
    if (s_defer_action == DEFER_GALLERY) {
        if (tablet_camera_record_is_active() || s_worker_state == 1)
            return;
        ctx->gallery_open = 1;
        tablet_camera_gallery_show(ctx->window, TABLET_GALLERY_PHOTO, gallery_closed_cb, ctx);
        return;
    }
    if (s_defer_action == DEFER_SHUTTER) {
        if (s_worker_state == 1)
            return;
        if (!s_preview_ready || client_handle == INVALID_HANDLE) {
            log_info("camera: shutter ignored, preview not ready");
            return;
        }
        s_worker_state = 1;
        refresh_shutter_discs();
        if (pthread_create(&s_worker_thread, NULL, shutter_worker_thread, NULL) != 0) {
            s_worker_state = 3;
            refresh_shutter_discs();
        } else {
            pthread_detach(s_worker_thread);
        }
        lv_timer_create(shutter_poll_cb, 50, NULL);
    }
}

static void defer_action(defer_action_t action)
{
    if (s_defer_timer != NULL)
        return;
    s_defer_action = action;
    s_defer_timer = lv_timer_create(defer_cb, 5, NULL);
}

static void event_mode_toggle(lv_event_t *e)
{
    uint32_t now = lv_tick_get();

    (void)e;
    if (s_mode_toggle_tick != 0 &&
        now - s_mode_toggle_tick < CAM_MODE_TOGGLE_GRACE_MS)
        return;
    s_mode_toggle_tick = now;
    defer_action(DEFER_MODE_TOGGLE);
}

static void event_shutter(lv_event_t *e)
{
    (void)e;
    defer_action(DEFER_SHUTTER);
}

static void event_gallery(lv_event_t *e)
{
    (void)e;
    defer_action(DEFER_GALLERY);
}

static void camera_create_bottom_bar(context_t *ctx)
{
    lv_obj_t *ring;
    lv_font_t *font;
    int col_w;

    if (!ctx || !ctx->window)
        return;

    col_w = LV_HOR_RES / 3;
    font = tablet_font_get_freetype_font(GUI_SCALE(28));
    if (!font)
        font = tablet_font_get_montserrat_font(GUI_SCALE(26));

    ctx->bottom_bar = lv_obj_create(ctx->window);
    lv_obj_remove_style_all(ctx->bottom_bar);
    lv_obj_set_size(ctx->bottom_bar, LV_HOR_RES, CAM_BOTTOM_H);
    /* Reserve the bottom strip for the launcher home indicator. */
    lv_obj_align(ctx->bottom_bar, LV_ALIGN_BOTTOM_MID, 0, -TABLET_HOME_BAR_H);
    lv_obj_set_style_bg_color(ctx->bottom_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ctx->bottom_bar, LV_OPA_70, 0);
    lv_obj_clear_flag(ctx->bottom_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    ctx->btn_gallery = lv_button_create(ctx->bottom_bar);
    lv_obj_remove_style_all(ctx->btn_gallery);
    lv_obj_remove_flag(ctx->btn_gallery, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(ctx->btn_gallery, col_w, CAM_BOTTOM_H);
    lv_obj_align(ctx->btn_gallery, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(ctx->btn_gallery, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(ctx->btn_gallery, event_gallery, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ctx->btn_gallery, event_gallery, LV_EVENT_LONG_PRESSED, NULL);
    {
        lv_obj_t *lb = lv_label_create(ctx->btn_gallery);
        lv_label_set_text(lb, lan("Gallery"));
        lv_obj_set_style_text_color(lb, lv_color_hex(0xffffff), 0);
        if (font)
            lv_obj_set_style_text_font(lb, font, 0);
        lv_obj_center(lb);
    }

    ctx->btn_shutter = lv_button_create(ctx->bottom_bar);
    lv_obj_remove_style_all(ctx->btn_shutter);
    lv_obj_remove_flag(ctx->btn_shutter, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(ctx->btn_shutter, col_w, CAM_BOTTOM_H);
    lv_obj_align(ctx->btn_shutter, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(ctx->btn_shutter, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(ctx->btn_shutter, event_shutter, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ctx->btn_shutter, event_shutter, LV_EVENT_LONG_PRESSED, NULL);

    ring = lv_obj_create(ctx->btn_shutter);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, CAM_SHUTTER_SZ, CAM_SHUTTER_SZ);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, CAM_SHUTTER_BORDER, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xffffff), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    ctx->shutter_disc_white = create_shutter_disc(ring, lv_color_hex(0xffffff));
    ctx->shutter_disc_red = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_RECORD));
    ctx->shutter_disc_green = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_RECORDING));
    ctx->shutter_disc_gray = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_BUSY));
    lv_obj_add_flag(ctx->shutter_disc_red, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->shutter_disc_green, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->shutter_disc_gray, LV_OBJ_FLAG_HIDDEN);

    ctx->btn_mode = lv_button_create(ctx->bottom_bar);
    lv_obj_remove_style_all(ctx->btn_mode);
    lv_obj_remove_flag(ctx->btn_mode, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(ctx->btn_mode, col_w, CAM_BOTTOM_H);
    lv_obj_align(ctx->btn_mode, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(ctx->btn_mode, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(ctx->btn_mode, event_mode_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ctx->btn_mode, event_mode_toggle, LV_EVENT_LONG_PRESSED, NULL);
    ctx->lb_mode = lv_label_create(ctx->btn_mode);
    lv_label_set_text(ctx->lb_mode, lan("Photo"));
    lv_obj_set_style_text_color(ctx->lb_mode, lv_color_hex(CAM_COLOR_ACTIVE), 0);
    if (font)
        lv_obj_set_style_text_font(ctx->lb_mode, font, 0);
    lv_obj_center(ctx->lb_mode);

    ctx->record_dot = lv_obj_create(ctx->window);
    lv_obj_remove_style_all(ctx->record_dot);
    lv_obj_set_size(ctx->record_dot, GUI_SCALE(220), GUI_SCALE_VER(40));
    lv_obj_align(ctx->record_dot, LV_ALIGN_TOP_MID, 0, GUI_SCALE_VER(64));
    lv_obj_set_style_bg_color(ctx->record_dot, lv_color_hex(CAM_COLOR_RECORD), 0);
    lv_obj_set_style_bg_opa(ctx->record_dot, LV_OPA_70, 0);
    lv_obj_set_style_radius(ctx->record_dot, 8, 0);
    lv_obj_add_flag(ctx->record_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ctx->record_dot, LV_OBJ_FLAG_CLICKABLE);
    ctx->lb_record = lv_label_create(ctx->record_dot);
    lv_label_set_text(ctx->lb_record, "REC 00:00");
    lv_obj_set_style_text_color(ctx->lb_record, lv_color_hex(0xffffff), 0);
    if (font)
        lv_obj_set_style_text_font(ctx->lb_record, font, 0);
    lv_obj_center(ctx->lb_record);

    if (!s_record_timer)
        s_record_timer = lv_timer_create(record_status_timer_cb, 500, NULL);
}

lv_result_t tablet_camera_create(lv_obj_t * main)
{
    context_t *ctx = pContext;
    void *pv;
    tablet_camera_streams_t cam_streams;
    int epoch;

    epoch = ++s_preview_epoch;
    s_preview_ready = 0;
    s_stream_fail_note = 0;
    has_call_data = 0;

    pthread_mutex_lock(&s_preview_lock);
    pv = pContext->image_buffer;
    memset(ctx, 0, sizeof(context_t));
    pContext->image_buffer = pv;
    ctx->main = main;
    ctx->mode = TABLET_CAM_MODE_PHOTO;
    s_camera_af_triggered = 0;
    s_worker_state = 0;
    s_record_start_tick = 0;

    g_multimedia = (multimedia_t *)app_main_get_multimedia();
    ctx->preview_w = (uint32_t)LV_HOR_RES;
    ctx->preview_h = (uint32_t)LV_VER_RES;
    memset(&cam_streams, 0, sizeof(cam_streams));
    cam_streams.idx_preview = -1;
    cam_streams.idx_main = -1;
    if (g_multimedia && tablet_camera_streams_resolve(g_multimedia, &cam_streams) == 0 &&
        cam_streams.preview_w > 0 && cam_streams.preview_h > 0) {
        ctx->preview_w = (uint32_t)cam_streams.preview_w;
        ctx->preview_h = (uint32_t)cam_streams.preview_h;
    }

#if CONFIG_IMAGE_WIDGET
    if (camera_preview_buffer_ensure(ctx, ctx->preview_w, ctx->preview_h) != 0) {
        pthread_mutex_unlock(&s_preview_lock);
        log_error("tablet_camera_create: preview buffer alloc failed");
        show_msgbox_note("Start video device fail");
        memset((void *)ctx, 0, sizeof(context_t));
        pContext->image_buffer = pv;
        return LV_RESULT_INVALID;
    }
#endif
    pthread_mutex_unlock(&s_preview_lock);

    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (!screen)
        return SYSTEM_FAILED;

    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *window = lv_obj_create(screen);
    if (NULL == window)
        goto ERROR;
    ctx->window = window;
    lv_obj_remove_style_all(window);
    lv_obj_remove_flag(window, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_bg_color(window, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(window, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_event_cb(window, camera_window_delete_event_cb, LV_EVENT_DELETE, (void *)ctx);

#if CONFIG_IMAGE_WIDGET
    memset(&ctx->image_dsc, 0, sizeof(ctx->image_dsc));
    lv_obj_t * image = lv_image_create(window);
    if (NULL == image)
        goto ERROR;
    lv_obj_remove_style_all(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
    lv_obj_center(image);
    ctx->image = image;
    {
        uint32_t width = ctx->preview_w;
        uint32_t height = ctx->preview_h;
        uint32_t size = width * height * 3u / 2u;

        if (NULL == ctx->image_buffer) {
            if (camera_preview_buffer_ensure(ctx, width, height) != 0)
                goto ERROR;
        }
        ctx->image_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        ctx->image_dsc.header.cf     = LV_COLOR_FORMAT_NV12;
        ctx->image_dsc.header.w      = (uint16_t)width;
        ctx->image_dsc.header.h      = (uint16_t)height;
        ctx->image_dsc.header.stride = (uint16_t)width;
        ctx->image_dsc.data          = (const uint8_t *)ctx->image_buffer;
        ctx->image_dsc.data_size     = size;
        lv_image_set_src(image, &ctx->image_dsc);
        lv_obj_add_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(image, camera_preview_click_cb, LV_EVENT_CLICKED, (void *)ctx);
    }
#endif

    lv_obj_t *top = tablet_create_top(window, lan("Camera"), event_handler_btn_back, (void *)ctx);
    if (NULL == top)
        goto ERROR;

    camera_create_bottom_bar(ctx);
    camera_raise_chrome(ctx);

    ctx->state = TABLET_PLAY_STATUS_PLAYING;
    {
        lv_timer_t *timer = lv_timer_create(timing_task_timer, 1000 / 15, (void *)ctx);
        if (NULL == timer)
            goto ERROR;
        ctx->timer = timer;
    }
    lv_obj_invalidate(window);
    lv_refr_now(lv_display_get_default());
    log_info("camera: ui ready, register preview in background epoch=%d", epoch);
    camera_thread_create_detached(camera_preview_start_thread, (void *)(intptr_t)epoch);
    return LV_RESULT_OK;

ERROR:
    camera_focus_box_hide(ctx);
    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
    }
    show_msgbox_note("Start video device fail");
    if (screen)
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    if (main)
        lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);
    if (ctx->timer != NULL) {
        lv_timer_del(ctx->timer);
        ctx->timer = NULL;
    }
    if (window != NULL)
        lv_obj_delete(window);
    memset((void *)ctx, 0, sizeof(context_t));
    pContext->image_buffer = pv;
    return LV_RESULT_INVALID;
}

static void timing_task_timer(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL)
        return;
    if (s_stream_fail_note) {
        s_stream_fail_note = 0;
        if (ctx->state != TABLET_PLAY_STATUS_CLOSE)
            show_msgbox_note("Start video device fail");
    }
    if (TABLET_PLAY_STATUS_PLAYING != ctx->state)
        return;
    if (ctx->gallery_open)
        return;
    lv_obj_t *window = ctx->window;
    if (NULL == window)
        return;

    if (has_call_data == 0)
        return;
    has_call_data = 0;
#if CONFIG_IMAGE_WIDGET
    lv_obj_t *image = ctx->image;
    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    if (!image || !image_dsc)
        return;
    if (image_dsc->data && image_dsc->header.w > 0 && image_dsc->header.h > 0) {
        lv_image_set_src(image, image_dsc);
        if (!s_camera_af_triggered)
            camera_trigger_af_once();
    }
#endif
    lv_obj_invalidate(window);
}

static void event_handler_btn_back(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    if (ctx == NULL)
        return;
    if (ctx->gallery_open)
        return;
    if (tablet_camera_record_is_active() || s_worker_state == 1) {
        show_toast_note(lan("Stop recording first"), 2000, 1);
        return;
    }
    int closed_epoch = s_preview_epoch;

    ctx->state = TABLET_PLAY_STATUS_CLOSE;
    s_preview_ready = 0;
    s_preview_epoch = closed_epoch + 1;
    if (s_record_timer) {
        lv_timer_delete(s_record_timer);
        s_record_timer = NULL;
    }
    if (s_defer_timer) {
        lv_timer_delete(s_defer_timer);
        s_defer_timer = NULL;
    }
    camera_stop_preview_ui(ctx);
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (screen)
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    if (ctx->window != NULL)
        lv_obj_add_flag(ctx->window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(ctx->main);
    lv_obj_remove_flag(ctx->main, LV_OBJ_FLAG_HIDDEN);
    if (disp)
        lv_refr_now(disp);
    if (ctx->window != NULL) {
        lv_obj_delete(ctx->window);
        ctx->window = NULL;
    }
    camera_thread_create_detached(camera_preview_stop_thread, (void *)(intptr_t)closed_epoch);
}

#endif /* CONFIG_APP_TABLET_CAMERA */
