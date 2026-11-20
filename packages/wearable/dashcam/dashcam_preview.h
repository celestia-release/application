// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * LCD preview extension for Luna dashcam.
 *
 * First release ships a stub (no-op) implementation: the dashcam runs purely as
 * a background recorder + RTSP server. The interface is kept stable so a future
 * release can enable LCD preview by setting CONFIG_APP_WEARABLE_DASHCAM_PREVIEW=y in
 * config.mk. The preview channel (role=preview, 256x192 NV12) is defined in
 * fw_video_config.json alongside main/sub H.265 encode streams.
 */
#ifndef WEARABLE_DASHCAM_PREVIEW_H
#define WEARABLE_DASHCAM_PREVIEW_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @return 1 when preview support is compiled in, 0 for the stub build. */
int dashcam_preview_available(void);

/** Bind the reserved preview canvas (created by the UI). */
int dashcam_preview_init(lv_obj_t *canvas);

/** Begin rendering the preview NV12 stream into the bound canvas. */
int dashcam_preview_start(void);

/** Stop rendering. */
void dashcam_preview_stop(void);

/** Release preview resources; the canvas itself is owned by the UI. */
void dashcam_preview_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_DASHCAM_PREVIEW_H */
