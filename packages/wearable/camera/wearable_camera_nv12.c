// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#define LOG_TAG "WEARABLE_CAMERA_NV12"

#include "wearable_camera_nv12.h"
#include "wearable_camera_container.h"
#include "wearable_camera_streams.h"

#include "app_main.h"
#include "gui_camera.h"
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
#define NV12_APPEND_FAIL_LOG_MAX    8u

static volatile int s_recording;
static pthread_t s_worker;
static volatile int s_worker_running;
static int s_client_handle = NV12_RECORD_CLIENT_INVALID;
static uint32_t s_lcm_w;
static uint32_t s_lcm_h;
static void *s_pack_buf;
static size_t s_pack_buf_size;
static unsigned int s_frames_written;
static unsigned int s_append_fail_count;

static int frame_is_nv12(const media_frame_info_t *frame)
{
    size_t need;

    if (!frame || !frame->data || frame->width <= 0 || frame->height <= 0)
        return 0;

    need = wearable_nv12_frame_size((uint32_t)frame->width, (uint32_t)frame->height);
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

    tight = wearable_nv12_frame_size(w, h);
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

    need = wearable_nv12_frame_size(out_w, out_h);
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

static void nv12_record_log_append_fail(int ret, uint32_t fw, uint32_t fh)
{
    s_append_fail_count++;
    if (s_append_fail_count <= NV12_APPEND_FAIL_LOG_MAX) {
        log_warn("nv12 record append failed ret=%d frame=%ux%u lcm=%ux%u total_fail=%u",
                 ret, fw, fh, s_lcm_w, s_lcm_h, s_append_fail_count);
    }
}

static int nv12_record_append_frame(const media_frame_info_t *frame)
{
    const void *payload;
    size_t payload_len;
    int ret;

    if (!frame_is_nv12(frame))
        return -1;

    if ((uint32_t)frame->width == s_lcm_w && (uint32_t)frame->height == s_lcm_h &&
        nv12_src_stride(frame) == s_lcm_w) {
        payload = frame->data;
        payload_len = wearable_nv12_frame_size(s_lcm_w, s_lcm_h);
    } else {
        if (nv12_pack_tight(frame, s_lcm_w, s_lcm_h, s_pack_buf, s_pack_buf_size) != 0)
            return -1;
        payload = s_pack_buf;
        payload_len = wearable_nv12_frame_size(s_lcm_w, s_lcm_h);
    }

    ret = wearable_lcm_video_append_preview(payload, payload_len);
    if (ret != 0) {
        nv12_record_log_append_fail(ret, (uint32_t)frame->width, (uint32_t)frame->height);
        return -1;
    }

    s_frames_written++;
    if (s_frames_written == 1u || (s_frames_written % 150u) == 0u) {
        log_info("nv12 record: %u preview frames written (%ux%u)",
                 s_frames_written, s_lcm_w, s_lcm_h);
    }
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
        log_warn("nv12 record: stream_reg_client(preview=%d) failed ret=%d", stream_idx, ret);
        s_client_handle = NV12_RECORD_CLIENT_INVALID;
        return -1;
    }

    log_info("nv12 record: preview client registered stream=%d handle=0x%x",
             stream_idx, s_client_handle);
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

        (void)nv12_record_append_frame(&frame);
    }

    return NULL;
}

size_t wearable_nv12_frame_size(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * 3u / 2u;
}

static int write_header(FILE *fp, uint32_t width, uint32_t height)
{
    wearable_nv12_file_hdr_t hdr;

    memcpy(hdr.magic, WEARABLE_NV12_MAGIC, 4);
    hdr.width = width;
    hdr.height = height;
    return (fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) ? 0 : -1;
}

int wearable_nv12_save_frame(const char *path, uint32_t width, uint32_t height,
                         const void *data, size_t data_len, int append)
{
    FILE *fp;
    size_t need;
    const char *mode;

    if (!path || !data || width == 0 || height == 0)
        return -1;

    need = wearable_nv12_frame_size(width, height);
    if (data_len < need)
        return -1;

    mode = append ? "ab" : "wb";
    fp = fopen(path, mode);
    if (!fp)
        return -1;

    if (!append && write_header(fp, width, height) != 0) {
        fclose(fp);
        unlink(path);
        return -1;
    }

    if (fwrite(data, 1, need, fp) != need) {
        fclose(fp);
        if (!append)
            unlink(path);
        return -1;
    }

    fclose(fp);
    return 0;
}

int wearable_nv12_load_single(const char *path, uint32_t *width, uint32_t *height,
                          void *buf, size_t buf_size)
{
    FILE *fp;
    wearable_nv12_file_hdr_t hdr;
    size_t need;

    if (!path || !width || !height || !buf)
        return -1;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return -1;
    }

    if (memcmp(hdr.magic, WEARABLE_NV12_MAGIC, 4) != 0) {
        fclose(fp);
        return -1;
    }

    need = wearable_nv12_frame_size(hdr.width, hdr.height);
    if (buf_size < need) {
        fclose(fp);
        return -1;
    }

    if (fread(buf, 1, need, fp) != need) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *width = hdr.width;
    *height = hdr.height;
    return 0;
}

int wearable_nv12_capture_preview_frame(multimedia_t *mm, int stream_idx,
                                    uint32_t width, uint32_t height,
                                    const char *path)
{
    void *buf;
    size_t need;
    int ret;

    (void)mm;
    (void)stream_idx;

    if (!path || width == 0 || height == 0)
        return -1;

    need = wearable_nv12_frame_size(width, height);
    buf = malloc(need);
    if (!buf)
        return -1;

    ret = wearable_nv12_capture_preview_buffer(mm, stream_idx, &width, &height, buf, need);
    if (ret != 0) {
        free(buf);
        return -1;
    }

    ret = wearable_nv12_save_frame(path, width, height, buf, need, 0);
    free(buf);
    return ret;
}

int wearable_nv12_capture_preview_buffer(multimedia_t *mm, int stream_idx,
                                     uint32_t *width, uint32_t *height,
                                     void *buf, size_t buf_size)
{
    (void)mm;
    (void)stream_idx;

    /* One-shot probe via the LCD preview client (no extra stream_reg_client). */
    return gui_camera_grab_nv12_frame(buf, buf_size, width, height);
}

static void nv12_record_stop_worker(void)
{
    if (!s_worker_running)
        return;

    s_worker_running = 0;
    pthread_join(s_worker, NULL);
}

int wearable_nv12_record_start(multimedia_t *mm, int stream_idx,
                           uint32_t width, uint32_t height,
                           const char *path)
{
    const gui_camera_streams_t *st;
    int preview_idx;
    size_t pack_need;

    (void)path;

    if (s_recording)
        return 0;

    if (!mm || !mm->camera || width == 0 || height == 0)
        return -1;

    (void)wearable_camera_streams_refresh();
    st = wearable_camera_streams_get();
    preview_idx = (st && st->idx_preview >= 0) ? st->idx_preview : stream_idx;
    if (preview_idx < 0) {
        log_warn("nv12 record: preview stream unavailable");
        return -1;
    }

    s_lcm_w = width;
    s_lcm_h = height;
    pack_need = wearable_nv12_frame_size(s_lcm_w, s_lcm_h);
    if (!s_pack_buf || s_pack_buf_size < pack_need) {
        void *nb = realloc(s_pack_buf, pack_need);

        if (!nb)
            return -1;
        s_pack_buf = nb;
        s_pack_buf_size = pack_need;
    }

    if (nv12_record_register_client(mm, preview_idx) != 0)
        return -1;

    s_frames_written = 0;
    s_append_fail_count = 0;
    s_worker_running = 1;
    s_recording = 1;

    if (pthread_create(&s_worker, NULL, nv12_record_worker, mm) != 0) {
        log_warn("nv12 record: worker thread create failed");
        s_recording = 0;
        s_worker_running = 0;
        nv12_record_release_client(mm);
        return -1;
    }

    log_info("nv12 record: started preview stream=%d lcm=%ux%u", preview_idx, s_lcm_w, s_lcm_h);
    return 0;
}

void wearable_nv12_record_stop(void)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();

    if (!s_recording)
        return;

    s_recording = 0;
    nv12_record_stop_worker();
    nv12_record_release_client(mm);

    log_info("nv12 record: stopped (%u frames written, append_fail=%u)",
             s_frames_written, s_append_fail_count);
    s_frames_written = 0;
    s_append_fail_count = 0;
    s_lcm_w = 0;
    s_lcm_h = 0;
}

int wearable_nv12_record_is_active(void)
{
    return s_recording;
}
