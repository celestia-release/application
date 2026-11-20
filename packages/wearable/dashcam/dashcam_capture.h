// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Dashcam still capture: main-channel MJPEG (role=picture) → Dashcam/Photo/*.jpg
 * Runs off the LVGL thread (capture blocks).
 */
#ifndef WEARABLE_DASHCAM_CAPTURE_H
#define WEARABLE_DASHCAM_CAPTURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Trigger a snap on a worker thread (non-blocking for LVGL).
 * @return 0 if queued/started, -1 if busy or app not ready.
 */
int dashcam_capture_trigger(void);

/** @return 1 while a snap worker is running (includes post-restore settle). */
int dashcam_capture_busy(void);

/** @return 1 when not busy and camera VIDEO_RECORD pipeline has settled. */
int dashcam_capture_ready(void);

/** Wait until snap worker finishes (exit path). */
void dashcam_capture_wait_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_DASHCAM_CAPTURE_H */
