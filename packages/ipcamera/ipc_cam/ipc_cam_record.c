// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Loop recording worker for ipcamera.
 */
#define LOG_TAG "IPCAMERA_RECORD"

#include "ipc_cam_record.h"
#include "ipc_cam_storage.h"

#include "app_main.h"
#include "multimedia.h"
#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IPC_CAM_SEGMENT_MS      (3 * 60 * 1000)     /* 3-minute segments */
/* Headless boards often use small TF cards; keep a modest reserve. */
#define IPC_CAM_MIN_FREE_MB     48
#define IPC_CAM_POLL_US         (200 * 1000)
#define IPC_CAM_FULL_BACKOFF_US (30 * 1000 * 1000)  /* avoid log spam when card is full */

static pthread_t s_worker;
static volatile int s_running;
static volatile int s_worker_started;

static media_recorder_handle_t s_recorder;
static int s_main_hold_active; /* 1 while segment gap holds main stream */
static uint32_t s_full_warn_last_s; /* rate-limit storage-full warnings */

/* Segment timing must use a monotonic clock: the device boots with an unset wall
 * clock and steps it when NTP lands, so time(NULL) deltas can underflow (segment
 * cut instantly) or jump. Wall clock is still fine for the segment *filename*. */
static uint32_t ipc_cam_monotonic_s(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (uint32_t)time(NULL);
    return (uint32_t)ts.tv_sec;
}

/* Status shared with the LVGL UI thread (plain ints, single-writer worker). */
static volatile int s_st_recording;
static volatile int s_st_card_missing;
static volatile int s_st_storage_full;
static volatile int s_st_paused;
static volatile int s_st_elapsed_s;
static volatile int s_st_session_base_s;   /* finished segments total */
static volatile int s_st_session_elapsed_s;
static volatile int s_st_segment_count;
static volatile long s_st_free_mb = -1;

static void stamp_segment_path(char *buf, size_t buf_size, const char *dir)
{
    time_t t = time(NULL);
    struct tm tmv;
    struct tm *tm = localtime_r(&t, &tmv);

    if (!buf || buf_size == 0)
        return;

    if (!tm) {
        snprintf(buf, buf_size, "%s/ipcam.mp4", dir ? dir : "");
        return;
    }

    snprintf(buf, buf_size, "%s/DVR_%04d%02d%02d_%02d%02d%02d.mp4",
             dir ? dir : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static int find_stream_role(multimedia_t *mm, const char *role)
{
    int idx = -1;

    if (mm && mm->camera && mm->camera->stream_index_by_role &&
        mm->camera->stream_index_by_role(role, &idx) == 0 && idx >= 0)
        return idx;
    return -1;
}

/*
 * Scheme A: before recorder stop/destroy unregs the main client, take a
 * lightweight stream_hold so VIDEO_RECORD mask / main VENC stay up. That avoids
 * stream_apply flipping main into VI-keepalive and batch-reconfiguring RTSP sub.
 */
static void main_stream_hold_acquire(multimedia_t *mm)
{
    int stream_id;

    if (s_main_hold_active || mm == NULL || mm->camera == NULL ||
        mm->camera->stream_hold == NULL)
        return;

    stream_id = find_stream_role(mm, "main");
    if (stream_id < 0)
        stream_id = 0;

    if (mm->camera->stream_hold(stream_id) != 0) {
        log_warn("record: stream_hold(main=%d) failed", stream_id);
        return;
    }
    s_main_hold_active = 1;
    log_info("record: main stream hold acquired (segment gap)");
}

static void main_stream_hold_release(multimedia_t *mm)
{
    int stream_id;

    if (!s_main_hold_active)
        return;

    if (mm != NULL && mm->camera != NULL && mm->camera->stream_release_hold != NULL) {
        stream_id = find_stream_role(mm, "main");
        if (stream_id < 0)
            stream_id = 0;
        if (mm->camera->stream_release_hold(stream_id) != 0)
            log_warn("record: stream_release_hold(main=%d) failed", stream_id);
        else
            log_info("record: main stream hold released");
    }
    s_main_hold_active = 0;
}

/* Log main-stream encode params from the already-loaded video JSON
 * (fw_video_config.json via load_config). Do not hardcode width/fps/bitrate
 * here — HAL/VENC already use streams_cfg from that file; stream_set_enc_param is
 * currently a no-op for runtime re-open. */
static void log_main_video_from_config(multimedia_t *mm)
{
    media_video_enc_param_t param;
    int stream_id;

    if (!mm || !mm->camera || !mm->camera->stream_get_enc_param)
        return;

    stream_id = find_stream_role(mm, "main");
    if (stream_id < 0)
        stream_id = 0;

    memset(&param, 0, sizeof(param));
    if (mm->camera->stream_get_enc_param(stream_id, &param) != 0) {
        log_warn("record: stream_get_enc_param(main=%d) failed", stream_id);
        return;
    }

    log_info("record: main from video cfg stream=%d %dx%d@%dfps bitrate=%d gop=%d codec=%d",
             stream_id, param.width, param.height, param.framerate,
             param.bitrate, param.gop, (int)param.codec);
}

static void set_audio_g711a(multimedia_t *mm)
{
    media_audio_enc_param_t param;

    if (!mm || !mm->audio || !mm->audio->stream_set_enc_param)
        return;

    memset(&param, 0, sizeof(param));
    if (mm->audio->stream_get_enc_param)
        mm->audio->stream_get_enc_param(0, &param);

    /* Match fw_audio_config.ini: G711A @ 8 kHz mono — RTSP PCMA and MP4 alaw. */
    param.codec = MEDIA_CODEC_AUDIO_G711A;
    param.enable = 1;
    param.sample_rate = 8000;
    if (param.channels <= 0)
        param.channels = 1;
    if (param.sample_bits <= 0)
        param.sample_bits = 16;

    (void)mm->audio->stream_set_enc_param(0, &param);
    log_info("record: audio enc G711A %dHz ch=%d", param.sample_rate, param.channels);
}

void ipc_cam_audio_ensure_g711a(void)
{
    set_audio_g711a((multimedia_t *)app_main_get_multimedia());
}

static int segment_start(multimedia_t *mm, const char *path)
{
    media_recorder_profile_t profile;
    int ret;

    ret = mm->recorder->create(&s_recorder);
    if (ret != 0 || s_recorder == NULL)
        return -1;

    memset(&profile, 0, sizeof(profile));
    profile.module_flags.enable_video = 1;
    profile.module_flags.enable_audio = 1;
    profile.output_flags = MEDIA_OUTPUT_CARD_STORAGE;

    ret = mm->recorder->set_profile(s_recorder, &profile);
    if (ret != 0)
        goto fail;

    ret = mm->recorder->set_output_file(s_recorder, path);
    if (ret != 0)
        goto fail;

    set_audio_g711a(mm);
    log_main_video_from_config(mm);

    ret = mm->recorder->prepare(s_recorder, NULL);
    if (ret != 0)
        goto fail;

    ret = mm->recorder->start(s_recorder);
    if (ret != 0)
        goto fail;

    return 0;

fail:
    log_warn("segment start failed ret=%d path=%s", ret, path);
    if (s_recorder) {
        mm->recorder->destroy(s_recorder);
        s_recorder = NULL;
    }
    return -1;
}

static void segment_stop(multimedia_t *mm)
{
    if (mm && mm->recorder && s_recorder) {
        /* Hold main before stop so unreg does not drop mask / flip keepalive. */
        main_stream_hold_acquire(mm);
        mm->recorder->stop(s_recorder);
        mm->recorder->destroy(s_recorder);
    }
    s_recorder = NULL;
}

static void *record_worker(void *arg)
{
    (void)arg;

    while (s_running) {
        multimedia_t *mm;
        char dir[300];
        char path[512];
        uint32_t seg_t0;

        s_st_free_mb = ipc_cam_storage_get_free_mb();

        /* Manual stop: keep the worker alive but do not open segments. */
        if (s_st_paused) {
            main_stream_hold_release((multimedia_t *)app_main_get_multimedia());
            s_st_recording = 0;
            for (int i = 0; i < 10 && s_running; i++)
                usleep(IPC_CAM_POLL_US);
            continue;
        }

        if (!ipc_cam_storage_is_ready() ||
            ipc_cam_storage_get_dir(dir, sizeof(dir)) != 0) {
            main_stream_hold_release((multimedia_t *)app_main_get_multimedia());
            s_st_card_missing = 1;
            s_st_storage_full = 0;
            s_st_recording = 0;
            for (int i = 0; i < 10 && s_running; i++)
                usleep(IPC_CAM_POLL_US);
            continue;
        }
        s_st_card_missing = 0;

        if (ipc_cam_storage_ensure_free(IPC_CAM_MIN_FREE_MB) != 0) {
            /* Card present but full and nothing could be freed: do NOT start a
             * segment (would fail or write a truncated MP4). Surface the state
             * and retry later instead of churning the recorder. */
            main_stream_hold_release((multimedia_t *)app_main_get_multimedia());
            s_st_storage_full = 1;
            s_st_recording = 0;
            {
                uint32_t now = ipc_cam_monotonic_s();
                if (s_full_warn_last_s == 0 || now - s_full_warn_last_s >= 300u) {
                    log_warn("record: storage full (free=%ldMB < need=%dMB); pause until space available",
                             ipc_cam_storage_get_free_mb(), IPC_CAM_MIN_FREE_MB);
                    s_full_warn_last_s = now;
                }
            }
            usleep(IPC_CAM_FULL_BACKOFF_US);
            continue;
        }
        s_st_storage_full = 0;

        mm = (multimedia_t *)app_main_get_multimedia();
        if (mm == NULL || mm->recorder == NULL) {
            log_warn("record: multimedia recorder unavailable");
            for (int i = 0; i < 10 && s_running; i++)
                usleep(IPC_CAM_POLL_US);
            continue;
        }

        stamp_segment_path(path, sizeof(path), dir);
        if (segment_start(mm, path) != 0) {
            /* Keep hold across a failed start so the next retry still bridges;
             * release only when leaving the record loop or pausing. */
            for (int i = 0; i < 10 && s_running; i++)
                usleep(IPC_CAM_POLL_US);
            continue;
        }
        /* New segment's rec_write client is registered; hold no longer needed. */
        main_stream_hold_release(mm);

        seg_t0 = ipc_cam_monotonic_s();
        log_info("record: segment started %s", path);
        s_st_recording = 1;

        while (s_running && !s_st_paused) {
            uint32_t now = ipc_cam_monotonic_s();
            uint32_t elapsed_s = now - seg_t0;

            s_st_elapsed_s = (int)elapsed_s;
            s_st_session_elapsed_s = s_st_session_base_s + (int)elapsed_s;
            if (elapsed_s * 1000u >= (uint32_t)IPC_CAM_SEGMENT_MS)
                break;
            usleep(IPC_CAM_POLL_US);
        }

        segment_stop(mm);
        s_st_recording = 0;
        s_st_session_base_s += s_st_elapsed_s;
        s_st_session_elapsed_s = s_st_session_base_s;
        s_st_segment_count++;
        log_info("record: segment finished %s (%ds, session %ds)",
                 path, s_st_elapsed_s, s_st_session_elapsed_s);
        s_st_elapsed_s = 0;

        /* Manual pause / worker exit: drop hold so main can go keepalive or idle. */
        if (!s_running || s_st_paused)
            main_stream_hold_release(mm);
    }

    main_stream_hold_release((multimedia_t *)app_main_get_multimedia());

    /* segment_stop() above already ran for any started segment before the loop
     * exits (s_recorder is NULL here), so no extra flush is needed. */
    return NULL;
}

int ipc_cam_record_start(void)
{
    multimedia_t *mm;

    if (s_worker_started)
        return 0;

    /* Apply G711A before RTSP DESCRIBE so SDP advertises PCMA clients will receive. */
    mm = (multimedia_t *)app_main_get_multimedia();
    set_audio_g711a(mm);

    s_running = 1;
    s_st_segment_count = 0;
    s_st_elapsed_s = 0;
    s_st_session_base_s = 0;
    s_st_session_elapsed_s = 0;
    s_st_recording = 0;
    s_st_storage_full = 0;
    s_st_paused = 0;

    if (pthread_create(&s_worker, NULL, record_worker, NULL) != 0) {
        log_error("record: worker thread create failed");
        s_running = 0;
        return -1;
    }

    s_worker_started = 1;
    log_info("record: loop recording started");
    return 0;
}

void ipc_cam_record_stop(void)
{
    if (!s_worker_started)
        return;

    s_running = 0;
    pthread_join(s_worker, NULL);
    s_worker_started = 0;
    s_st_recording = 0;
    log_info("record: loop recording stopped");
}

int ipc_cam_record_is_active(void)
{
    return s_worker_started;
}

void ipc_cam_record_set_paused(int paused)
{
    s_st_paused = paused ? 1 : 0;
    log_info("record: %s", s_st_paused ? "paused (manual stop)" : "resumed");
}

int ipc_cam_record_is_paused(void)
{
    return s_st_paused;
}

void ipc_cam_record_get_status(ipc_cam_record_status_t *out)
{
    if (!out)
        return;

    out->recording = s_st_recording;
    out->card_missing = s_st_card_missing;
    out->storage_full = s_st_storage_full;
    out->paused = s_st_paused;
    out->segment_elapsed_s = s_st_elapsed_s;
    out->session_elapsed_s = s_st_session_elapsed_s;
    out->segment_count = s_st_segment_count;
    out->free_mb = s_st_free_mb;
}
