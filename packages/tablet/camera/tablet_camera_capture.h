// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Photo (JPEG) and video (MP4) capture for Nova tablet camera.
 */
#ifndef APPLICATION_TABLET_CAMERA_CAPTURE_H
#define APPLICATION_TABLET_CAMERA_CAPTURE_H

#include <stddef.h>

int tablet_camera_take_photo(char *out_path, size_t out_len);

/**
 * Load the sub JPEG appended to a photo. Caller frees *data with free().
 * Returns 0 when the file has a sub JPEG.
 */
int tablet_camera_photo_load_sub(const char *path, void **data, size_t *size,
                                 unsigned *width, unsigned *height);
int tablet_camera_record_start(void);
int tablet_camera_record_stop(void);
int tablet_camera_record_is_active(void);

#endif /* APPLICATION_TABLET_CAMERA_CAPTURE_H */
