// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * NV12 capture/save/playback helpers for Luna camera (preview-compatible format).
 */
#ifndef WEARABLE_CAMERA_NV12_H
#define WEARABLE_CAMERA_NV12_H

#include <stddef.h>
#include <stdint.h>

#include "multimedia.h"

#define WEARABLE_NV12_MAGIC "LNV1"

typedef struct {
    char magic[4];
    uint32_t width;
    uint32_t height;
} wearable_nv12_file_hdr_t;

size_t wearable_nv12_frame_size(uint32_t width, uint32_t height);

int wearable_nv12_save_frame(const char *path, uint32_t width, uint32_t height,
                         const void *data, size_t data_len, int append);

int wearable_nv12_load_single(const char *path, uint32_t *width, uint32_t *height,
                          void *buf, size_t buf_size);

int wearable_nv12_capture_preview_frame(multimedia_t *mm, int stream_idx,
                                    uint32_t width, uint32_t height,
                                    const char *path);

int wearable_nv12_capture_preview_buffer(multimedia_t *mm, int stream_idx,
                                     uint32_t *width, uint32_t *height,
                                     void *buf, size_t buf_size);

int wearable_nv12_record_start(multimedia_t *mm, int stream_idx,
                           uint32_t width, uint32_t height,
                           const char *path);

void wearable_nv12_record_stop(void);

int wearable_nv12_record_is_active(void);

#endif /* WEARABLE_CAMERA_NV12_H */
