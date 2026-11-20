// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Stream role/channel cache for Luna camera (application -> framework multimedia only).
 */
#define LOG_TAG "WEARABLE_CAMERA_STREAMS"

#include "wearable_camera_streams.h"

#include "app_main.h"
#include "config.h"
#include "gui_common.h"
#include "multimedia.h"
#include "log.h"

#include <string.h>

static gui_camera_streams_t s_streams;

int wearable_camera_streams_refresh(void)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();

    if (mm == NULL) {
        memset(&s_streams, 0, sizeof(s_streams));
        return -1;
    }

    if (gui_camera_streams_resolve(mm, &s_streams) != 0) {
        memset(&s_streams, 0, sizeof(s_streams));
        return -1;
    }

    log_info("streams main idx=%d %dx%d preview idx=%d %dx%d (lcd %dx%d)",
             s_streams.idx_main, s_streams.main_w, s_streams.main_h,
             s_streams.idx_preview, s_streams.preview_w, s_streams.preview_h,
             CONFIG_LCD_HOR_RES, CONFIG_LCD_VER_RES);

    if (s_streams.preview_w > 0 && s_streams.preview_h > 0) {
        log_info("preview stream %dx%d (lcd %dx%d)",
                 s_streams.preview_w, s_streams.preview_h,
                 CONFIG_LCD_HOR_RES, CONFIG_LCD_VER_RES);
    }
    return 0;
}

const gui_camera_streams_t *wearable_camera_streams_get(void)
{
    return s_streams.valid ? &s_streams : NULL;
}
