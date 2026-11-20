// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Photo capture and video recording for Luna camera.
 */
#ifndef WEARABLE_CAMERA_CAPTURE_H
#define WEARABLE_CAMERA_CAPTURE_H

#include <stddef.h>

int wearable_camera_take_photo(char *out_path, size_t out_len);
int wearable_camera_record_start(void);
int wearable_camera_record_stop(void);
int wearable_camera_record_is_active(void);

/**
 * Worker-thread safe: main-channel MJPEG still capture to TF card.
 * Preview sub-channel (LCD) is not stopped or unregistered.
 */
int wearable_camera_capture_to_tfcard(char *out_path, size_t out_len);

/** No-op: preview stays live; kept for API compatibility. */
void wearable_camera_finish_after_capture(void);

#endif /* WEARABLE_CAMERA_CAPTURE_H */
