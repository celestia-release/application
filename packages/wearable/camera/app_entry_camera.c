// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/wearable.
 *
 * Camera app dynamic entry point loaded by launcher via dlopen.
 */
#define LOG_TAG "LUNA CAMERA ENTRY"

#include "app_main.h"
#include "wearable_camera_streams.h"
#include "wearable_camera_ui.h"
#include "gui_common.h"
#include "log.h"
#include "lvgl.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_camera(lv_obj_t *main)
{
    const gui_camera_streams_t *streams;

    if (!main)
        return -1;

    log_info("camera app entry (preview pull+rgb565)");

    if (!app_main_get_multimedia()) {
        log_info("opening multimedia for camera preview");
        (void)app_main_open_framework_handles();
        if (!app_main_get_multimedia()) {
            log_error("multimedia open failed for camera");
            return -1;
        }
    }

    if (wearable_camera_streams_refresh() != 0) {
        log_error("camera streams unavailable (check fw_video_config / sensor)");
        return -1;
    }

    streams = wearable_camera_streams_get();
    if (!streams || (streams->idx_preview < 0 && streams->idx_main < 0)) {
        log_error("camera: no preview/main stream index");
        return -1;
    }

    log_info("camera app entry: streams ok preview=%d main=%d, create UI",
             streams->idx_preview, streams->idx_main);

    if (wearable_camera_ui_create(main) != 0) {
        log_error("camera UI create failed");
        return -1;
    }

    log_info("camera app entry: UI created, preview parent=%p", (void *)main);
    return 0;
}

static int (*const s_keep_app_entry_camera)(lv_obj_t *) = app_entry_start_camera;
