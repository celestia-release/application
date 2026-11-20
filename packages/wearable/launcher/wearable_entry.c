// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/wearable.
 *
 * Luna GUI run loop (app_gui_run implementation for CONFIG_APP_PACKAGES_WEARABLE).
 */
#define LOG_TAG "LUNA ENTRY"

#include <unistd.h>

#include "app_config.h"
#include "app_gui.h"
#include "app_main.h"
#include "log.h"

void app_gui_run(void)
{
    const char *cfg;

    /* Log first so a later block in app_lv_open is visible in serial logs. */
    log_info("luna launcher starting");

    if (!app_main_get_multimedia())
        app_main_open_framework_handles();

    cfg = app_main_get_gui_config_path();
    if (app_lv_open(cfg ? cfg : "/etc/config/settings.json") != 0) {
        log_error("app_lv_open failed");
        return;
    }

    log_info("luna launcher started");
    while (!app_main_shutdown_requested())
        usleep(1000 * 1000);
}
