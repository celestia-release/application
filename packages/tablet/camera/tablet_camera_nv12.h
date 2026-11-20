// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * NV12 preview sidecar recorder / trailer format for Nova tablet camera.
 *
 * During recording a second pull-client captures the 800x480 NV12 preview
 * stream and writes raw frames to a temp file.  On stop the frames are
 * appended to the MP4 followed by a 32-byte NOVAPREV footer so the file
 * remains a valid MP4 (standard parsers stop at the mdat boundary and never
 * see the trailer) while the on-device gallery can play it back without
 * software-decoding the high-resolution H.264 stream.
 *
 * Footer layout (32 bytes, always at the very end of the file):
 *   magic[8]     "NOVAPREV"
 *   width        uint32_t  NV12 frame width  (e.g. 800)
 *   height       uint32_t  NV12 frame height (e.g. 480)
 *   fps          uint32_t  capture fps        (e.g. 15)
 *   n_frames     uint32_t  total NV12 frames
 *   data_size    uint64_t  n_frames * width * height * 3/2
 *
 * NV12 data starts at:  file_size - data_size - sizeof(nova_preview_footer_t)
 */
#ifndef APPLICATION_TABLET_CAMERA_NV12_H
#define APPLICATION_TABLET_CAMERA_NV12_H

#include <stddef.h>
#include <stdint.h>
#include "multimedia.h"

#define NOVA_PREVIEW_MAGIC  "NOVAPREV"

typedef struct {
    char     magic[8];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t n_frames;
    uint64_t data_size;
} nova_preview_footer_t;  /* sizeof == 32 */

static inline size_t nova_nv12_frame_size(uint32_t w, uint32_t h)
{
    return (size_t)w * (size_t)h * 3u / 2u;
}

int  tablet_nv12_record_start(multimedia_t *mm, int preview_stream_idx,
                              uint32_t w, uint32_t h, uint32_t fps,
                              const char *mp4_path);
void tablet_nv12_record_stop(const char *mp4_path);
int  tablet_nv12_record_is_active(void);

#endif /* APPLICATION_TABLET_CAMERA_NV12_H */
