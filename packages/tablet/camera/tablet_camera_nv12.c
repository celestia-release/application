// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * NV12 preview sidecar recorder for Nova tablet camera.
 * Captures the 800x480 NV12 preview stream during H.264 recording,
 * writing raw frames to a temp file.  On stop the frames are appended
 * to the MP4 together with a NOVAPREV footer (see tablet_camera_nv12.h).
 */
#define LOG_TAG "TABLET_CAMERA_NV12"

#include "tablet_camera_nv12.h"

#include "app_main.h"
#include "log.h"
#include "system_thread.h"
#include "system_time.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NV12_RECORD_CLIENT_INVALID  (-1)
#define NV12_RECORD_POLL_MS         20u
#define NV12_WRITE_FAIL_LOG_MAX     8u

static volatile int s_recording;
static pthread_t s_worker;
static volatile int s_worker_running;
static int s_client_handle = NV12_RECORD_CLIENT_INVALID;
static uint32_t s_w;
static uint32_t s_h;
static uint32_t s_fps;
static FILE *s_tmp_fp;
static char s_tmp_path[400];
static void *s_pack_buf;
static size_t s_pack_buf_size;
static unsigned int s_frames_written;
static unsigned int s_write_fail_count;

static int frame_is_nv12(const media_frame_info_t *frame)
{
    size_t need;

    if (!frame || !frame->data || frame->width <= 0 || frame->height <= 0)
        return 0;

    need = nova_nv12_frame_size((uint32_t)frame->width, (uint32_t)frame->height);
    if (frame->data_len < need)
        return 0;

    if (frame->code_type == MEDIA_CODEC_VIDEO_NV12 || frame->code_type == 3)
        return 1;

    return 0;
}

static uint32_t nv12_src_stride(const media_frame_info_t *frame)
{
    uint32_t w = (uint32_t)frame->width;
    uint32_t h = (uint32_t)frame->height;
    size_t tight;
    uint32_t inferred;

    if (w == 0 || h == 0)
        return 0;

    tight = nova_nv12_frame_size(w, h);
    if (frame->data_len <= tight)
        return w;

    inferred = (uint32_t)((frame->data_len * 2u) / (h * 3u));
    return inferred >= w ? inferred : w;
}

static int nv12_pack_tight(const media_frame_info_t *frame, uint32_t out_w, uint32_t out_h,
                           void *out, size_t out_size)
{
    const uint8_t *src = (const uint8_t *)frame->data;
    uint8_t *dst = (uint8_t *)out;
    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t src_stride;
    size_t need;
    uint32_t y;

    if (!src || out_w == 0 || out_h == 0)
        return -1;

    need = nova_nv12_frame_size(out_w, out_h);
    if (out_size < need)
        return -1;
    if (src_w < out_w || src_h < out_h)
        return -1;

    src_stride = nv12_src_stride(frame);
    if (src_stride == 0)
        return -1;

    if (src_stride == out_w && src_w == out_w && src_h == out_h) {
        memcpy(out, src, need);
        return 0;
    }

    for (y = 0; y < out_h; y++)
        memcpy(dst + (size_t)y * out_w, src + (size_t)y * src_stride, out_w);

    src += (size_t)src_stride * src_h;
    dst += (size_t)out_w * out_h;
    for (y = 0; y < out_h / 2u; y++)
        memcpy(dst + (size_t)y * out_w, src + (size_t)y * src_stride, out_w);

    return 0;
}

static int nv12_record_write_frame(const media_frame_info_t *frame)
{
    const void *payload;
    size_t payload_len;
    size_t n;

    if (!frame_is_nv12(frame) || !s_tmp_fp)
        return -1;

    if ((uint32_t)frame->width == s_w && (uint32_t)frame->height == s_h &&
        nv12_src_stride(frame) == s_w) {
        payload = frame->data;
        payload_len = nova_nv12_frame_size(s_w, s_h);
    } else {
        if (nv12_pack_tight(frame, s_w, s_h, s_pack_buf, s_pack_buf_size) != 0)
            return -1;
        payload = s_pack_buf;
        payload_len = nova_nv12_frame_size(s_w, s_h);
    }

    n = fwrite(payload, 1, payload_len, s_tmp_fp);
    if (n != payload_len) {
        s_write_fail_count++;
        if (s_write_fail_count <= NV12_WRITE_FAIL_LOG_MAX)
            log_warn("nv12 write failed: wrote %zu of %zu (fail=%u)",
                     n, payload_len, s_write_fail_count);
        return -1;
    }

    s_frames_written++;
    if (s_frames_written == 1u || (s_frames_written % 150u) == 0u)
        log_info("nv12 record: %u frames written (%ux%u)", s_frames_written, s_w, s_h);
    return 0;
}

static void nv12_record_release_client(multimedia_t *mm)
{
    if (!mm || !mm->camera || s_client_handle == NV12_RECORD_CLIENT_INVALID)
        return;

    mm->camera->stream_unreg_client(s_client_handle);
    s_client_handle = NV12_RECORD_CLIENT_INVALID;
}

static int nv12_record_register_client(multimedia_t *mm, int stream_idx)
{
    int ret;

    if (!mm || !mm->camera || stream_idx < 0)
        return -1;
    if (s_client_handle != NV12_RECORD_CLIENT_INVALID)
        return 0;

    ret = mm->camera->stream_reg_client(stream_idx, &s_client_handle, NULL, NULL);
    if (ret != 0 || s_client_handle == NV12_RECORD_CLIENT_INVALID) {
        log_warn("nv12 record: stream_reg_client(%d) failed ret=%d", stream_idx, ret);
        s_client_handle = NV12_RECORD_CLIENT_INVALID;
        return -1;
    }

    log_info("nv12 record: client registered stream=%d handle=0x%x", stream_idx, s_client_handle);
    return 0;
}

static void *nv12_record_worker(void *arg)
{
    multimedia_t *mm = (multimedia_t *)arg;

    (void)thread_set_current_name("cam_nv12rec");

    while (s_worker_running) {
        media_frame_info_t frame = {0};
        int ret;

        if (!mm || !mm->camera || s_client_handle == NV12_RECORD_CLIENT_INVALID) {
            system_sleep_ms(NV12_RECORD_POLL_MS);
            continue;
        }

        ret = mm->camera->stream_get_frame(s_client_handle, &frame);
        if (ret != 0 || frame.data == NULL || frame.data_len == 0) {
            system_sleep_ms(NV12_RECORD_POLL_MS);
            continue;
        }

        (void)nv12_record_write_frame(&frame);
    }

    return NULL;
}

static void nv12_record_stop_worker(void)
{
    if (!s_worker_running)
        return;

    s_worker_running = 0;
    pthread_join(s_worker, NULL);
}

static int nv12_append_to_mp4(const char *mp4_path)
{
    FILE *src_fp;
    FILE *dst_fp;
    uint8_t buf[65536];
    size_t n;
    uint64_t data_size;
    nova_preview_footer_t footer;

    src_fp = fopen(s_tmp_path, "rb");
    if (!src_fp) {
        log_warn("nv12 append: cannot open temp %s", s_tmp_path);
        return -1;
    }

    dst_fp = fopen(mp4_path, "ab");
    if (!dst_fp) {
        log_warn("nv12 append: cannot open %s for append", mp4_path);
        fclose(src_fp);
        return -1;
    }

    data_size = 0;
    while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0) {
        if (fwrite(buf, 1, n, dst_fp) != n) {
            log_warn("nv12 append: write error after %llu bytes",
                     (unsigned long long)data_size);
            fclose(src_fp);
            fclose(dst_fp);
            return -1;
        }
        data_size += (uint64_t)n;
    }
    fclose(src_fp);

    memset(&footer, 0, sizeof(footer));
    memcpy(footer.magic, NOVA_PREVIEW_MAGIC, 8);
    footer.width     = s_w;
    footer.height    = s_h;
    footer.fps       = s_fps;
    footer.n_frames  = s_frames_written;
    footer.data_size = data_size;

    if (fwrite(&footer, 1, sizeof(footer), dst_fp) != sizeof(footer)) {
        log_warn("nv12 append: footer write failed");
        fclose(dst_fp);
        return -1;
    }
    fclose(dst_fp);

    log_info("nv12 append: %u frames (%llu bytes) + footer -> %s",
             s_frames_written, (unsigned long long)data_size, mp4_path);
    return 0;
}

int tablet_nv12_record_start(multimedia_t *mm, int preview_stream_idx,
                             uint32_t w, uint32_t h, uint32_t fps,
                             const char *mp4_path)
{
    size_t pack_need;

    if (s_recording)
        return 0;

    if (!mm || !mm->camera || w == 0 || h == 0 || !mp4_path)
        return -1;

    snprintf(s_tmp_path, sizeof(s_tmp_path), "%s.nv12tmp", mp4_path);

    s_tmp_fp = fopen(s_tmp_path, "wb");
    if (!s_tmp_fp) {
        log_warn("nv12 record: cannot create temp %s", s_tmp_path);
        return -1;
    }

    s_w   = w;
    s_h   = h;
    s_fps = fps > 0 ? fps : 15u;

    pack_need = nova_nv12_frame_size(s_w, s_h);
    if (!s_pack_buf || s_pack_buf_size < pack_need) {
        void *nb = realloc(s_pack_buf, pack_need);

        if (!nb) {
            fclose(s_tmp_fp);
            s_tmp_fp = NULL;
            return -1;
        }
        s_pack_buf      = nb;
        s_pack_buf_size = pack_need;
    }

    if (nv12_record_register_client(mm, preview_stream_idx) != 0) {
        fclose(s_tmp_fp);
        s_tmp_fp = NULL;
        return -1;
    }

    s_frames_written  = 0;
    s_write_fail_count = 0;
    s_worker_running  = 1;
    s_recording       = 1;

    if (pthread_create(&s_worker, NULL, nv12_record_worker, mm) != 0) {
        log_warn("nv12 record: worker thread create failed");
        s_recording      = 0;
        s_worker_running = 0;
        nv12_record_release_client(mm);
        fclose(s_tmp_fp);
        s_tmp_fp = NULL;
        return -1;
    }

    log_info("nv12 record: started stream=%d %ux%u@%ufps tmp=%s",
             preview_stream_idx, s_w, s_h, s_fps, s_tmp_path);
    return 0;
}

void tablet_nv12_record_stop(const char *mp4_path)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();

    if (!s_recording)
        return;

    s_recording = 0;
    nv12_record_stop_worker();
    nv12_record_release_client(mm);

    if (s_tmp_fp) {
        fflush(s_tmp_fp);
        fclose(s_tmp_fp);
        s_tmp_fp = NULL;
    }

    log_info("nv12 record: stopped (%u frames, %u write_fails)",
             s_frames_written, s_write_fail_count);

    if (mp4_path && s_frames_written > 0)
        (void)nv12_append_to_mp4(mp4_path);

    unlink(s_tmp_path);
    s_tmp_path[0] = '\0';
    s_frames_written   = 0;
    s_write_fail_count = 0;
    s_w   = 0;
    s_h   = 0;
    s_fps = 0;
}

int tablet_nv12_record_is_active(void)
{
    return s_recording;
}
