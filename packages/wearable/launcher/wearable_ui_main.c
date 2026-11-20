// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna main UI: watch-style home launcher with clock face and side apps.
 */
#define LOG_TAG "LUNA MAIN"

#include <stdlib.h>
#include <string.h>

#include "gui_shell.h"
#include "wearable_app_registry.h"
#include "wearable_home.h"
#include "wearable_input.h"
#include "log.h"
#include "lvgl.h"

#if defined(CONFIG_APP_WEARABLE_CAMERA) && CONFIG_APP_WEARABLE_CAMERA
#include "wearable_camera_tfcard.h"
#endif

static void wearable_boot_app_timer_cb(lv_timer_t *timer)
{
    const char *boot_app = getenv("WEARABLE_BOOT_APP");

    if (boot_app && boot_app[0] != '\0') {
        log_info("WEARABLE_BOOT_APP=%s: auto-launch", boot_app);
        if (wearable_home_launch_app_by_id(boot_app) != 0) {
            log_error("auto-launch failed for %s", boot_app);
        }
    }
    lv_timer_delete(timer);
}

#if defined(CONFIG_APP_WEARABLE_DASHCAM_AUTOSTART) && CONFIG_APP_WEARABLE_DASHCAM_AUTOSTART
static void wearable_dashcam_autostart_timer_cb(lv_timer_t *timer)
{
    log_info("dashcam autostart: launching");
    if (wearable_home_launch_app_by_id("dashcam") != 0)
        log_error("dashcam autostart failed");
    lv_timer_delete(timer);
}
#endif

int wearable_ui_main(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(lv_display_get_default());
    gui_shell_app_desc_t apps[WEARABLE_APP_MAX_DYNAMIC];
    unsigned int app_count;

    if (!screen) {
        log_error("no active screen");
        return -1;
    }

#if defined(CONFIG_APP_WEARABLE_CAMERA) && CONFIG_APP_WEARABLE_CAMERA
    (void)wearable_tfcard_ensure_init();
#endif

    wearable_app_registry_refresh();
    app_count = wearable_app_registry_fill_gui_shell(apps, WEARABLE_APP_MAX_DYNAMIC);

    if (wearable_home_create(screen, apps, app_count) != 0) {
        log_error("luna home launcher create failed");
        return -1;
    }

    wearable_input_init(wearable_home_get_root());

    if (getenv("WEARABLE_BOOT_APP") != NULL && getenv("WEARABLE_BOOT_APP")[0] != '\0') {
        lv_timer_create(wearable_boot_app_timer_cb, 500, NULL);
    }

#if defined(CONFIG_APP_WEARABLE_DASHCAM_AUTOSTART) && CONFIG_APP_WEARABLE_DASHCAM_AUTOSTART
    /* Boot straight into dashcam (still exit-able via swipe-up). Env override wins. */
    if (getenv("WEARABLE_BOOT_APP") == NULL || getenv("WEARABLE_BOOT_APP")[0] == '\0') {
        lv_timer_create(wearable_dashcam_autostart_timer_cb, 800, NULL);
    }
#endif
    return 0;
}
