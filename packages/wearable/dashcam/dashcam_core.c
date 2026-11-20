// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Lifecycle coordinator for the Luna dashcam app.
 *
 * Preview LVGL objects/timers must be created on the LVGL thread
 * (dashcam_ui + dashcam_preview_init/start). This module only brings up
 * storage / record / RTSP on a worker thread.
 */
#define LOG_TAG "WEARABLE_DASHCAM_CORE"

#include "dashcam_core.h"
#include "dashcam_capture.h"
#include "dashcam_preview.h"
#include "dashcam_record.h"
#include "dashcam_rtsp.h"
#include "dashcam_storage.h"

#include "app_main.h"
#include "multimedia.h"
#include "log.h"

#include <pthread.h>

/* Temporary OOM A/B test: 1 = RTSP only (no local record / main VENC client).
 * Set back to 0 after the memory experiment. */
#define DASHCAM_TEST_RTSP_ONLY  0

static int s_running;
static volatile int s_pipeline_active; /* demo active; 0 = manually stopped */
static pthread_t s_start_thread;
static volatile int s_start_thread_active;
static int s_start_thread_joinable;

static multimedia_t *dashcam_get_multimedia(void)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();

    if (mm == NULL) {
        log_info("dashcam: opening multimedia");
        (void)app_main_open_framework_handles();
        mm = (multimedia_t *)app_main_get_multimedia();
    }
    return mm;
}

static int dashcam_core_start_sync(void)
{
    multimedia_t *mm;

    if (s_running)
        return 0;

    mm = dashcam_get_multimedia();
    if (mm == NULL || mm->camera == NULL) {
        log_error("dashcam: multimedia/camera unavailable");
        return -1;
    }

#if !DASHCAM_TEST_RTSP_ONLY
    (void)dashcam_storage_init();
    /* One-shot background scan for crash leftovers; never block UI/open-record. */
    {
        char loop_dir[360];

        if (dashcam_storage_get_subdir(DASHCAM_DIR_LOOP_VIDEO, loop_dir,
                                       sizeof(loop_dir)) == 0 &&
            mm->recorder && mm->recorder->repair_kick)
            (void)mm->recorder->repair_kick(loop_dir);
    }
#endif

    /* Ensure G711A is advertised in RTSP SDP before the server accepts clients.
     * DESCRIBE reads stream_get_enc_param(0); without this, m=audio is omitted
     * and the player never SETUPs audio (live_start audio=0). */
    dashcam_audio_ensure_g711a();

#if DASHCAM_TEST_RTSP_ONLY
    if (dashcam_rtsp_start() != 0)
        log_warn("dashcam: rtsp start failed");
#else
    if (dashcam_record_start() != 0)
        log_warn("dashcam: record start failed");

    if (dashcam_rtsp_start() != 0)
        log_warn("dashcam: rtsp start failed");
#endif

    s_pipeline_active = 1;
    s_running = 1;
#if DASHCAM_TEST_RTSP_ONLY
    log_info("dashcam: pipeline started (rtsp-only test, record off)");
#else
    log_info("dashcam: pipeline started (record+rtsp); preview starts on LVGL thread");
#endif
    return 0;
}

static void *dashcam_core_start_thread(void *arg)
{
    (void)arg;
    (void)dashcam_core_start_sync();
    s_start_thread_active = 0;
    return NULL;
}

int dashcam_core_start(lv_obj_t *preview_canvas)
{
    (void)preview_canvas;

    if (s_running || s_start_thread_active || s_start_thread_joinable)
        return 0;

    /* Blocking bring-up (TF mount, RTSP listen) off the LVGL thread. Preview
     * LVGL objects are owned by dashcam_ui / dashcam_preview_* on LVGL thread. */
    s_start_thread_active = 1;
    s_start_thread_joinable = 1;
    if (pthread_create(&s_start_thread, NULL, dashcam_core_start_thread, NULL) != 0) {
        log_warn("dashcam: start thread create failed, running inline");
        s_start_thread_active = 0;
        s_start_thread_joinable = 0;
        return dashcam_core_start_sync();
    }
    return 0;
}

void dashcam_core_stop(void)
{
    if (s_start_thread_joinable) {
        pthread_join(s_start_thread, NULL);
        s_start_thread_joinable = 0;
        s_start_thread_active = 0;
    }

    /* Always tear down subsystems (idempotent) so a failed/partial start cannot
     * leave RTSP/record/preview clients alive across app re-entry. */
    dashcam_capture_wait_idle();

#if !DASHCAM_TEST_RTSP_ONLY
    dashcam_record_stop();
#endif
    dashcam_rtsp_stop();

    if (dashcam_preview_available())
        dashcam_preview_stop();
    dashcam_preview_deinit();

#if !DASHCAM_TEST_RTSP_ONLY
    dashcam_storage_deinit();
#endif

    s_pipeline_active = 0;
    s_running = 0;
    log_info("dashcam: pipeline stopped");
}

int dashcam_core_is_running(void)
{
    return s_running;
}

int dashcam_core_is_starting(void)
{
    return s_start_thread_active;
}

void dashcam_core_set_active(int active)
{
    if (!s_running)
        return;

    active = active ? 1 : 0;
    if (active == s_pipeline_active)
        return;

#if DASHCAM_TEST_RTSP_ONLY
    s_pipeline_active = active;
    log_info("dashcam: record toggle ignored (rtsp-only test)");
    return;
#else
    dashcam_record_set_paused(active ? 0 : 1);
    s_pipeline_active = active;
    log_info("dashcam: recording %s (preview+rtsp stay on)",
             active ? "on" : "off");
#endif
}

int dashcam_core_is_active(void)
{
    return s_running && s_pipeline_active;
}
