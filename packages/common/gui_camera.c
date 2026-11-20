// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/common.
 *
 * LVGL demo camera preview screen implementation.
 *
 * Camera flow (Nova -> UI):
 * 1. tablet_camera_create() -> gui_camera_streams_resolve() -> stream_reg_client(preview|main, get_stream_callback, ctx)
 * 2. camera_service_reg_client: if broadcast[1] not inited -> camera_manager_init()
 *    (set_callback_ops(broadcast_only), configure_streams, stream_process_request for VIDEO_RECORD)
 * 3. HAL capture threads produce frames -> process_capture_result(result)
 * 4. broadcast_only_process_capture_result: match result->output_buffers->stream to streams_cfg[i],
 *    type VIDEO_RECORD -> camera_broadcast_joint(broadcast[i], inbuf) -> ring
 * 5. cam_bc_v1 thread: camera_broadcast_get_frame() -> get_stream_callback(handle, &buf, ctx)
 *    -> apply_frame_to_ctx(ctx, frame), has_call_data=1
 * 6. timing_task_timer (main thread): if has_call_data==0, optionally stream_get_frame() and apply_frame_to_ctx;
 *    if has_call_data==1 -> lv_image_set_src(image, image_dsc), lv_obj_invalidate(window)
 * So both callback (push) and timer pull can feed the UI; volatile has_call_data links them.
 */
#define LOG_TAG "GUI_CAMERA"

/*********************
 *      INCLUDES
 *********************/
#include "gui_camera.h"

#if APP_GUI_CAMERA
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr.h"
#include "src/misc/lv_types.h"
#include "app_main.h"
#include "multimedia.h"
#include <string.h>

/* Framework GUI header */
#include "log.h"
#include "system_error.h"
#include "system_memory.h"
#include "system_time.h"
#include "app_globle_config.h"

#include "gui_common.h"
#include "gui_font.h"
#include "lv_display_config.h"
#include "dlfcn.h"
#include "src/misc/cache/instance/lv_image_cache.h"
/*********************
 *      DEFINES
 *********************/

#define INVALID_HANDLE              (-1)
#define CONFIG_IMAGE_WIDGET         1
#define GUI_CAMERA_PREVIEW_TIMER_MS 40u
#define GUI_CAMERA_REFR_PERIOD_FAST 25u
#define GUI_CAMERA_PULL_BURST       1u

/**********************
 *      TYPEDEFS
 **********************/
enum play_status {
    GUI_PLAY_STATUS_UNREADY    = 0,
    GUI_PLAY_STATUS_STOP,
    GUI_PLAY_STATUS_PAUSE,
    GUI_PLAY_STATUS_PLAYING,
    GUI_PLAY_STATUS_CLOSE,
    GUI_PLAY_STATUS_MAX
};

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_obj_t *director;
    int state;
} botton_t;

typedef struct {
    lv_obj_t *obj;
    botton_t play;
    botton_t capture;
    botton_t record;
} ctrl_panel_t;

typedef struct {
    lv_obj_t *main;
    lv_obj_t *window;
#if CONFIG_IMAGE_WIDGET
    lv_obj_t *image;
    lv_image_dsc_t image_dsc;
    void *image_buffer;
    void *image_buffer_back;
    int preview_buf_write;
    int preview_frame_ready;
    const void *preview_last_present_ptr;
#endif
    ctrl_panel_t ctrl;
    lv_timer_t *timer;
    int state;
    uint32_t preview_w;
    uint32_t preview_h;
    uint32_t stream_w;
    uint32_t stream_h;
    uint32_t last_seq;
    uint32_t frame_token;
    int stream_idx_preview;
    int stream_idx_main;
    int preview_started;
    int preview_present_hold;
    int preview_pull_suspend;
} context_t;

static uint32_t s_saved_refr_period;
static int s_refr_period_saved;

static int client_handle;
static multimedia_t *g_multimedia = NULL;
static gui_camera_preview_frame_cb_t s_preview_frame_cb;
static void *s_preview_frame_cb_user;

static void gui_camera_free_preview_buffer(void *buf)
{
    if (buf != NULL)
        system_free(buf);
}

static void *gui_camera_alloc_preview_buffer(uint32_t size)
{
    /* CPU-only buffer for LVGL lv_image_dsc; avoid DMA (cache coherency on SW draw path). */
    return system_malloc(size);
}

static void gui_camera_free_preview_buffers(context_t *ctx)
{
    if (!ctx)
        return;

    gui_camera_free_preview_buffer(ctx->image_buffer);
    gui_camera_free_preview_buffer(ctx->image_buffer_back);
    ctx->image_buffer = NULL;
    ctx->image_buffer_back = NULL;
    ctx->preview_buf_write = 0;
    ctx->preview_frame_ready = 0;
    ctx->preview_last_present_ptr = NULL;
}

static int gui_camera_alloc_preview_buffers(context_t *ctx, uint32_t size)
{
    if (!ctx)
        return -1;

    gui_camera_free_preview_buffers(ctx);
    ctx->image_buffer = gui_camera_alloc_preview_buffer(size);
    ctx->image_buffer_back = gui_camera_alloc_preview_buffer(size);
    if (!ctx->image_buffer || !ctx->image_buffer_back) {
        gui_camera_free_preview_buffers(ctx);
        return -1;
    }
    ctx->preview_buf_write = 0;
    return 0;
}

static void *gui_camera_write_buf(context_t *ctx)
{
    return ctx->preview_buf_write ? ctx->image_buffer_back : ctx->image_buffer;
}

static void *gui_camera_display_buf(context_t *ctx)
{
    return ctx->preview_buf_write ? ctx->image_buffer_back : ctx->image_buffer;
}

static void camera_window_delete_event_cb(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    if (ctx == NULL)
        return;
    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
        log_info("camera: stream_unreg_client (preview shutdown)");
    }
    if (ctx && ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
#if CONFIG_IMAGE_WIDGET
    if (ctx)
        gui_camera_free_preview_buffers(ctx);
#endif
    /* The window (and its child image) are being deleted by LVGL right now.
     * Clear our cached widget pointers so a later gui_camera_shutdown() or
     * present does not invalidate a freed object (SIGSEGV in
     * lv_obj_invalidate -> lv_obj_get_ext_draw_size). */
    if (ctx) {
        ctx->window = NULL;
        ctx->image = NULL;
        ctx->preview_last_present_ptr = NULL;
        ctx->timer = NULL;
    }
}

static void event_handler_btn_back(lv_event_t *e);
static void event_handler_btn_play(lv_event_t *e);
static void event_handler_btn_capture(lv_event_t *e);
static void timing_task_timer(lv_timer_t *timer);
static int apply_frame_to_ctx(context_t *ctx, media_frame_info_t *frame);
static void gui_camera_present_frame(context_t *ctx);
static int gui_camera_pull_and_present(context_t *ctx);

/** Crop NV12 (x0/y0 must be even for chroma alignment). align_top: crop from top edge. */
static void nv12_crop_region(const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h,
                             uint8_t *dst, uint32_t dst_w, uint32_t dst_h, int align_top)
{
    uint32_t x0 = (src_w > dst_w) ? (src_w - dst_w) / 2u : 0u;
    uint32_t y0 = align_top ? 0u : ((src_h > dst_h) ? (src_h - dst_h) / 2u : 0u);
    const uint8_t *src_y = src;
    const uint8_t *src_uv = src + (size_t)src_stride * src_h;
    uint8_t *dst_y = dst;
    uint8_t *dst_uv = dst + (size_t)dst_w * dst_h;
    uint32_t y;

    if (x0 & 1u)
        x0--;
    if (y0 & 1u)
        y0--;

    for (y = 0; y < dst_h; y++)
        memcpy(dst_y + (size_t)y * dst_w, src_y + (size_t)(y0 + y) * src_stride + x0, dst_w);

    y0 /= 2u;
    for (y = 0; y < dst_h / 2u; y++)
        memcpy(dst_uv + (size_t)y * dst_w, src_uv + (size_t)(y0 + y) * src_stride + x0, dst_w);
}

/** Center-crop NV12 (x0/y0 must be even for chroma alignment). */
static void nv12_crop_center(const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h,
                              uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    nv12_crop_region(src, src_stride, src_w, src_h, dst, dst_w, dst_h, 0);
}

static int s_yuv_lut_ready;
static int16_t s_v_r_lut[256];
static int16_t s_u_g_lut[256];
static int16_t s_v_g_lut[256];
static int16_t s_u_b_lut[256];

static void nv12_init_yuv_lut(void)
{
    int i;

    if (s_yuv_lut_ready)
        return;

    for (i = 0; i < 256; i++) {
        int c = i - 128;
        s_v_r_lut[i] = (int16_t)((359 * c) >> 8);
        s_u_g_lut[i] = (int16_t)((88 * c) >> 8);
        s_v_g_lut[i] = (int16_t)((183 * c) >> 8);
        s_u_b_lut[i] = (int16_t)((454 * c) >> 8);
    }
    s_yuv_lut_ready = 1;
}

static inline uint16_t nv12_yuv_to_rgb565_fast(int y, int u_idx, int v_idx)
{
    int r = y + s_v_r_lut[v_idx];
    int g = y - s_u_g_lut[u_idx] - s_v_g_lut[v_idx];
    int b = y + s_u_b_lut[u_idx];
    uint16_t px;

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    px = (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                    ((uint16_t)(g & 0xFC) << 3) |
                    ((uint16_t)b >> 3));
    return lv_display_rgb565_pack_fb(px);
}

static void gui_camera_boost_refresh_rate(int boost)
{
    lv_display_t *disp = lv_display_get_default();
    lv_timer_t *refr = disp ? lv_display_get_refr_timer(disp) : NULL;

    if (!refr)
        return;

    if (boost) {
        if (!s_refr_period_saved) {
            s_saved_refr_period = (uint32_t)LV_DEF_REFR_PERIOD;
            s_refr_period_saved = 1;
        }
        lv_timer_set_period(refr, GUI_CAMERA_REFR_PERIOD_FAST);
    } else if (s_refr_period_saved) {
        lv_timer_set_period(refr, s_saved_refr_period);
        s_refr_period_saved = 0;
    }
}

/** Crop NV12/NV21 region and convert to RGB565 (software LCD path without GPU/IPU). */
static void nv12_crop_to_rgb565(const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h,
                                uint32_t dst_w, uint32_t dst_h, uint16_t *rgb_out, int nv21,
                                int align_top)
{
    uint32_t x0 = (src_w > dst_w) ? (src_w - dst_w) / 2u : 0u;
    uint32_t y0 = align_top ? 0u : ((src_h > dst_h) ? (src_h - dst_h) / 2u : 0u);
    const uint8_t *src_y = src;
    const uint8_t *src_uv = src + (size_t)src_stride * src_h;
    uint32_t row;

    if (x0 & 1u)
        x0--;
    if (y0 & 1u)
        y0--;

    nv12_init_yuv_lut();

    for (row = 0; row < dst_h; row++) {
        const uint8_t *yrow = src_y + (size_t)(y0 + row) * src_stride + x0;
        const uint8_t *uvrow = src_uv + (size_t)((y0 + row) / 2u) * src_stride + x0;
        uint16_t *dst = rgb_out + (size_t)row * dst_w;
        uint32_t col;

        for (col = 0; col + 1u < dst_w; col += 2u) {
            int u_idx;
            int v_idx;
            int y0v = (int)yrow[col];
            int y1v = (int)yrow[col + 1u];

            if (nv21) {
                v_idx = (int)uvrow[col & ~1u];
                u_idx = (int)uvrow[(col & ~1u) + 1u];
            } else {
                u_idx = (int)uvrow[col & ~1u];
                v_idx = (int)uvrow[(col & ~1u) + 1u];
            }
            dst[col] = nv12_yuv_to_rgb565_fast(y0v, u_idx, v_idx);
            dst[col + 1u] = nv12_yuv_to_rgb565_fast(y1v, u_idx, v_idx);
        }
        if (dst_w & 1u) {
            int u_idx;
            int v_idx;
            uint32_t col_last = dst_w - 1u;
            int yv = (int)yrow[col_last];

            if (nv21) {
                v_idx = (int)uvrow[col_last & ~1u];
                u_idx = (int)uvrow[(col_last & ~1u) + 1u];
            } else {
                u_idx = (int)uvrow[col_last & ~1u];
                v_idx = (int)uvrow[(col_last & ~1u) + 1u];
            }
            dst[col_last] = nv12_yuv_to_rgb565_fast(yv, u_idx, v_idx);
        }
    }
}

static void nv12_full_to_rgb565(const uint8_t *src, uint32_t stride, uint32_t w, uint32_t h,
                                uint16_t *rgb_out, int nv21)
{
    nv12_crop_to_rgb565(src, stride, w, h, w, h, rgb_out, nv21, 0);
}

int gui_camera_nv12_to_rgb565(const void *nv12, uint32_t width, uint32_t height,
                              int nv21, uint16_t *rgb_out)
{
    if (!nv12 || !rgb_out || width == 0 || height == 0)
        return -1;

    /* Packed NV12/NV21 (Y-plane stride == width) -> framebuffer-order RGB565.
     * Luna KM01W has no GPU/IPU, so NV12 cannot be blitted to the SW fb; callers
     * convert up front, the same path the live preview uses (preview_rgb565=1). */
    nv12_full_to_rgb565((const uint8_t *)nv12, width, width, height, rgb_out, nv21);
    return 0;
}

static uint32_t nv12_y_stride_from_frame(const media_frame_info_t *frame, uint32_t fallback_w,
                                         uint32_t fallback_h)
{
    uint32_t w = (uint32_t)frame->width;
    uint32_t h = (uint32_t)frame->height;

    if (w == 0 || h == 0) {
        w = fallback_w;
        h = fallback_h;
    }
    if (w == 0 || h == 0 || frame->data_len == 0)
        return w;

    {
        size_t plane = ((size_t)frame->data_len * 2u) / 3u;
        if (h > 0 && plane >= (size_t)w * (size_t)h) {
            uint32_t inferred = (uint32_t)(plane / h);
            if (inferred >= w)
                return inferred;
        }
    }
    return w;
}

static context_t context = {0};
static context_t *pContext = &context;
static gui_camera_opts_t s_camera_opts;

lv_obj_t *gui_camera_get_window(void)
{
    return pContext->window;
}

lv_obj_t *gui_camera_get_preview_image(void)
{
#if CONFIG_IMAGE_WIDGET
    return pContext->image;
#else
    return NULL;
#endif
}

/* NV12/NV21 semiplane preview: memcpy frame->data into a stable DMA buffer. */
static int apply_frame_to_ctx(context_t *ctx, media_frame_info_t *frame)
{
    if (frame == NULL || ctx == NULL || (ctx->image_buffer == NULL && ctx->image_buffer_back == NULL))
        return 0;
#if CONFIG_IMAGE_WIDGET
    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t src_stride;
    uint32_t dst_w = src_w;
    uint32_t dst_h = src_h;
    uint32_t src_nv_size;
    uint32_t dst_nv_size;
    uint32_t display_w = s_camera_opts.preview_display_w;
    uint32_t display_h = s_camera_opts.preview_display_h;
    int crop_for_lcd = 0;
    int align_top = (s_camera_opts.preview_align_top != 0);
    int nv21 = (s_camera_opts.preview_nv21 != 0);

    src_stride = nv12_y_stride_from_frame(frame, ctx->stream_w > 0 ? ctx->stream_w : ctx->preview_w,
                                          ctx->stream_h > 0 ? ctx->stream_h : ctx->preview_h);
    if (src_w == 0 || src_h == 0) {
        src_w = ctx->stream_w > 0 ? ctx->stream_w : src_stride;
        src_h = ctx->stream_h > 0 ? ctx->stream_h : ctx->preview_h;
    }
    src_nv_size = src_stride * src_h * 3u / 2u;
    dst_nv_size = src_nv_size;

    if (display_w > 0 && display_h > 0 &&
        src_w >= display_w && src_h >= display_h &&
        ((src_w - display_w) % 2u) == 0u && ((src_h - display_h) % 2u) == 0u)
    {
        dst_w = display_w;
        dst_h = display_h;
        crop_for_lcd = 1;
        dst_nv_size = dst_w * dst_h * 3u / 2u;
    }

    if ((src_w == 0 || src_h == 0) && frame->data_len > 0) {
        uint32_t infer_w = ctx->stream_w > 0 ? ctx->stream_w : ctx->preview_w;
        uint32_t infer_h = ctx->stream_h > 0 ? ctx->stream_h : ctx->preview_h;
        size_t need = (size_t)infer_w * (size_t)infer_h * 3u / 2u;

        if (infer_w > 0 && infer_h > 0 && frame->data_len >= need) {
            src_w = infer_w;
            src_h = infer_h;
            src_nv_size = (uint32_t)need;
        }
    }

    if ((frame->code_type != MEDIA_CODEC_VIDEO_NV12 && frame->code_type != 3) ||
        frame->data == NULL ||
        frame->data_len < src_nv_size)
    {
        static unsigned log_skip_once;
        if (log_skip_once == 0) {
            log_warn("[camera] frame skipped: code_type=%d (accept NV12=%d), data=%p, len=%u need=%u stride=%u %ux%u",
                     frame->code_type, MEDIA_CODEC_VIDEO_NV12, (void *)frame->data, (unsigned)frame->data_len,
                     src_nv_size, src_stride, src_w, src_h);
            log_skip_once = 1;
        }
        return 0;
    }

    if (s_preview_frame_cb)
        s_preview_frame_cb(frame, s_preview_frame_cb_user);

    if (frame->seq_no != 0 && frame->seq_no == ctx->last_seq)
        return 0;
    if (frame->seq_no != 0)
        ctx->last_seq = (uint32_t)frame->seq_no;

    /* Hardware output for preview is NV12 (Y + interleaved UV). */
    int use_rgb565 = (s_camera_opts.preview_rgb565 != 0);
    lv_color_format_t cf = lv_display_camera_preview_cf(use_rgb565);
    uint32_t buf_bytes = use_rgb565 ? (dst_w * dst_h * 2u) : dst_nv_size;

    /* Realloc preview buffers if display resolution changed */
    if (ctx->preview_w != dst_w || ctx->preview_h != dst_h) {
        if (gui_camera_alloc_preview_buffers(ctx, buf_bytes) != 0)
            return 0;
        ctx->preview_w = dst_w;
        ctx->preview_h = dst_h;
    }

    {
        void *write_buf = gui_camera_write_buf(ctx);

        if (use_rgb565) {
            uint16_t *rgb = (uint16_t *)write_buf;
            if (crop_for_lcd)
                nv12_crop_to_rgb565((const uint8_t *)frame->data, src_stride, src_w, src_h,
                                    dst_w, dst_h, rgb, nv21, align_top);
            else
                nv12_full_to_rgb565((const uint8_t *)frame->data, src_stride, src_w, src_h, rgb, nv21);
        } else if (crop_for_lcd) {
            nv12_crop_region((const uint8_t *)frame->data, src_stride, src_w, src_h,
                             (uint8_t *)write_buf, dst_w, dst_h, align_top);
        } else {
            memcpy(write_buf, frame->data, src_nv_size < frame->data_len ?
                   src_nv_size : frame->data_len);
        }
    }

    ctx->frame_token++;
    ctx->preview_frame_ready = 1;

    lv_image_dsc_t *image_dsc = &(ctx->image_dsc);
    image_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    image_dsc->header.cf     = cf;
    image_dsc->header.w      = (uint16_t)dst_w;
    image_dsc->header.h      = (uint16_t)dst_h;
    image_dsc->header.stride = use_rgb565 ? (uint16_t)(dst_w * 2u) : (uint16_t)((dst_w * 3u) / 2u);
    image_dsc->data_size     = buf_bytes;

    {
        static unsigned s_frames_applied;
        static uint32_t s_last_lcd_log_tick;

        s_frames_applied++;
        {
            uint32_t now = lv_tick_get();
            if (s_frames_applied == 1u || (now - s_last_lcd_log_tick) >= 5000u) {
                s_last_lcd_log_tick = now;
                log_debug("camera: preview lcd refresh %ux%u code_type=%d (#%u)",
                          dst_w, dst_h, frame->code_type, s_frames_applied);
            }
        }
    }
#endif
    return 1;
}

lv_result_t gui_camera_create(lv_obj_t *main)
{
    return gui_camera_create_ex(main, NULL);
}

lv_result_t gui_camera_create_ex(lv_obj_t *main, const gui_camera_opts_t *opts)
{
    memset(&s_camera_opts, 0, sizeof(s_camera_opts));
    if (opts)
        s_camera_opts = *opts;

    context_t *ctx = pContext;
    void *pv = pContext->image_buffer;
    void *pv_back = pContext->image_buffer_back;
    int pv_write = pContext->preview_buf_write;
    memset(ctx, 0, sizeof(context_t));
    pContext->image_buffer = pv;
    pContext->image_buffer_back = pv_back;
    pContext->preview_buf_write = pv_write;
    ctx->main = main;
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *screen = disp ? lv_display_get_screen_active(disp) : NULL;
    lv_obj_t *parent = (main != NULL) ? main : screen;
    if (!parent) return SYSTEM_FAILED;
    if (main != NULL) {
        lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(main, LV_OPA_TRANSP, LV_PART_MAIN);
    } else if (screen) {
        lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_t *window = lv_obj_create(parent);
    if (NULL == window) {
        log_error("camera: create window failed");
        goto ERROR;
    }
    ctx->window = window;
    lv_obj_remove_style_all(window);
    lv_obj_set_style_bg_opa(window, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(window, LV_HOR_RES, LV_VER_RES);
    /* Ensure cleanup even if app exits without using back button. */
    lv_obj_add_event_cb(window, camera_window_delete_event_cb, LV_EVENT_DELETE, (void *)ctx);
    g_multimedia = (multimedia_t *)app_main_get_multimedia();
    ctx->preview_w = (uint32_t)LV_HOR_RES;
    ctx->preview_h = (uint32_t)LV_VER_RES;
    gui_camera_streams_t cam_streams;
    memset(&cam_streams, 0, sizeof(cam_streams));
    cam_streams.idx_preview = -1;
    cam_streams.idx_main = -1;
    if (g_multimedia && gui_camera_streams_resolve(g_multimedia, &cam_streams) == 0 &&
        cam_streams.preview_w > 0 && cam_streams.preview_h > 0) {
        if (s_camera_opts.preview_display_w > 0 && s_camera_opts.preview_display_h > 0) {
            ctx->preview_w = (uint32_t)s_camera_opts.preview_display_w;
            ctx->preview_h = (uint32_t)s_camera_opts.preview_display_h;
        } else {
            ctx->preview_w = (uint32_t)cam_streams.preview_w;
            ctx->preview_h = (uint32_t)cam_streams.preview_h;
        }
        ctx->stream_w = (uint32_t)cam_streams.preview_w;
        ctx->stream_h = (uint32_t)cam_streams.preview_h;
    } else if (s_camera_opts.preview_display_w > 0 && s_camera_opts.preview_display_h > 0) {
        ctx->preview_w = (uint32_t)s_camera_opts.preview_display_w;
        ctx->preview_h = (uint32_t)s_camera_opts.preview_display_h;
    }
#if CONFIG_IMAGE_WIDGET
    memset(&ctx->image_dsc, 0, sizeof(ctx->image_dsc));
    lv_obj_t * image = lv_image_create(window);
    if (NULL == image) {
        log_error("camera: create preview image failed");
        goto ERROR;
    }
    lv_obj_remove_style_all(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    if (s_camera_opts.preview_display_w > 0 && s_camera_opts.preview_display_h > 0) {
        lv_obj_set_size(image, (int32_t)s_camera_opts.preview_display_w,
                        (int32_t)s_camera_opts.preview_display_h);
    } else {
        lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
    }
    lv_image_set_inner_align(image, s_camera_opts.preview_cover ?
                             LV_IMAGE_ALIGN_COVER : LV_IMAGE_ALIGN_CONTAIN);
    if (s_camera_opts.preview_y_offset > 0) {
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, (int32_t)s_camera_opts.preview_y_offset);
    } else if (s_camera_opts.preview_align_top) {
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);
    } else {
        lv_obj_center(image);
    }
    ctx->image = image;
    uint32_t width = ctx->preview_w;
    uint32_t height = ctx->preview_h;
    int use_rgb565 = (s_camera_opts.preview_rgb565 != 0);
    uint32_t size = use_rgb565 ? (width * height * 2u) : (width * height * 3u / 2u);
    lv_color_format_t init_cf = lv_display_camera_preview_cf(use_rgb565);
    if (NULL == ctx->image_buffer || NULL == ctx->image_buffer_back) {
        if (gui_camera_alloc_preview_buffers(ctx, size) != 0) {
            log_error("camera: preview buffer alloc failed (%ux%u)", width, height);
            return SYSTEM_FAILED;
        }
    }
    memset(ctx->image_buffer, 0, size);
    memset(ctx->image_buffer_back, 0, size);
    ctx->image_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    ctx->image_dsc.header.cf     = init_cf;
    ctx->image_dsc.header.w      = (uint16_t)width;
    ctx->image_dsc.header.h      = (uint16_t)height;
    ctx->image_dsc.header.stride = use_rgb565 ? (uint16_t)(width * 2u) : (uint16_t)((width * 3u) / 2u);
    ctx->image_dsc.data          = (const uint8_t *)ctx->image_buffer;
    ctx->image_dsc.data_size     = size;
    lv_image_set_src(image, &ctx->image_dsc);
#endif
    if (!s_camera_opts.hide_top_bar) {
        lv_obj_t *top = gui_create_top(window, lan("Camera"), event_handler_btn_back, (void *)ctx);
        if (NULL == top)
            goto ERROR;
    }
    lv_obj_invalidate(window);
    ctx->state = GUI_PLAY_STATUS_PLAYING;
    ctx->stream_idx_preview = cam_streams.idx_preview;
    ctx->stream_idx_main = cam_streams.idx_main;
    ctx->preview_started = 0;
    client_handle = INVALID_HANDLE;
    log_info("camera: preview UI ready window=%p image=%p buf=%p stream=%ux%u display=%ux%u",
             (void *)window, (void *)ctx->image, ctx->image_buffer,
             (unsigned)ctx->stream_w, (unsigned)ctx->stream_h,
             (unsigned)ctx->preview_w, (unsigned)ctx->preview_h);
    fflush(stdout);
    lv_obj_invalidate(window);
    if (s_camera_opts.out_window)
        *s_camera_opts.out_window = window;
    return LV_RESULT_OK;
ERROR:
    show_msgbox_note("Start video device fail");
    if (screen) lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    if (main) lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);
    if (ctx->timer != NULL) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    if (NULL != window) lv_obj_delete(window);
    memset((void *)ctx, 0, sizeof(context_t));
    return LV_RESULT_INVALID;
}

int gui_camera_start_preview(void)
{
    context_t *ctx = pContext;

    if (!ctx || !ctx->window || ctx->state != GUI_PLAY_STATUS_PLAYING)
        return -1;
    if (ctx->preview_started)
        return 0;

    if (!g_multimedia)
        g_multimedia = (multimedia_t *)app_main_get_multimedia();
    if (!g_multimedia || !g_multimedia->camera) {
        log_error("camera: multimedia camera ops missing");
        return -1;
    }

    client_handle = INVALID_HANDLE;
    {
        int ret = -1;
        int stream = -1;

        if (ctx->stream_idx_preview >= 0) {
            ret = g_multimedia->camera->stream_reg_client(ctx->stream_idx_preview, &client_handle,
                                                          NULL, NULL);
            stream = ctx->stream_idx_preview;
        }
        if (ret != 0 && ctx->stream_idx_main >= 0) {
            ret = g_multimedia->camera->stream_reg_client(ctx->stream_idx_main, &client_handle,
                                                          NULL, NULL);
            stream = ctx->stream_idx_main;
        }
        if (ret != 0) {
            log_error("camera: stream_reg_client failed (preview=%d main=%d)",
                      ctx->stream_idx_preview, ctx->stream_idx_main);
            return -1;
        }
        log_info("camera: started preview stream %d (pull%s) handle=0x%x; main VI keepalive via framework (no VENC until record)",
                 stream, s_camera_opts.preview_rgb565 ? ", rgb565" : "", client_handle);
    }

    if (ctx->timer == NULL) {
        lv_timer_t *timer = lv_timer_create(timing_task_timer, GUI_CAMERA_PREVIEW_TIMER_MS, (void *)ctx);
        if (timer == NULL) {
            log_error("camera: preview timer create failed");
            if (g_multimedia->camera && client_handle != INVALID_HANDLE) {
                g_multimedia->camera->stream_unreg_client(client_handle);
                client_handle = INVALID_HANDLE;
            }
            return -1;
        }
        ctx->timer = timer;
        log_info("camera: preview timer ready period=%ums", (unsigned)GUI_CAMERA_PREVIEW_TIMER_MS);
    }

    ctx->preview_started = 1;
    gui_camera_boost_refresh_rate(1);
    fflush(stdout);
    return 0;
}

void gui_camera_restart_preview_streams(void)
{
    context_t *ctx = pContext;
    lv_timer_t *old_timer = NULL;

    if (ctx) {
        ctx->preview_pull_suspend = 1;
        old_timer = ctx->timer;
        ctx->timer = NULL;
    }

    if (old_timer)
        lv_timer_delete(old_timer);

    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
        log_info("camera: preview client unregistered for restart");
    }

    if (ctx) {
        ctx->preview_started = 0;
        ctx->preview_frame_ready = 0;
        ctx->last_seq = 0;
        ctx->preview_last_present_ptr = NULL;
    }

    if (ctx && ctx->state == GUI_PLAY_STATUS_PLAYING)
        (void)gui_camera_start_preview();

    if (ctx)
        ctx->preview_pull_suspend = 0;

    log_info("camera: preview streams restarted");
}

void gui_camera_stop_preview_for_capture(void)
{
    context_t *ctx = pContext;
    lv_timer_t *old_timer = NULL;

    if (ctx) {
        ctx->preview_pull_suspend = 1;
        ctx->preview_present_hold = 1;
        old_timer = ctx->timer;
        ctx->timer = NULL;
    }

    if (old_timer)
        lv_timer_delete(old_timer);

    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
        log_info("camera: preview stopped for capture (client unregistered, VI idle)");
    }

    if (ctx) {
        ctx->preview_started = 0;
        ctx->preview_frame_ready = 0;
        ctx->last_seq = 0;
        ctx->preview_last_present_ptr = NULL;
    }
}

void gui_camera_shutdown(void)
{
    context_t *ctx = pContext;

    if (!ctx || !ctx->window)
        return;

    ctx->state = GUI_PLAY_STATUS_CLOSE;
    gui_camera_set_preview_frame_cb(NULL, NULL);
    gui_camera_boost_refresh_rate(0);

    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
        log_info("camera: stream_unreg_client (preview shutdown)");
    }
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
#if CONFIG_IMAGE_WIDGET
    gui_camera_free_preview_buffers(ctx);
#endif
    lv_obj_add_flag(ctx->window, LV_OBJ_FLAG_HIDDEN);

    /* Drop cached widget pointers: the caller deletes the window right after
     * shutdown, so keep this idempotent and never invalidate a stale object. */
    ctx->image = NULL;
    ctx->window = NULL;
    ctx->preview_last_present_ptr = NULL;
}

void gui_camera_refresh_preview(void)
{
    context_t *ctx = pContext;

    if (!ctx || !ctx->window || ctx->state != GUI_PLAY_STATUS_PLAYING)
        return;
    if (!ctx->preview_started && gui_camera_start_preview() != 0)
        return;
    log_info("camera: manual refresh requested handle=0x%x", client_handle);
    if (gui_camera_pull_and_present(ctx))
        return;
    log_warn("camera: manual refresh no frame yet");
    lv_obj_invalidate(ctx->window);
}

void gui_camera_set_preview_paused(int paused)
{
    context_t *ctx = pContext;

    if (!ctx)
        return;
    /* Hold LVGL present only; keep pull timer running to drain preview VI (avoid NoBuf stall). */
    ctx->preview_present_hold = paused ? 1 : 0;
}

void gui_camera_set_preview_pull_suspend(int suspend)
{
    context_t *ctx = pContext;

    if (!ctx)
        return;
    ctx->preview_pull_suspend = suspend ? 1 : 0;
}

int gui_camera_grab_nv12_frame(void *buf, size_t buf_size, uint32_t *width, uint32_t *height)
{
    context_t *ctx = pContext;
    media_frame_info_t frame = {0};
    uint32_t w;
    uint32_t h;
    uint32_t stride;
    size_t need;
    int tries;

    if (!buf || !width || !height || !g_multimedia || !g_multimedia->camera ||
        client_handle == INVALID_HANDLE)
        return -1;

    if (ctx && ctx->preview_pull_suspend)
        return -1;

    for (tries = 0; tries < 50; tries++) {
        memset(&frame, 0, sizeof(frame));
        if (g_multimedia->camera->stream_get_frame(client_handle, &frame) != 0 ||
            frame.data == NULL || frame.width <= 0 || frame.height <= 0) {
            system_sleep_ms(10);
            continue;
        }
        if (frame.code_type != MEDIA_CODEC_VIDEO_NV12 && frame.code_type != 3)
            continue;

        w = (uint32_t)frame.width;
        h = (uint32_t)frame.height;
        stride = (ctx && ctx->stream_w > 0) ? (uint32_t)ctx->stream_w : w;
        need = (size_t)stride * (size_t)h * 3u / 2u;
        if (frame.data_len < need || buf_size < need)
            return -1;

        memcpy(buf, frame.data, need);
        *width = w;
        *height = h;
        return 0;
    }

    return -1;
}

void gui_camera_set_preview_frame_cb(gui_camera_preview_frame_cb_t cb, void *user_data)
{
    s_preview_frame_cb = cb;
    s_preview_frame_cb_user = user_data;
}

static void gui_camera_present_frame(context_t *ctx)
{
#if CONFIG_IMAGE_WIDGET
    lv_obj_t *image;
    lv_image_dsc_t *image_dsc;

    if (!ctx)
        return;
    if (ctx->preview_present_hold)
        return;
    image = ctx->image;
    image_dsc = &(ctx->image_dsc);
    if (!image || !image_dsc || !image_dsc->data || image_dsc->header.w == 0 ||
        image_dsc->header.h == 0) {
        return;
    }
    /* Double-buffer: alternate display pointer; rebind src only when buffer swaps. */
    if (!ctx->preview_frame_ready)
        return;

    image_dsc->data = (const uint8_t *)gui_camera_display_buf(ctx);
    if (ctx->preview_last_present_ptr != image_dsc->data) {
        lv_image_set_src(image, image_dsc);
        ctx->preview_last_present_ptr = image_dsc->data;
    } else {
        lv_image_cache_drop(image_dsc);
    }
    lv_obj_invalidate(image);
    ctx->preview_buf_write ^= 1;
    ctx->preview_frame_ready = 0;
#endif
}

static int gui_camera_pull_and_present(context_t *ctx)
{
    media_frame_info_t frame = {0};
    int gf;

    if (!ctx || !g_multimedia || !g_multimedia->camera || client_handle == INVALID_HANDLE)
        return 0;

    if (ctx->preview_pull_suspend)
        return 0;

    {
        static unsigned s_manual_pull_ctr;
        s_manual_pull_ctr++;
        if (s_manual_pull_ctr <= 3u)
            log_info("camera: pull begin #%u handle=0x%x", s_manual_pull_ctr, client_handle);
    }
    gf = g_multimedia->camera->stream_get_frame(client_handle, &frame);
    {
        static unsigned s_manual_pull_done_ctr;
        s_manual_pull_done_ctr++;
        if (s_manual_pull_done_ctr <= 3u || gf == 0) {
            log_info("camera: pull done #%u ret=%d data=%p len=%u %ux%u code=%d seq=%u",
                     s_manual_pull_done_ctr, gf, (void *)frame.data, (unsigned)frame.data_len,
                     (unsigned)frame.width, (unsigned)frame.height, frame.code_type,
                     (unsigned)frame.seq_no);
        }
    }
    if (gf != 0 || !apply_frame_to_ctx(ctx, &frame))
        return 0;

    gui_camera_present_frame(ctx);
    return 1;
}

static void timing_task_timer(lv_timer_t *timer)
{
    context_t *ctx = (context_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) return;
    if (GUI_PLAY_STATUS_PLAYING != ctx->state) return;
    lv_obj_t *window = ctx->window;
    if (NULL == window) return;

    if (ctx->preview_pull_suspend)
        return;

    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        media_frame_info_t frame = {0};
        static unsigned s_timer_fire;
        int gf;
        int applied = 0;
        unsigned int burst;

        s_timer_fire++;
        if (s_timer_fire <= 3u || s_timer_fire == 30u)
            log_info("camera: preview timer fire #%u handle=0x%x hold=%d",
                     s_timer_fire, client_handle, ctx->preview_present_hold);

        if (ctx->preview_present_hold) {
            for (burst = 0; burst < GUI_CAMERA_PULL_BURST; burst++) {
                gf = g_multimedia->camera->stream_get_frame(client_handle, &frame);
                if (gf != 0)
                    break;
            }
            return;
        }

        for (burst = 0; burst < GUI_CAMERA_PULL_BURST; burst++) {
            gf = g_multimedia->camera->stream_get_frame(client_handle, &frame);
            if (gf != 0)
                break;
            if (apply_frame_to_ctx(ctx, &frame))
                applied = 1;
        }

        if (applied) {
            gui_camera_present_frame(ctx);
        } else if (burst == 0) {
            static unsigned s_pull_miss;

            s_pull_miss++;
            if (s_pull_miss == 1u || s_pull_miss == 30u) {
                log_warn("camera: preview pull miss #%u get_frame=%d w=%u h=%u code=%d len=%u",
                         s_pull_miss, gf, (unsigned)frame.width, (unsigned)frame.height,
                         frame.code_type, (unsigned)frame.data_len);
            }
        }
    }
}

static void event_handler_btn_back(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    if (ctx == NULL) return;
    if (s_camera_opts.on_back) {
        s_camera_opts.on_back(ctx->main, s_camera_opts.user_data);
        return;
    }
    ctx->state = GUI_PLAY_STATUS_CLOSE;
    if (g_multimedia && g_multimedia->camera && client_handle != INVALID_HANDLE) {
        g_multimedia->camera->stream_unreg_client(client_handle);
        client_handle = INVALID_HANDLE;
        log_info("camera: stream_unreg_client (preview shutdown)");
    }
    if (ctx->timer != NULL) { lv_timer_delete(ctx->timer); ctx->timer = NULL; }
#if CONFIG_IMAGE_WIDGET
    gui_camera_free_preview_buffers(ctx);
#endif
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (screen) lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(ctx->main);
    lv_obj_remove_flag(ctx->main, LV_OBJ_FLAG_HIDDEN);
    if (ctx->window != NULL) { lv_obj_delete(ctx->window); ctx->window = NULL; }
}

static void event_handler_btn_play(lv_event_t *e)
{
    context_t *ctx = (context_t *)lv_event_get_user_data(e);
    lv_obj_t *label = ctx->ctrl.play.label;
    if (GUI_PLAY_STATUS_PLAYING == ctx->state) {
        ctx->state = GUI_PLAY_STATUS_PAUSE;
        lv_label_set_text(label, LV_SYMBOL_PLAY);
    } else {
        ctx->state = GUI_PLAY_STATUS_PLAYING;
        lv_label_set_text(label, LV_SYMBOL_PAUSE);
    }
}

static void event_handler_btn_capture(lv_event_t *e)
{
    botton_t *capture = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_opa(capture->director, LV_OPA_10, 0);
        lv_obj_invalidate(capture->director);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_set_style_bg_opa(capture->director, LV_OPA_COVER, 0);
        lv_obj_invalidate(capture->director);
    }
}

#endif /* APP_GUI_CAMERA */
