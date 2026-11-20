// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Nova entry: tablet_run() called by app_main after framework modules and app_lv_open.
 */
#define LOG_TAG "TABLET_ENTRY"

#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

#include "config.h"        /* CONFIG_APP_UVC (project) */
#include "system_config.h" /* config_build_file_path */
#include "app_main.h"
#include "log.h"
#include "multimedia.h"
#include "network.h"
#include "tablet_font.h"
#include "tablet_home.h"
#include "system_time.h"
#include "lvgl.h"
#if defined(CONFIG_APP_UVC)
#include "uvc_server.h"
#endif

#define GUI_READY_PATH              "/tmp/ipc_gui_ready"
#define POST_HOME_AUDIO_DELAY_US    (300 * 1000)
#define GUI_READY_POLL_US           (50 * 1000)
#define GUI_READY_TIMEOUT_LOOPS     200  /* ~10s */
#define WIFI_BOOT_RETRY_COUNT       20

static void tablet_wifi_auto_connect(void)
{
    network_t *network = (network_t *)app_main_get_network();
    int i;

    if (!network || !network->wifi || !network->wifi->station)
        return;

    if (network->wifi->enable)
        (void)network->wifi->enable();

    /* The board loads its WiFi driver after Home's first frame. Wait until the
     * station backend is ready, then associate from settings.json. */
    for (i = 0; i < WIFI_BOOT_RETRY_COUNT; i++) {
        if (!network->wifi->station->init ||
            network->wifi->station->init() == NETWORK_OK) {
            if (network->wifi->station->connect_from_config) {
                int ret = network->wifi->station->connect_from_config();

                if (ret == NETWORK_OK) {
                    log_info("tablet boot WiFi association started");
                    return;
                }
                if (ret == NETWORK_ERROR_NOT_SUPPORTED)
                    return; /* settings.json explicitly disables auto-connect */
            }
        }
        sleep(1);
    }
    log_warn("tablet boot WiFi: station unavailable after %ds",
             WIFI_BOOT_RETRY_COUNT);
}

static void tablet_wait_gui_ready(void)
{
    int i;

    for (i = 0; i < GUI_READY_TIMEOUT_LOOPS; i++) {
        if (access(GUI_READY_PATH, F_OK) == 0)
            return;
        usleep(GUI_READY_POLL_US);
    }
    log_warn("tablet_run: %s not seen within timeout", GUI_READY_PATH);
}

static void *tablet_post_home_init_thread(void *arg)
{
    multimedia_t *multimedia;
    char audio_cfg[256];

    (void)arg;

    tablet_wait_gui_ready();
    /* Let GuiThread finish first refr + font critical path before IPU-heavy opens. */
    usleep(POST_HOME_AUDIO_DELAY_US);

    /*
     * Sequential after Home: FT/png (deferred GUI), then multimedia/network, then fonts/audio.
     * Home does not need FreeType/libpng (bitmap fonts + QOI).
     */
    (void)app_main_load_deferred_gui_libs();
    (void)app_main_open_multimedia_handle();
    (void)app_main_open_network_handle();
    multimedia = (multimedia_t *)app_main_get_multimedia();
    if (multimedia && multimedia->camera && multimedia->camera->prepare)
        (void)multimedia->camera->prepare();
    tablet_wifi_auto_connect();
    (void)tablet_font_ensure_full();

    multimedia = (multimedia_t *)app_main_get_multimedia();
    if (!multimedia || !multimedia->audio || !multimedia->audio->load_config)
        return NULL;
    if (config_build_file_path(MULTIMEDIA_CFG_AUDIO_FILE, audio_cfg, sizeof(audio_cfg)) != 0)
        return NULL;

    log_info("tablet_run: post-home audio config preload");
    multimedia->audio->load_config(audio_cfg);
    return NULL;
}

static void tablet_schedule_post_home_init(void)
{
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, tablet_post_home_init_thread, NULL) != 0)
        log_warn("tablet_run: post-home init thread create failed");
    pthread_attr_destroy(&attr);
}

/* Entry called by app_main after open handles + app_lv_open + deferred load scheduled. */
void tablet_run(void)
{
    long long llstart = system_get_sys_run_time64();
    log_info("tablet_run start");

    /* Home first; multimedia/network/audio after /tmp/ipc_gui_ready. */
    tablet_schedule_post_home_init();

#if defined(CONFIG_APP_UVC)
    app_uvc_init();
#endif

    /* WiFi driver + wpa + DHCP: app_01_start.sh after GUI first frame.
     * HAL station->init() is on demand (settings WiFi page / wifi_station_ensure_init). */

    log_info("tablet_run done, elapsed=%lld ms", system_get_sys_run_time64() - llstart);

    while (!app_main_shutdown_requested())
        usleep(100 * 1000);

    log_info("tablet_run: shutdown requested, graceful teardown");
    app_main_graceful_teardown();
    sync();
    log_info("tablet_run: exit");
    exit(0);
}
