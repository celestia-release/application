// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Shared LVGL camera preview API.
 */
#ifndef APPLICATION_GUI_CAMERA_H
#define APPLICATION_GUI_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gui_common.h"
#include "multimedia.h"

#if APP_GUI_CAMERA

typedef void (*gui_camera_back_cb_t)(lv_obj_t *main, void *user_data);

/** Raw preview frame hook (NV12); called on LVGL thread before RGB565 conversion. */
typedef void (*gui_camera_preview_frame_cb_t)(const media_frame_info_t *frame, void *user_data);

typedef struct {
    gui_camera_back_cb_t on_back;
    void *user_data;
    lv_obj_t **out_window;
    int hide_top_bar;
    /** 1: use COVER (center crop) for 1:1 LCD; 0: CONTAIN (letterbox). */
    int preview_cover;
    /** 1: crop from top (not center) when scaling to display; preview flush to top. */
    int preview_align_top;
    /** LCD display size cropped from preview NV12 (0 = show full stream frame). */
    uint16_t preview_display_w;
    uint16_t preview_display_h;
    /** Preview image top offset from window (0 = center vertically). */
    uint16_t preview_y_offset;
    /** 1: convert NV12 to RGB565 for software fb (no GPU/IPU on Luna KM01W). */
    int preview_rgb565;
    /** 1: chroma is NV21 (VU) instead of NV12 (UV); AK VI YUV420SP is often NV21. */
    int preview_nv21;
} gui_camera_opts_t;

lv_result_t gui_camera_create(lv_obj_t *main);
lv_result_t gui_camera_create_ex(lv_obj_t *main, const gui_camera_opts_t *opts);
/** Register preview stream client and start pull timer (after UI/overlay is ready). */
int gui_camera_start_preview(void);
void gui_camera_shutdown(void);
void gui_camera_refresh_preview(void);
void gui_camera_set_preview_paused(int paused);
/** Suspend preview stream_get_frame (during photo capture / stream restart). */
void gui_camera_set_preview_pull_suspend(int suspend);
/** Copy one NV12 preview frame using the existing preview stream client (no extra reg_client). */
int gui_camera_grab_nv12_frame(void *buf, size_t buf_size, uint32_t *width, uint32_t *height);
/**
 * Convert a packed NV12/NV21 frame (Y-plane stride == width) to framebuffer-order
 * RGB565. rgb_out must hold width*height uint16_t. Lets the GPU-less Luna LCD show
 * NV12 frames (e.g. gallery photo/video preview). Returns 0 on success.
 */
int gui_camera_nv12_to_rgb565(const void *nv12, uint32_t width, uint32_t height,
                              int nv21, uint16_t *rgb_out);
/** Hook invoked for each NV12 preview frame while preview is running. */
void gui_camera_set_preview_frame_cb(gui_camera_preview_frame_cb_t cb, void *user_data);
/** After capture_picture hijacks channel 0, restart VI streams for preview to continue. */
void gui_camera_restart_preview_streams(void);
/** Stop preview client/timer and idle VI before capture/record (LVGL thread only). */
void gui_camera_stop_preview_for_capture(void);
/** When paused (hold=1), preview frames are still pulled/drained but not shown on LCD. */
lv_obj_t *gui_camera_get_window(void);
lv_obj_t *gui_camera_get_preview_image(void);

#endif

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_GUI_CAMERA_H */
