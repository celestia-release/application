// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * LCD preview for Luna dashcam: NV12 preview channel (256x192) -> RGB565 canvas.
 * Mirrors the camera app pull-timer path (gui_camera) without owning a full-screen window.
 */
#define LOG_TAG "WEARABLE_DASHCAM_PREVIEW"

#include "dashcam_preview.h"

#include "app_main.h"
#include "gui_camera.h"
#include "gui_common.h"
#include "log.h"
#include "lv_display_config.h"
#include "multimedia.h"
#include "system_memory.h"

#include <string.h>

#if defined(CONFIG_APP_WEARABLE_DASHCAM_PREVIEW) && CONFIG_APP_WEARABLE_DASHCAM_PREVIEW

#define DASHCAM_PREVIEW_TIMER_MS    40u
#define DASHCAM_PREVIEW_INVALID     (-1)

static lv_obj_t *s_canvas;
static lv_obj_t *s_image;
static lv_image_dsc_t s_image_dsc;
static void *s_rgb_front;
static void *s_rgb_back;
static int s_rgb_write_back;
static lv_timer_t *s_timer;
static int s_client_handle = DASHCAM_PREVIEW_INVALID;
static int s_stream_idx = DASHCAM_PREVIEW_INVALID;
static uint32_t s_stream_w;
static uint32_t s_stream_h;
static uint32_t s_display_w;
static uint32_t s_display_h;
static uint32_t s_last_seq;
static multimedia_t *s_mm;

static void *dashcam_preview_write_rgb(void)
{
    return s_rgb_write_back ? s_rgb_back : s_rgb_front;
}

static void *dashcam_preview_read_rgb(void)
{
    return s_rgb_write_back ? s_rgb_front : s_rgb_back;
}

static void dashcam_preview_free_rgb(void)
{
    if (s_rgb_front) {
        system_free(s_rgb_front);
        s_rgb_front = NULL;
    }
    if (s_rgb_back) {
        system_free(s_rgb_back);
        s_rgb_back = NULL;
    }
    s_rgb_write_back = 0;
}

static int dashcam_preview_alloc_rgb(uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * (size_t)h * 2u;

    dashcam_preview_free_rgb();
    s_rgb_front = system_malloc(bytes);
    s_rgb_back = system_malloc(bytes);
    if (!s_rgb_front || !s_rgb_back) {
        dashcam_preview_free_rgb();
        return -1;
    }
    memset(s_rgb_front, 0, bytes);
    memset(s_rgb_back, 0, bytes);
    s_display_w = w;
    s_display_h = h;
    return 0;
}

static uint32_t dashcam_preview_nv12_stride(const media_frame_info_t *frame)
{
    uint32_t w = (uint32_t)frame->width;
    uint32_t h = (uint32_t)frame->height;

    if (w == 0 || h == 0 || frame->data_len == 0)
        return w > 0 ? w : s_stream_w;

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

static int dashcam_preview_apply_frame(media_frame_info_t *frame)
{
    uint32_t src_w;
    uint32_t src_h;
    uint32_t src_stride;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t need_nv12;
    uint16_t *rgb;
    lv_coord_t canvas_w;
    lv_coord_t canvas_h;

    if (!frame || !frame->data || frame->width <= 0 || frame->height <= 0)
        return 0;

    if (frame->code_type != MEDIA_CODEC_VIDEO_NV12 && frame->code_type != 3)
        return 0;

    if (frame->seq_no != 0 && (uint32_t)frame->seq_no == s_last_seq)
        return 0;
    if (frame->seq_no != 0)
        s_last_seq = (uint32_t)frame->seq_no;

    src_w = (uint32_t)frame->width;
    src_h = (uint32_t)frame->height;
    src_stride = dashcam_preview_nv12_stride(frame);
    need_nv12 = src_stride * src_h * 3u / 2u;
    if (frame->data_len < need_nv12)
        return 0;

    canvas_w = s_canvas ? lv_obj_get_width(s_canvas) : 0;
    canvas_h = s_canvas ? lv_obj_get_height(s_canvas) : 0;
    if (canvas_w <= 0 || canvas_h <= 0)
        return 0;

    dst_w = (uint32_t)canvas_w;
    dst_h = (uint32_t)canvas_h;
    if (src_w >= dst_w && src_h >= dst_h &&
        ((src_w - dst_w) % 2u) == 0u && ((src_h - dst_h) % 2u) == 0u) {
        /* Top-aligned center crop like camera app (256x192 -> canvas). */
    } else {
        dst_w = src_w;
        dst_h = src_h;
    }

    if (s_display_w != dst_w || s_display_h != dst_h) {
        if (dashcam_preview_alloc_rgb(dst_w, dst_h) != 0)
            return 0;
        memset(&s_image_dsc, 0, sizeof(s_image_dsc));
        s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_image_dsc.header.cf = lv_display_rgb565_format();
        s_image_dsc.header.w = (uint16_t)dst_w;
        s_image_dsc.header.h = (uint16_t)dst_h;
        s_image_dsc.header.stride = (uint16_t)(dst_w * 2u);
    }

    rgb = (uint16_t *)dashcam_preview_write_rgb();
    if (dst_w == src_w && dst_h == src_h) {
        /* nv21=0: match luna camera preview (UV order), not NV21. */
        if (gui_camera_nv12_to_rgb565(frame->data, src_w, src_h, 0, rgb) != 0)
            return 0;
    } else {
        /* Crop then convert: reuse gui_camera on packed crop buffer when sizes match. */
        uint32_t x0 = (src_w > dst_w) ? (src_w - dst_w) / 2u : 0u;
        uint32_t y0 = 0u;
        uint8_t *crop_nv12;
        size_t crop_bytes = (size_t)dst_w * (size_t)dst_h * 3u / 2u;
        const uint8_t *src = (const uint8_t *)frame->data;
        const uint8_t *src_y = src;
        const uint8_t *src_uv = src + (size_t)src_stride * src_h;
        uint8_t *dst_y;
        uint8_t *dst_uv;
        uint32_t row;

        if (x0 & 1u)
            x0--;
        crop_nv12 = (uint8_t *)system_malloc(crop_bytes);
        if (!crop_nv12)
            return 0;
        dst_y = crop_nv12;
        dst_uv = crop_nv12 + (size_t)dst_w * dst_h;
        for (row = 0; row < dst_h; row++)
            memcpy(dst_y + (size_t)row * dst_w,
                   src_y + (size_t)(y0 + row) * src_stride + x0, dst_w);
        for (row = 0; row < dst_h / 2u; row++)
            memcpy(dst_uv + (size_t)row * dst_w,
                   src_uv + (size_t)(y0 / 2u + row) * src_stride + x0, dst_w);
        if (gui_camera_nv12_to_rgb565(crop_nv12, dst_w, dst_h, 0, rgb) != 0) {
            system_free(crop_nv12);
            return 0;
        }
        system_free(crop_nv12);
    }

    s_image_dsc.data = (const uint8_t *)rgb;
    s_image_dsc.data_size = (uint32_t)dst_w * dst_h * 2u;
    s_rgb_write_back ^= 1;
    return 1;
}

static void dashcam_preview_present(void)
{
    if (!s_image || !s_image_dsc.data || s_image_dsc.header.w == 0)
        return;

    s_image_dsc.data = (const uint8_t *)dashcam_preview_read_rgb();
    lv_image_set_src(s_image, &s_image_dsc);
    lv_obj_invalidate(s_image);
}

static void dashcam_preview_timer_cb(lv_timer_t *timer)
{
    media_frame_info_t frame = {0};
    int pulled = 0;

    (void)timer;

    if (!s_mm || !s_mm->camera || s_client_handle == DASHCAM_PREVIEW_INVALID)
        return;

    memset(&frame, 0, sizeof(frame));
    if (s_mm->camera->stream_get_frame(s_client_handle, &frame) != 0)
        return;

    if (dashcam_preview_apply_frame(&frame))
        pulled = 1;

    if (pulled)
        dashcam_preview_present();
}

static void dashcam_preview_unregister_client(void)
{
    if (s_mm && s_mm->camera && s_client_handle != DASHCAM_PREVIEW_INVALID) {
        s_mm->camera->stream_unreg_client(s_client_handle);
        log_info("preview: stream_unreg_client handle=0x%x", s_client_handle);
    }
    s_client_handle = DASHCAM_PREVIEW_INVALID;
}

int dashcam_preview_available(void)
{
    return 1;
}

int dashcam_preview_init(lv_obj_t *canvas)
{
    gui_camera_streams_t streams;

    /* Re-entry: drop any leftover LVGL/timer/client state from a prior session. */
    dashcam_preview_deinit();

    s_canvas = canvas;
    s_stream_idx = DASHCAM_PREVIEW_INVALID;
    s_last_seq = 0;
    s_mm = NULL;

    if (!canvas)
        return -1;

    s_mm = (multimedia_t *)app_main_get_multimedia();
    if (!s_mm || !s_mm->camera) {
        log_warn("preview: multimedia/camera unavailable");
        return -1;
    }

    if (gui_camera_streams_resolve(s_mm, &streams) != 0 || streams.idx_preview < 0) {
        log_warn("preview: no preview stream in dashcam config");
        return -1;
    }

    s_stream_idx = streams.idx_preview;
    s_stream_w = streams.preview_w;
    s_stream_h = streams.preview_h;

    s_image = lv_image_create(canvas);
    if (!s_image) {
        log_warn("preview: lv_image_create failed");
        return -1;
    }
    lv_obj_remove_style_all(s_image);
    lv_obj_set_size(s_image, lv_pct(100), lv_pct(100));
    lv_obj_center(s_image);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_SCROLLABLE);

    log_info("preview: init canvas=%p stream=%d %ux%u",
             (void *)canvas, s_stream_idx, s_stream_w, s_stream_h);
    return 0;
}

int dashcam_preview_start(void)
{
    int ret;

    if (!s_canvas || !s_mm || !s_mm->camera || s_stream_idx < 0)
        return -1;
    if (s_client_handle != DASHCAM_PREVIEW_INVALID)
        return 0;

    s_client_handle = DASHCAM_PREVIEW_INVALID;
    ret = s_mm->camera->stream_reg_client(s_stream_idx, &s_client_handle, NULL, NULL);
    if (ret != 0 || s_client_handle == DASHCAM_PREVIEW_INVALID) {
        log_warn("preview: stream_reg_client(stream=%d) failed ret=%d",
                 s_stream_idx, ret);
        s_client_handle = DASHCAM_PREVIEW_INVALID;
        return -1;
    }

    if (!s_timer) {
        s_timer = lv_timer_create(dashcam_preview_timer_cb, DASHCAM_PREVIEW_TIMER_MS, NULL);
        if (!s_timer) {
            dashcam_preview_unregister_client();
            return -1;
        }
    }

    log_info("preview: started stream=%d handle=0x%x (NV12 pull -> RGB565)",
             s_stream_idx, s_client_handle);
    return 0;
}

void dashcam_preview_stop(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    dashcam_preview_unregister_client();
    s_last_seq = 0;
}

void dashcam_preview_deinit(void)
{
    dashcam_preview_stop();

    if (s_image) {
        if (lv_obj_is_valid(s_image))
            lv_obj_delete(s_image);
        s_image = NULL;
    }
    dashcam_preview_free_rgb();
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    s_canvas = NULL;
    s_stream_idx = DASHCAM_PREVIEW_INVALID;
    s_stream_w = 0;
    s_stream_h = 0;
    s_display_w = 0;
    s_display_h = 0;
    s_last_seq = 0;
    s_mm = NULL;
}

#else /* stub build */

int dashcam_preview_available(void) { return 0; }
int dashcam_preview_init(lv_obj_t *canvas) { (void)canvas; return 0; }
int dashcam_preview_start(void) { return 0; }
void dashcam_preview_stop(void) { }
void dashcam_preview_deinit(void) { }

#endif
