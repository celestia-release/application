// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * Handle registry and framework module startup: platform startup, open/register multimedia, network,
 * and optional UI (expect /dev/fb0 from app.sh app_02_load.sh, then app_lv_open).
 */
#define LOG_TAG "APP MAIN"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include "app_main.h"
#include "app_main_error.h"
#include "config.h"  /* CONFIG_APP_PACKAGES_WEARABLE/TABLET (and legacy GUI_LUNA/NOVA) */
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
#include <dlfcn.h>
#endif
#include "app_config.h"
#include "multimedia.h"
#include "network.h"
#if defined(CONFIG_APP_PACKAGES_WEARABLE) || defined(CONFIG_APP_GUI_LUNA) || \
    defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
#include "app_gui.h"
#endif

/* Teardown only: release /dev/ipu even if a module leaked open refs. */
extern int hal_ipu_force_close(void);
#include "log.h"
#include "runtime_bootstrap.h"
#include "soc_platform.h"
#include "notification.h"
#if defined(CONFIG_FW_RUNTIME_POWER_BUTTON) && CONFIG_FW_RUNTIME_POWER_BUTTON
#include "power_button.h"
#endif
#if defined(CONFIG_FW_RUNTIME_WATCHDOG) && CONFIG_FW_RUNTIME_WATCHDOG
#include "watchdog_service.h"
#endif

/* -------- Unified platform startup -------- */
#define APP_MAIN_LOG_LEVEL_NORMAL 4
static notification_t *s_app_main_notification;

int app_main_platform_startup(void)
{
    if (framework_init(0, LOG_INFO) != 0)
        return APP_MAIN_ERROR_INVALID_CONFIG;
    /* Persist framework logs to /var/log/DevLog_a.log (rotating to
     * DevLog_b.log) so log_info/log_warn lines are greppable from a remote
     * shell, not only on the serial console. Without this the save callback
     * is never installed and our diagnostics (e.g. the T-card format-thread
     * path) are invisible over telnet/ssh. */
    log_write_init();
    soc_platform_set_log_level(APP_MAIN_LOG_LEVEL_NORMAL);
    if (notification_open(&s_app_main_notification) == NOTIFICATION_OK
            && s_app_main_notification && s_app_main_notification->input) {
        s_app_main_notification->input->init();
        if (s_app_main_notification->message) {
            s_app_main_notification->message->init();
        }
        if (s_app_main_notification->alarm) {
            s_app_main_notification->alarm->init();
        }
        app_main_register_notification(s_app_main_notification);
    }
#if defined(CONFIG_FW_RUNTIME_POWER_BUTTON) && CONFIG_FW_RUNTIME_POWER_BUTTON
    power_button_init();
#endif
#if defined(CONFIG_FW_RUNTIME_WATCHDOG) && CONFIG_FW_RUNTIME_WATCHDOG
    /* Start the hardware watchdog feed thread (default 3s timeout). */
    if (watchdog_service_init(WATCHDOG_SERVICE_DEFAULT_TIMEOUT_SEC) != RUNTIME_OK)
        log_error("watchdog_service_init failed");
#endif
    return APP_MAIN_OK;
}

/* -------- Handle registry -------- */
static void *s_notification;
static void *s_network;
static void *s_multimedia;
static void *s_nebula;
static void *s_runtime;
static volatile sig_atomic_t s_app_main_shutdown_requested = 0;

void app_main_register_notification(void *n) { s_notification = n; }
void app_main_register_network(void *n)        { s_network = n; }
void app_main_register_multimedia(void *m)     { s_multimedia = m; }
void app_main_register_nebula(void *n)        { s_nebula = n; }
void app_main_register_runtime(void *r)        { s_runtime = r; }

void *app_main_get_notification(void) { return s_notification; }
void *app_main_get_network(void)     { return s_network; }
void *app_main_get_multimedia(void)  { return s_multimedia; }
void *app_main_get_nebula(void)      { return s_nebula; }
void *app_main_get_runtime(void)     { return s_runtime; }

void app_main_request_shutdown(void) { s_app_main_shutdown_requested = 1; }
int app_main_shutdown_requested(void) { return s_app_main_shutdown_requested ? 1 : 0; }

#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
/* Scheme B: do not DT_NEEDED libmultimedia/libnetwork — dlopen after Home. */
typedef int (*app_mm_open_fn)(multimedia_t **);
typedef int (*app_mm_close_fn)(multimedia_t *);
typedef int (*app_net_open_fn)(network_t **);

static void *s_mm_dl;
static void *s_net_dl;
static app_mm_open_fn s_mm_open_fn;
static app_mm_close_fn s_mm_close_fn;
static app_net_open_fn s_net_open_fn;

static int app_main_dlopen_multimedia(void)
{
    if (s_mm_open_fn)
        return 0;
    s_mm_dl = dlopen("libmultimedia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s_mm_dl) {
        log_error("dlopen libmultimedia.so failed: %s", dlerror());
        return -1;
    }
    s_mm_open_fn = (app_mm_open_fn)dlsym(s_mm_dl, "multimedia_open");
    s_mm_close_fn = (app_mm_close_fn)dlsym(s_mm_dl, "multimedia_close");
    if (!s_mm_open_fn) {
        log_error("dlsym multimedia_open failed: %s", dlerror());
        return -1;
    }
    return 0;
}

static int app_main_dlopen_network(void)
{
    if (s_net_open_fn)
        return 0;
    /* libnetwork resolves media_video_payload_is_keyframe from libmultimedia. */
    if (app_main_dlopen_multimedia() != 0)
        return -1;
    s_net_dl = dlopen("libnetwork.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s_net_dl) {
        log_error("dlopen libnetwork.so failed: %s", dlerror());
        return -1;
    }
    s_net_open_fn = (app_net_open_fn)dlsym(s_net_dl, "network_open");
    if (!s_net_open_fn) {
        log_error("dlsym network_open failed: %s", dlerror());
        return -1;
    }
    return 0;
}
#endif

void app_main_graceful_teardown(void)
{
    multimedia_t *multimedia;

    multimedia = (multimedia_t *)app_main_get_multimedia();
    if (multimedia) {
        log_info("graceful teardown: multimedia_close");
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
        if (s_mm_close_fn)
            s_mm_close_fn(multimedia);
        else
            log_warn("multimedia_close unavailable (so not loaded)");
#else
        multimedia_close(multimedia);
#endif
        app_main_register_multimedia(NULL);
    }

#if defined(CONFIG_APP_PACKAGES_WEARABLE) || defined(CONFIG_APP_GUI_LUNA) || \
    defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
    if (app_main_get_launch_mode_ui()) {
        log_info("graceful teardown: app_lv_close");
        app_lv_close();
    }
#endif
    /* Ensure /dev/ipu released even if OSD/camera leaked open refs (local_restart). */
    (void)hal_ipu_force_close();
}

/* -------- Open all framework handles (no UI) -------- */
int app_main_open_framework_handles(void) {
    /*
     * multimedia_open (~71ms) shares IPU with LVGL — never run it concurrently with
     * app_lv_open / display init (causes fb_get_info / IPU stalls).
     *
     * Tablet/Nova: defer until after Home first frame (tablet_entry post-home thread)
     * so the ~71ms stays off the Home critical path, still strictly sequential.
     * Other products: open multimedia (+ network) here before UI as before.
     */
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
    return APP_MAIN_OK;
#else
    if (!app_main_get_multimedia()) {
        log_info("opening multimedia service");
        multimedia_t *multimedia = NULL;
        if (multimedia_open(&multimedia) == 0 && multimedia)
            app_main_register_multimedia(multimedia);
        else
            log_warn("multimedia_open failed");
    }
    (void)app_main_open_network_handle();
    return APP_MAIN_OK;
#endif
}

int app_main_open_multimedia_handle(void)
{
    multimedia_t *multimedia = NULL;

    if (app_main_get_multimedia())
        return APP_MAIN_OK;

    log_info("opening multimedia service");
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
    if (app_main_dlopen_multimedia() != 0)
        return APP_MAIN_ERROR_FILE_OPEN_FAILED;
    if (s_mm_open_fn(&multimedia) == 0 && multimedia)
        app_main_register_multimedia(multimedia);
    else
        log_warn("multimedia_open failed");
#else
    if (multimedia_open(&multimedia) == 0 && multimedia)
        app_main_register_multimedia(multimedia);
    else
        log_warn("multimedia_open failed");
#endif
    return APP_MAIN_OK;
}

int app_main_open_network_handle(void)
{
    network_t *network = NULL;

    if (app_main_get_network())
        return APP_MAIN_OK;

    log_info("opening network service");
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
    if (app_main_dlopen_network() != 0)
        return APP_MAIN_ERROR_FILE_OPEN_FAILED;
    if (s_net_open_fn(&network) == NETWORK_OK && network)
        app_main_register_network(network);
#else
    if (network_open(&network) == NETWORK_OK && network)
        app_main_register_network(network);
#endif
    log_info("network service ready");
    return APP_MAIN_OK;
}

/* -------- Framework module startup (open handles + optional UI) -------- */
int app_main_start_framework_modules(int headless, const char *gui_config_path)
{
    if (app_main_open_framework_handles() != APP_MAIN_OK)
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if (!headless) {
        /*
         * Display is loaded by app.sh (app_02_load.sh) before luna_main starts.
         * Never system() that script here: notification worker threads are already
         * running, and system()/fork() in a multithreaded process can deadlock on
         * uClibc — parent blocks in wait with no further logs after multimedia_open.
         *
         * Wearable: defer app_lv_open to app_gui_run() so "luna launcher started"
         * is printed even if LVGL/GPU init blocks.
         * Tablet: open LVGL here before tablet_run().
         */
        if (access("/dev/fb0", F_OK) != 0)
            log_warn("/dev/fb0 missing; run /etc/config/app_02_load.sh before UI");
        else
            log_info("display ready (/dev/fb0)");
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
        const char *cfg = gui_config_path ? gui_config_path : "/etc/config/app_gui_config.ini";
        log_info("opening LVGL UI (%s)", cfg);
        if (app_lv_open(cfg) != 0) {
            log_error("app_lv_open failed");
            return APP_MAIN_ERROR_FILE_OPEN_FAILED;
        }
#else
        (void)gui_config_path;
#endif
    } else if (network_init() != NETWORK_OK) {
        log_warn("network_init failed (check settings.json wifi)");
    }
    return APP_MAIN_OK;
}
