// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Nova standalone/embedded entry: excluded from product build via TABLET_EXCLUDE_MAIN=1.
 * Product entry is application/app_main/main.c; main() here is for standalone nova build/debug.
 */
#define LOG_TAG "TABLET_MAIN"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "system_config.h"
#include "log.h"
#include "hardware.h"
#include "app_main.h"
#include "tablet_gui.h"
#include "runtime_bootstrap.h"
#include "rtsp_server.h"
#include "system_time.h"

int nb_link_init();
extern int resize_main();

/* uvc_main: platform/input init already done by app_main_platform_startup() in app_main */
int uvc_main(void)
{
    long long llstart = system_get_sys_run_time64();
    log_debug("%s_%d start time=%lld", __FUNCTION__, __LINE__, llstart);

    runtime_bootstrap_t *sys = (runtime_bootstrap_t *)app_main_get_runtime();
    if (sys)
        sys->load_config(NULL);

    char gui_cfg[256];
    if (config_build_file_path("app_gui_config.ini", gui_cfg, sizeof(gui_cfg)) == 0)
        app_lv_open(gui_cfg);

    char app_script[256];
    if (config_build_file_path("app_02_load.sh", app_script, sizeof(app_script)) == 0) {
        system(app_script);
    }

    tablet_run();
    return 0;
}

#ifndef TABLET_EXCLUDE_MAIN
/* Standalone nova entry: platform startup + runtime + UI + script, then tablet_run(). */
int main(void)
{
    long long llstart = system_get_sys_run_time64();
    log_debug("%s_%d start time=%lld", __FUNCTION__, __LINE__, llstart);

    if (app_main_platform_startup() != 0)
        return -1;

    runtime_bootstrap_t *sys = (runtime_bootstrap_t *)app_main_get_runtime();
    if (sys)
        sys->load_config(NULL);

    char gui_cfg[256];
    if (config_build_file_path("app_gui_config.ini", gui_cfg, sizeof(gui_cfg)) == 0)
        app_lv_open(gui_cfg);

    char app_script[256];
    if (config_build_file_path("app_02_load.sh", app_script, sizeof(app_script)) == 0)
        system(app_script);

    tablet_run();
    return 0;
}
#endif
