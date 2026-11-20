// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/wearable.
 *
 * Dashcam app dynamic entry point loaded by launcher via dlopen.
 */
#define LOG_TAG "LUNA DASHCAM ENTRY"

#include "app_main.h"
#include "dashcam_ui.h"
#include "log.h"
#include "lvgl.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

APP_ENTRY_EXPORT int app_entry_start_dashcam(lv_obj_t *main)
{
    if (!main)
        return -1;

    log_info("dashcam app entry (background record + rtsp)");

    if (!app_main_get_multimedia()) {
        log_info("opening multimedia for dashcam");
        (void)app_main_open_framework_handles();
        if (!app_main_get_multimedia()) {
            log_error("multimedia open failed for dashcam");
            return -1;
        }
    }

    if (dashcam_ui_create(main) != 0) {
        log_error("dashcam UI create failed");
        return -1;
    }

    log_info("dashcam app entry: UI created, parent=%p", (void *)main);
    return 0;
}

static int (*const s_keep_app_entry_dashcam)(lv_obj_t *) = app_entry_start_dashcam;
