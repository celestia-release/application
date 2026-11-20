// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Dashcam still capture from main-channel picture stream.
 */
#define LOG_TAG "WEARABLE_DASHCAM_CAPTURE"

#include "dashcam_capture.h"
#include "dashcam_core.h"
#include "dashcam_storage.h"

#include "app_main.h"
#include "multimedia.h"
#include "system_memory.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DASHCAM_PIPELINE_READY_WAIT_MS  2000

static pthread_t s_worker;
static volatile int s_busy;
static int s_worker_joinable;
static unsigned s_snap_seq;

static int dashcam_capture_picture_stream(multimedia_t *mm)
{
    int idx = -1;

    if (!mm || !mm->camera || !mm->camera->stream_index_by_role)
        return -1;

    /* Picture = MJPEG on main VI (group 0); preferred for full-res stills. */
    if (mm->camera->stream_index_by_role("picture", &idx) == 0 && idx >= 0)
        return idx;
    if (mm->camera->stream_index_by_role("main", &idx) == 0 && idx >= 0)
        return idx;
    if (mm->camera->stream_index_by_role("dashcam", &idx) == 0 && idx >= 0)
        return idx;
    return -1;
}

static void dashcam_stamp_photo_path(char *buf, size_t len, const char *dir)
{
    time_t t = time(NULL);
    struct tm tmv;
    struct tm *tm = localtime_r(&t, &tmv);
    unsigned seq = ++s_snap_seq;

    if (!buf || len == 0)
        return;
    if (!tm) {
        snprintf(buf, len, "%s/SNAP_%u.jpg", dir ? dir : "", seq);
        return;
    }
    /* Include seq so same-second snaps (or wall-clock jump) never overwrite. */
    snprintf(buf, len, "%s/SNAP_%04d%02d%02d_%02d%02d%02d_%u.jpg",
             dir ? dir : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, seq);
}

static void dashcam_capture_wait_pipeline_ready(void)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
    int ret = MULTIMEDIA_ERROR_INVALID_STATE;

    if (mm && mm->camera && mm->camera->wait_video_pipeline_ready)
        ret = mm->camera->wait_video_pipeline_ready(DASHCAM_PIPELINE_READY_WAIT_MS);

    if (ret != MULTIMEDIA_OK)
        log_warn("snap: pipeline settle timeout (%dms)", DASHCAM_PIPELINE_READY_WAIT_MS);
}

static int dashcam_capture_sync(void)
{
    multimedia_t *mm;
    char dir[360];
    char path[420];
    media_capture_picture_param_t param;
    void *jpeg = NULL;
    size_t jpeg_len = 0;
    int stream_idx;
    int ret;

    if (!dashcam_storage_is_ready()) {
        log_warn("snap: TF card not ready");
        return -1;
    }
    if (dashcam_storage_get_subdir(DASHCAM_DIR_PHOTO, dir, sizeof(dir)) != 0) {
        log_warn("snap: Photo dir unavailable");
        return -1;
    }

    mm = (multimedia_t *)app_main_get_multimedia();
    if (!mm || !mm->camera || !mm->camera->capture_picture) {
        log_warn("snap: camera capture API unavailable");
        return -1;
    }

    stream_idx = dashcam_capture_picture_stream(mm);
    if (stream_idx < 0) {
        log_warn("snap: no picture/main/dashcam stream");
        return -1;
    }

    dashcam_stamp_photo_path(path, sizeof(path), dir);
    memset(&param, 0, sizeof(param));
    param.save_path = path;
    param.format = 1; /* JPEG */

    log_info("snap: capture stream=%d -> %s", stream_idx, path);
    ret = mm->camera->capture_picture(stream_idx, &param, &jpeg, &jpeg_len);
    if (ret != 0) {
        log_warn("snap: capture_picture failed ret=%d", ret);
        return -1;
    }

    /* With save_path set, multimedia frees the buffer and returns *data=NULL.
     * Free defensively if a future API change returns memory. */
    if (jpeg != NULL) {
        system_free(jpeg);
        jpeg = NULL;
        jpeg_len = 0;
    }

    log_info("snap: saved %s", path);
    return 0;
}

static void dashcam_capture_finish_worker(void)
{
    /* capture_picture returns after restore request; wait for HAL/VENC settle. */
    dashcam_capture_wait_pipeline_ready();
    s_busy = 0;
}

static int dashcam_capture_wait_pre_snap_ready(void)
{
    int guard;

    for (guard = 0; guard < 100; guard++) {
        multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
        int ready = 1;

        if (!dashcam_core_is_running())
            return -1;
        if (mm && mm->camera && mm->camera->is_video_pipeline_ready)
            ready = mm->camera->is_video_pipeline_ready();
        if (ready)
            return 0;
        usleep(20 * 1000);
    }
    return -1;
}

static void *dashcam_capture_worker(void *arg)
{
    (void)arg;

    if (dashcam_capture_wait_pre_snap_ready() != 0) {
        log_warn("snap: video pipeline not settled, abort");
        s_busy = 0;
        return NULL;
    }

    (void)dashcam_capture_sync();
    dashcam_capture_finish_worker();
    return NULL;
}

int dashcam_capture_ready(void)
{
    multimedia_t *mm;

    if (s_busy)
        return 0;
    if (!dashcam_core_is_running())
        return 0;

    mm = (multimedia_t *)app_main_get_multimedia();
    if (!mm || !mm->camera)
        return 0;

    /* Older libmultimedia without settle API: treat as ready. */
    if (!mm->camera->is_video_pipeline_ready)
        return 1;

    return mm->camera->is_video_pipeline_ready();
}

int dashcam_capture_trigger(void)
{
    int guard;

    /* Pipeline starts on a worker; wait briefly so early snap keys still work. */
    for (guard = 0; !dashcam_core_is_running() && dashcam_core_is_starting() &&
         guard < 150; guard++)
        usleep(20 * 1000);

    if (!dashcam_core_is_running()) {
        log_warn("snap: pipeline not ready");
        return -1;
    }
    if (s_busy)
        return -1;

    s_busy = 1;
    if (pthread_create(&s_worker, NULL, dashcam_capture_worker, NULL) != 0) {
        int ret;

        log_warn("snap: worker create failed, running inline");
        if (dashcam_capture_wait_pre_snap_ready() != 0) {
            log_warn("snap: video pipeline not settled");
            s_busy = 0;
            return -1;
        }
        ret = dashcam_capture_sync();
        dashcam_capture_finish_worker();
        return (ret == 0) ? 0 : -1;
    }
    /* Joinable so destroy can wait instead of racing a detached snap worker. */
    s_worker_joinable = 1;
    return 0;
}

int dashcam_capture_busy(void)
{
    return s_busy;
}

/** Block until in-flight snap finishes (call from UI destroy / core stop). */
void dashcam_capture_wait_idle(void)
{
    if (s_worker_joinable) {
        pthread_join(s_worker, NULL);
        s_worker_joinable = 0;
        return;
    }

    /* Inline snap path (pthread_create failed): poll until busy clears. */
    {
        int guard;

        for (guard = 0; guard < 250 && s_busy; guard++)
            usleep(20 * 1000);
        if (s_busy)
            log_warn("snap: still busy after destroy wait");
    }
}
