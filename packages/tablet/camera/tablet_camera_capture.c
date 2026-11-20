// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Photo capture and short MP4 recording for Nova camera.
 * Preview (NV12 broadcast) stays registered; picture role JPEG avoids tearing down preview.
 */
#define LOG_TAG "TABLET_CAMERA_CAPTURE"

#include "tablet_camera_capture.h"
#include "tablet_camera_storage.h"

#include "app_main.h"
#include "multimedia.h"
#include "log.h"
#include "tablet_common.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PHOTO_SUB_MAGIC "NOVAJPG2"

typedef struct {
    char magic[8];
    uint32_t sub_size;
    uint16_t sub_w;
    uint16_t sub_h;
} photo_sub_footer_t;

static int photo_jpeg_size(const uint8_t *p, size_t len, uint16_t *w, uint16_t *h)
{
    size_t i;

    if (!p || len < 10 || p[0] != 0xff || p[1] != 0xd8)
        return -1;
    i = 2;
    while (i + 9 < len) {
        unsigned seglen;

        if (p[i] != 0xff) {
            i++;
            continue;
        }
        while (i < len && p[i] == 0xff)
            i++;
        if (i >= len)
            break;
        if (p[i] == 0xc0 || p[i] == 0xc2) {
            *h = (uint16_t)((p[i + 4] << 8) | p[i + 5]);
            *w = (uint16_t)((p[i + 6] << 8) | p[i + 7]);
            return (*w >= 16 && *h >= 16 && (*w % 16) == 0 && (*h % 16) == 0) ? 0 : -1;
        }
        if (p[i] == 0xd8 || p[i] == 0xd9 || p[i] == 0x01 || (p[i] >= 0xd0 && p[i] <= 0xd7)) {
            i++;
            continue;
        }
        if (i + 2 >= len)
            return -1;
        seglen = ((unsigned)p[i + 1] << 8) | p[i + 2];
        if (seglen < 2)
            return -1;
        i += 1 + seglen;
    }
    return -1;
}

static int photo_append_sub(const char *path, const void *jpg, size_t len)
{
    photo_sub_footer_t footer;
    uint16_t w = 0;
    uint16_t h = 0;
    FILE *fp;

    if (photo_jpeg_size((const uint8_t *)jpg, len, &w, &h) != 0)
        return -1;
    fp = fopen(path, "ab");
    if (!fp)
        return -1;
    if (fwrite(jpg, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    memset(&footer, 0, sizeof(footer));
    memcpy(footer.magic, PHOTO_SUB_MAGIC, 8);
    footer.sub_size = (uint32_t)len;
    footer.sub_w = w;
    footer.sub_h = h;
    if (fwrite(&footer, 1, sizeof(footer), fp) != sizeof(footer)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    log_info("photo: sub jpeg %ux%u %zu bytes", (unsigned)w, (unsigned)h, len);
    return 0;
}

int tablet_camera_photo_load_sub(const char *path, void **data, size_t *size,
                                 unsigned *width, unsigned *height)
{
    photo_sub_footer_t footer;
    FILE *fp;
    long file_size;
    void *buf;

    if (!path || !data || !size)
        return -1;
    *data = NULL;
    *size = 0;
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size < (long)sizeof(footer)) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, file_size - (long)sizeof(footer), SEEK_SET) != 0 ||
        fread(&footer, 1, sizeof(footer), fp) != sizeof(footer) ||
        memcmp(footer.magic, PHOTO_SUB_MAGIC, 8) != 0 ||
        footer.sub_size < 10 ||
        (long)footer.sub_size + (long)sizeof(footer) > file_size) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, file_size - (long)sizeof(footer) - (long)footer.sub_size, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buf = malloc(footer.sub_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, footer.sub_size, fp) != footer.sub_size) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *data = buf;
    *size = footer.sub_size;
    if (width)
        *width = footer.sub_w;
    if (height)
        *height = footer.sub_h;
    return 0;
}

static media_recorder_handle_t s_recorder;
static int s_recording;
static char s_video_path[360];

static int main_stream_index(multimedia_t *mm)
{
    int idx = -1;
    tablet_camera_streams_t st;

    if (mm && tablet_camera_streams_resolve(mm, &st) == 0 && st.idx_main >= 0)
        return st.idx_main;
    if (mm && mm->camera && mm->camera->stream_index_by_role &&
        mm->camera->stream_index_by_role("main", &idx) == 0 && idx >= 0)
        return idx;
    return 0;
}

static int capture_picture_stream_index(multimedia_t *mm)
{
    int idx = -1;

    if (mm && mm->camera && mm->camera->stream_index_by_role &&
        mm->camera->stream_index_by_role("picture", &idx) == 0 && idx >= 0)
        return idx;

    return main_stream_index(mm);
}

static int set_main_video_h264(multimedia_t *mm, uint32_t width, uint32_t height)
{
    media_video_enc_param_t param = {0};
    int stream_id;

    if (!mm || !mm->camera || !mm->camera->stream_set_enc_param)
        return -1;

    stream_id = main_stream_index(mm);

    if (mm->camera->stream_get_enc_param)
        mm->camera->stream_get_enc_param(stream_id, &param);

    param.codec = MEDIA_CODEC_VIDEO_H264;
    if (width > 0)
        param.width = width;
    if (height > 0)
        param.height = height;
    if (param.width <= 0)
        param.width = 3264;
    if (param.height <= 0)
        param.height = 2448;
    if (param.framerate <= 0)
        param.framerate = 20;
    if (param.bitrate <= 0)
        param.bitrate = 8192;

    return mm->camera->stream_set_enc_param(stream_id, &param);
}

static int set_audio_aac(multimedia_t *mm)
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

int tablet_camera_take_photo(char *out_path, size_t out_len)
{
    multimedia_t *mm;
    char dir[256];
    char jpg_path[360];
    media_capture_picture_param_t param;
    void *jpeg_data = NULL;
    size_t jpeg_len = 0;
    int ret;
    int stream_idx;

    if (!tablet_album_is_ready())
        return TABLET_ALBUM_ERR_NOT_READY;

    if (tablet_album_ensure_subdir(TABLET_PHOTO_SUBDIR, dir, sizeof(dir)) != 0)
        return -1;

    tablet_album_stamp_path(jpg_path, sizeof(jpg_path), dir, TABLET_PHOTO_PREFIX, ".jpg");

    mm = (multimedia_t *)app_main_get_multimedia();
    if (mm == NULL || mm->camera == NULL || !mm->camera->capture_picture)
        return -1;

    memset(&param, 0, sizeof(param));
    param.save_path = jpg_path;
    param.format = 1;

    stream_idx = capture_picture_stream_index(mm);
    log_info("take_photo: stream=%d path=%s", stream_idx, jpg_path);
    ret = mm->camera->capture_picture(stream_idx, &param, &jpeg_data, &jpeg_len);
    log_info("take_photo: ret=%d jpeg_len=%zu", ret, jpeg_len);
    if (jpeg_data)
        free(jpeg_data);

    if (ret != 0) {
        log_warn("photo: capture or save failed");
        return -1;
    }

    {
        int sub_idx = -1;
        media_capture_picture_param_t sub_param;
        void *sub_data = NULL;
        size_t sub_len = 0;

        if (mm->camera->stream_index_by_role &&
            mm->camera->stream_index_by_role("picture_bg", &sub_idx) == 0 &&
            sub_idx >= 0) {
            int attempt;

            /* Main close runs after the JPEG is copied. Give that close a moment,
             * then try again if the snapshot encoder is still open. */
            for (attempt = 0; attempt < 3; attempt++) {
                usleep(attempt == 0 ? 100 * 1000 : 200 * 1000);
                sub_data = NULL;
                sub_len = 0;
                memset(&sub_param, 0, sizeof(sub_param));
                sub_param.format = 1;
                if (mm->camera->capture_picture(sub_idx, &sub_param, &sub_data, &sub_len) == 0 &&
                    sub_data && sub_len > 0 &&
                    photo_append_sub(jpg_path, sub_data, sub_len) == 0) {
                    free(sub_data);
                    sub_data = NULL;
                    break;
                }
                log_warn("photo: sub jpeg capture failed attempt=%d", attempt + 1);
                if (sub_data)
                    free(sub_data);
                sub_data = NULL;
            }
        }
    }

    if (out_path && out_len > 0)
        snprintf(out_path, out_len, "%s", jpg_path);

    log_info("take_photo: saved %s", jpg_path);
    return 0;
}

int tablet_camera_record_start(void)
{
    multimedia_t *mm;
    char dir[256];
    media_recorder_profile_t profile;
    tablet_camera_streams_t st;
    uint32_t main_w = 3264;
    uint32_t main_h = 2448;
    int ret = -1;

    if (s_recording)
        return 0;

    if (!tablet_album_is_ready())
        return TABLET_ALBUM_ERR_NOT_READY;

    if (tablet_album_ensure_subdir(TABLET_RECORD_SUBDIR, dir, sizeof(dir)) != 0)
        return -1;

    mm = (multimedia_t *)app_main_get_multimedia();
    if (mm == NULL || mm->recorder == NULL)
        return -1;

    memset(&st, 0, sizeof(st));
    if (tablet_camera_streams_resolve(mm, &st) == 0 && st.main_w > 0 && st.main_h > 0) {
        main_w = (uint32_t)st.main_w;
        main_h = (uint32_t)st.main_h;
    }

    tablet_album_stamp_path(s_video_path, sizeof(s_video_path), dir, TABLET_VIDEO_PREFIX, ".mp4");

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

    ret = mm->recorder->set_output_file(s_recorder, s_video_path);
    if (ret != 0)
        goto fail;

    ret = set_audio_aac(mm);
    if (ret != 0)
        log_warn("record: audio stream_set_enc_param failed (%d), policy may override", ret);

    ret = set_main_video_h264(mm, main_w, main_h);
    if (ret != 0)
        log_warn("record: video stream_set_enc_param failed (%d), use policy default", ret);

    {
        media_dynamic_param_t dyn;
        int main_idx = main_stream_index(mm);
        int sub_idx = -1;

        if (!mm->camera || !mm->camera->stream_index_by_role ||
            mm->camera->stream_index_by_role("preview_bg", &sub_idx) != 0 ||
            sub_idx < 0) {
            log_error("record: preview_bg H.264 stream missing");
            goto fail;
        }
        memset(&dyn, 0, sizeof(dyn));
        dyn.group_id = 0;
        dyn.video_stream_index = main_idx;
        dyn.video_stream_index_sub = sub_idx;
        dyn.video_param.bitrate = -1;
        dyn.video_param.framerate = -1;
        dyn.video_param.gop = -1;
        dyn.audio_param.bitrate = -1;
        dyn.audio_param.sample_rate = -1;
        dyn.source_id = -1;
        if (mm->recorder->set_dynamic_param == NULL ||
            mm->recorder->set_dynamic_param(s_recorder, &dyn) != 0) {
            log_error("record: set_dynamic_param failed");
            goto fail;
        }
        log_info("record: main stream %d + preview_bg stream %d", main_idx, sub_idx);
    }

    ret = mm->recorder->prepare(s_recorder, NULL);
    if (ret != 0)
        goto fail;

    ret = mm->recorder->start(s_recorder);
    if (ret != 0)
        goto fail;

    s_recording = 1;
    log_info("recording started: %s (%ux%u h264+h264+aac)", s_video_path, main_w, main_h);
    return 0;

fail:
    log_warn("record_start: failed ret=%d", ret);
    if (mm && mm->recorder && s_recorder) {
        mm->recorder->destroy(s_recorder);
        s_recorder = NULL;
    }
    s_video_path[0] = '\0';
    return -1;
}

int tablet_camera_record_stop(void)
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

    log_info("recording stopped: %s", s_video_path[0] ? s_video_path : "(none)");
    s_video_path[0] = '\0';
    return 0;
}

int tablet_camera_record_is_active(void)
{
    return s_recording;
}
