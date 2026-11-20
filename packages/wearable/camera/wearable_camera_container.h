// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna composite media container (LCM2): preview NV12 + high-res payload in one file.
 * Device reads preview section only; PC reads high section via header offset.
 */
#ifndef WEARABLE_CAMERA_CONTAINER_H
#define WEARABLE_CAMERA_CONTAINER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WEARABLE_LCM_MAGIC      "LCM2"
#define WEARABLE_LCM_VERSION    2

#define WEARABLE_LCM_TYPE_PHOTO 0
#define WEARABLE_LCM_TYPE_VIDEO 1

#define WEARABLE_PHOTO_EXT      ".lcm"
#define WEARABLE_VIDEO_EXT      ".lcm"
#define WEARABLE_PHOTO_PREFIX   "IMG_"
#define WEARABLE_VIDEO_PREFIX   "VID_"

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t type;
    uint32_t preview_w;
    uint32_t preview_h;
    uint32_t preview_offset;
    uint32_t preview_size;
    uint32_t high_offset;
    uint32_t high_size;
    char high_format[8];
} wearable_lcm_hdr_t;

size_t wearable_lcm_preview_frame_bytes(uint32_t width, uint32_t height);

void wearable_lcm_photo_path(const char *dir, char *out_path, size_t out_len);

void wearable_lcm_video_path(const char *dir, char *out_path, size_t out_len);

int wearable_lcm_photo_write(const char *path, uint32_t preview_w, uint32_t preview_h,
                         const void *preview_nv12, size_t preview_len,
                         const void *jpeg_data, size_t jpeg_len);

int wearable_lcm_video_begin(const char *path, uint32_t preview_w, uint32_t preview_h);

int wearable_lcm_video_append_preview(const void *data, size_t len);

int wearable_lcm_video_finish(const char *high_mp4_path);

void wearable_lcm_video_abort(void);

const char *wearable_lcm_video_high_tmp_path(void);

int wearable_lcm_read_header(const char *path, wearable_lcm_hdr_t *hdr);

int wearable_lcm_read_photo_preview(const char *path, void *buf, size_t buf_size,
                                uint32_t *width, uint32_t *height);

FILE *wearable_lcm_open_video_preview(const char *path, wearable_lcm_hdr_t *hdr);

int wearable_lcm_is_media_name(const char *name);

#endif /* WEARABLE_CAMERA_CONTAINER_H */
