// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Video control widget for camera preview window.
 */
#ifndef APPLICATION_TABLET_VIDEO_CTRL_H
#define APPLICATION_TABLET_VIDEO_CTRL_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create camera preview window (uses default stream 0).
 * @param main Parent LVGL object.
 * @return LV_RESULT_OK on success.
 */
lv_result_t tablet_video_ctrl_create(lv_obj_t *main);

/**
 * @brief QR/settings preview: preview stream NV12 (LCD_HOR x LCD_VER), reg_client + YUV->RGB in callback.
 * @param stream_id Ignored; always uses stream 1 (kept for API compatibility).
 */
lv_result_t tablet_video_ctrl_create_for_stream(lv_obj_t *main, int stream_id);

/**
 * @brief Close camera preview window and release resources.
 * @return LV_RESULT_OK on success.
 */
lv_result_t tablet_video_ctrl_close(void);

/**
 * @brief Copy latest NV12 preview frame (the same frame shown on screen).
 *
 * Used by QR pairing to scan QR code from preview image, without registering
 * an extra camera client / stream_get_frame reader.
 *
 * @param dst       Output buffer to copy NV12 into.
 * @param dst_len   Size of dst in bytes.
 * @param out_w     Optional: returns frame width.
 * @param out_h     Optional: returns frame height.
 * @param inout_seq in: last seen seq; out: current seq if copied.
 * @return 1 if a newer frame was copied, 0 if no newer frame, -1 on error.
 */
int tablet_video_ctrl_copy_latest_nv12(uint8_t *dst, size_t dst_len,
                                  int *out_w, int *out_h, uint32_t *inout_seq);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_TABLET_VIDEO_CTRL_H */
