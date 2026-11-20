// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Photo capture and video recording for Luna camera.
 * Sub-channel preview (ch1) stays on LCD; photo/record only adds main-channel encode/save.
 *
 * Files are saved as LCM2 composite (WEARABLE_PHOTO_EXT / WEARABLE_VIDEO_EXT):
 *   - Photo: NV12 preview frame  +  full-res JPEG
 *   - Video: NV12 preview stream +  full-res H.264/AAC MP4
 * The gallery reads the embedded NV12 for fast thumbnail/playback display.
 */
#define LOG_TAG "WEARABLE_CAMERA_CAPTURE"

#include "wearable_camera_capture.h"
#include "wearable_camera_container.h"
#include "wearable_camera_nv12.h"
#include "wearable_camera_streams.h"
#include "wearable_camera_tfcard.h"

#include "app_main.h"
#include "gui_camera.h"
#include "multimedia.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static media_recorder_handle_t s_recorder;
static int s_recording;
static char s_video_path[360]; /* LCM file path (or plain MP4 on fallback) */
static char s_mp4_path[400];   /* MP4 path: high_tmp inside LCM, or same as s_video_path on fallback */
static int s_lcm_video;        /* 1 when recording into an active LCM2 container */

/* Max NV12 buffer for preview probe (handles up to 640×480). */
#define PREVIEW_BUF_MAX  (640u * 480u * 3u / 2u)

static void stamp_media_path(char *buf, size_t buf_size, const char *dir,
                             const char *prefix, const char *ext)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (!buf || buf_size == 0)
        return;

    if (!tm) {
        snprintf(buf, buf_size, "%s/%s%s", dir ? dir : "", prefix ? prefix : "", ext ? ext : "");
        return;
    }

    snprintf(buf, buf_size, "%s/%s_%04d%02d%02d_%02d%02d%02d%s",
             dir ? dir : "", prefix ? prefix : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, ext ? ext : "");
}

static int main_stream_index(void)
{
    const gui_camera_streams_t *st = wearable_camera_streams_get();

    if (st && st->idx_main >= 0)
        return st->idx_main;
    return 0;
}

static int capture_picture_stream_index(multimedia_t *mm)
{
    int idx = -1;

    if (mm && mm->camera && mm->camera->stream_index_by_role &&
        mm->camera->stream_index_by_role("picture", &idx) == 0 && idx >= 0)
        return idx;

    return main_stream_index();
}

static int wearable_camera_set_main_video_h264(multimedia_t *mm, uint32_t width, uint32_t height)
{
    media_video_enc_param_t param = {0};
    int stream_id;

    if (!mm || !mm->camera || !mm->camera->stream_set_enc_param)
        return -1;

    stream_id = main_stream_index();

    if (mm->camera->stream_get_enc_param)
        mm->camera->stream_get_enc_param(stream_id, &param);

    param.codec = MEDIA_CODEC_VIDEO_H264;
    if (width > 0)
        param.width = width;
    if (height > 0)
        param.height = height;
    if (param.width <= 0)
        param.width = 1600;
    if (param.height <= 0)
        param.height = 1200;
    if (param.framerate <= 0)
        param.framerate = 20;
    if (param.bitrate <= 0)
        param.bitrate = 2048;

    return mm->camera->stream_set_enc_param(stream_id, &param);
}

static int wearable_camera_set_audio_aac(multimedia_t *mm)
{
    media_audio_enc_param_t param = {0};
    int ret;

    if (!mm || !mm->audio || !mm->audio->stream_set_enc_param)
        return -1;

    if (mm->audio->stream_get_enc_param)
        mm->audio->stream_get_enc_param(0, &param);

    param.codec = MEDIA_CODEC_AUDIO_AAC;
    param.enable = 1;
    if (param.sample_rate <= 0)
        param.sample_rate = 16000;
    if (param.channels <= 0)
        param.channels = 1;
    if (param.sample_bits <= 0)
        param.sample_bits = 16;

    ret = mm->audio->stream_set_enc_param(0, &param);
    if (ret == 0) {
        log_info("record: audio codec set to AAC (%dHz, %dch)",
                 param.sample_rate, param.channels);
    }
    return ret;
}

int wearable_camera_capture_to_tfcard(char *out_path, size_t out_len)
{
    multimedia_t *mm;
    char dir[256];
    char lcm_path[360];
    media_capture_picture_param_t param;
    void *jpeg_data = NULL;
    size_t jpeg_len = 0;
    void *nv12_buf = NULL;
    uint32_t pw = 0, ph = 0;
    int ret;
    int stream_idx;

    if (!wearable_tfcard_is_ready())
        return WEARABLE_TFCARD_ERR_NOT_READY;

    if (wearable_tfcard_build_subdir(WEARABLE_PHOTO_SUBDIR, dir, sizeof(dir)) != 0)
        return -1;

    (void)wearable_camera_streams_refresh();

    wearable_lcm_photo_path(dir, lcm_path, sizeof(lcm_path));

    mm = (multimedia_t *)app_main_get_multimedia();
    if (mm == NULL || mm->camera == NULL || !mm->camera->capture_picture)
        return -1;

    /* Grab a preview NV12 frame before the shutter (preview still live). */
    nv12_buf = malloc(PREVIEW_BUF_MAX);
    if (nv12_buf) {
        if (wearable_nv12_capture_preview_buffer(mm, 0, &pw, &ph, nv12_buf, PREVIEW_BUF_MAX) != 0) {
            free(nv12_buf);
            nv12_buf = NULL;
            pw = ph = 0;
        }
    }

    /* Capture JPEG into memory (save_path=NULL → data/size returned to caller). */
    memset(&param, 0, sizeof(param));
    param.save_path = NULL;
    param.format = 1;

    stream_idx = capture_picture_stream_index(mm);
    log_info("take_photo: stream=%d path=%s preview=%ux%u", stream_idx, lcm_path, pw, ph);
    ret = mm->camera->capture_picture(stream_idx, &param, &jpeg_data, &jpeg_len);
    log_info("take_photo: ret=%d jpeg_len=%zu", ret, jpeg_len);

    if (ret == 0 && jpeg_data && jpeg_len > 0 && nv12_buf && pw > 0 && ph > 0) {
        size_t nv12_len = wearable_lcm_preview_frame_bytes(pw, ph);
        ret = wearable_lcm_photo_write(lcm_path, pw, ph, nv12_buf, nv12_len, jpeg_data, jpeg_len);
        if (ret != 0)
            log_warn("take_photo: lcm_photo_write failed");
    } else if (ret == 0) {
        log_warn("take_photo: no preview captured (pw=%u ph=%u), discarding", pw, ph);
        ret = -1;
    }

    free(nv12_buf);
    free(jpeg_data);

    if (ret != 0) {
        log_warn("photo: capture or lcm save failed");
        return -1;
    }

    if (out_path && out_len > 0)
        snprintf(out_path, out_len, "%s", lcm_path);

    log_info("take_photo: saved %s", lcm_path);
    return 0;
}

void wearable_camera_finish_after_capture(void)
{
    /* Preview sub-channel stays registered; nothing to restart. */
}

int wearable_camera_take_photo(char *out_path, size_t out_len)
{
    return wearable_camera_capture_to_tfcard(out_path, out_len);
}

int wearable_camera_record_start(void)
{
    multimedia_t *mm;
    char dir[256];
    media_recorder_profile_t profile;
    const gui_camera_streams_t *st;
    uint32_t main_w;
    uint32_t main_h;
    void *probe_buf;
    uint32_t pw = 0, ph = 0;
    const char *high_tmp;
    int ret;

    if (s_recording)
        return 0;

    if (!wearable_tfcard_is_ready())
        return WEARABLE_TFCARD_ERR_NOT_READY;

    if (wearable_tfcard_build_subdir(WEARABLE_RECORD_SUBDIR, dir, sizeof(dir)) != 0)
        return -1;

    (void)wearable_camera_streams_refresh();
    st = wearable_camera_streams_get();
    main_w = (st && st->main_w > 0) ? (uint32_t)st->main_w : 1600u;
    main_h = (st && st->main_h > 0) ? (uint32_t)st->main_h : 1200u;

    mm = (multimedia_t *)app_main_get_multimedia();
    if (mm == NULL || mm->recorder == NULL)
        return -1;

    /* Probe preview dimensions for the LCM2 container. */
    probe_buf = malloc(PREVIEW_BUF_MAX);
    if (probe_buf) {
        (void)wearable_nv12_capture_preview_buffer(mm, 0, &pw, &ph, probe_buf, PREVIEW_BUF_MAX);
        free(probe_buf);
    }

    /* Generate LCM path and try to start the composite container. */
    wearable_lcm_video_path(dir, s_video_path, sizeof(s_video_path));
    s_lcm_video = 0;
    s_mp4_path[0] = '\0';

    if (pw > 0 && ph > 0 && wearable_lcm_video_begin(s_video_path, pw, ph) == 0) {
        high_tmp = wearable_lcm_video_high_tmp_path();
        if (high_tmp) {
            snprintf(s_mp4_path, sizeof(s_mp4_path), "%s", high_tmp);
            wearable_nv12_record_start(mm, 0, pw, ph, NULL);
            s_lcm_video = 1;
            log_info("record: LCM2 container started %s preview=%ux%u", s_video_path, pw, ph);
        } else {
            wearable_lcm_video_abort();
        }
    }

    /* Fallback: plain MP4 when LCM init failed or no preview available. */
    if (!s_lcm_video) {
        log_warn("record: LCM2 init failed (pw=%u ph=%u), falling back to plain MP4", pw, ph);
        stamp_media_path(s_mp4_path, sizeof(s_mp4_path), dir, WEARABLE_VIDEO_PREFIX, ".mp4");
        snprintf(s_video_path, sizeof(s_video_path), "%s", s_mp4_path);
    }

    ret = mm->recorder->create(&s_recorder);
    if (ret != 0 || s_recorder == NULL)
        goto fail;

    memset(&profile, 0, sizeof(profile));
    profile.module_flags.enable_video = 1;
    profile.module_flags.enable_audio = 1;
    profile.output_flags = MEDIA_OUTPUT_CARD_STORAGE;

    ret = mm->recorder->set_profile(s_recorder, &profile);
    if (ret != 0)
        goto fail;

    ret = mm->recorder->set_output_file(s_recorder, s_mp4_path);
    if (ret != 0)
        goto fail;

    ret = wearable_camera_set_audio_aac(mm);
    if (ret != 0)
        log_warn("record: audio stream_set_enc_param failed (%d), policy may override", ret);

    ret = wearable_camera_set_main_video_h264(mm, main_w, main_h);
    if (ret != 0)
        log_warn("record: video stream_set_enc_param failed (%d), use policy default", ret);

    ret = mm->recorder->prepare(s_recorder, NULL);
    if (ret != 0)
        goto fail;

    ret = mm->recorder->start(s_recorder);
    if (ret != 0)
        goto fail;

    s_recording = 1;
    log_info("recording started: %s mp4=%s (%ux%u h264+aac)",
             s_video_path, s_mp4_path, main_w, main_h);
    return 0;

fail:
    log_warn("record_start: failed ret=%d", ret);
    if (s_lcm_video) {
        wearable_nv12_record_stop();
        wearable_lcm_video_abort();
        s_lcm_video = 0;
    }
    if (mm && mm->recorder && s_recorder) {
        mm->recorder->destroy(s_recorder);
        s_recorder = NULL;
    }
    s_video_path[0] = '\0';
    s_mp4_path[0] = '\0';
    log_warn("record start failed");
    return -1;
}

int wearable_camera_record_stop(void)
{
    multimedia_t *mm;

    if (!s_recording)
        return 0;

    mm = (multimedia_t *)app_main_get_multimedia();
    if (mm != NULL && mm->recorder != NULL && s_recorder != NULL) {
        mm->recorder->stop(s_recorder);
        mm->recorder->destroy(s_recorder);
    }
    s_recorder = NULL;
    s_recording = 0;

    if (s_lcm_video) {
        wearable_nv12_record_stop();
        if (wearable_lcm_video_finish(NULL) != 0)
            log_warn("record_stop: lcm_video_finish failed for %s", s_video_path);
        s_lcm_video = 0;
    }

    log_info("recording stopped: %s", s_video_path[0] ? s_video_path : "(none)");
    s_video_path[0] = '\0';
    s_mp4_path[0] = '\0';

    return 0;
}

int wearable_camera_record_is_active(void)
{
    return s_recording;
}
