// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Lifecycle coordinator for the Luna dashcam app: drives loop recording,
 * the RTSP server and (optionally) preview using fw_video_config.json.
 */
#ifndef WEARABLE_DASHCAM_CORE_H
#define WEARABLE_DASHCAM_CORE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Unified video layout in fw_video_config.json (main + sub + preview + picture). */

/**
 * @brief Start the dashcam pipeline (record + rtsp + optional preview).
 * @param preview_canvas Reserved canvas for future LCD preview (may be NULL).
 * @return 0 on success, negative on failure.
 */
int dashcam_core_start(lv_obj_t *preview_canvas);

/** Stop the pipeline. */
void dashcam_core_stop(void);

int dashcam_core_is_running(void);

/** @return 1 while async pipeline bring-up thread is still running. */
int dashcam_core_is_starting(void);

/**
 * @brief Enable/disable loop recording while the app stays open.
 * @details Preview and RTSP remain running; only the main-stream recorder
 *          is paused/resumed (physical ENTER / function key).
 */
void dashcam_core_set_active(int active);

/** @return 1 when recording is enabled (not manually paused). */
int dashcam_core_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_DASHCAM_CORE_H */
