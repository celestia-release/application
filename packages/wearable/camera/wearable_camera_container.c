// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#define LOG_TAG "WEARABLE_CAMERA_CONTAINER"

#include "wearable_camera_container.h"

#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    FILE *fp;
    char path[360];
    char high_tmp[400];
    uint32_t preview_w;
    uint32_t preview_h;
    uint32_t preview_size;
    int active;
} lcm_video_writer_t;

static lcm_video_writer_t s_vw;

static void stamp_lcm_name(char *buf, size_t buf_size, const char *dir, const char *prefix)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (!buf || buf_size == 0)
        return;

    if (!tm) {
        snprintf(buf, buf_size, "%s/%s%s", dir ? dir : "", prefix ? prefix : "", WEARABLE_PHOTO_EXT);
        return;
    }

    snprintf(buf, buf_size, "%s/%s_%04d%02d%02d_%02d%02d%02d%s",
             dir ? dir : "", prefix ? prefix : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, WEARABLE_PHOTO_EXT);
}

size_t wearable_lcm_preview_frame_bytes(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * 3u / 2u;
}

void wearable_lcm_photo_path(const char *dir, char *out_path, size_t out_len)
{
    stamp_lcm_name(out_path, out_len, dir, WEARABLE_PHOTO_PREFIX);
}

void wearable_lcm_video_path(const char *dir, char *out_path, size_t out_len)
{
    stamp_lcm_name(out_path, out_len, dir, WEARABLE_VIDEO_PREFIX);
}

static int write_header(FILE *fp, const wearable_lcm_hdr_t *hdr)
{
    return (fwrite(hdr, 1, sizeof(*hdr), fp) == sizeof(*hdr)) ? 0 : -1;
}

static void init_header(wearable_lcm_hdr_t *hdr, uint16_t type,
                        uint32_t preview_w, uint32_t preview_h)
{
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, WEARABLE_LCM_MAGIC, 4);
    hdr->version = WEARABLE_LCM_VERSION;
    hdr->type = type;
    hdr->preview_w = preview_w;
    hdr->preview_h = preview_h;
    hdr->preview_offset = (uint32_t)sizeof(wearable_lcm_hdr_t);
}

int wearable_lcm_photo_write(const char *path, uint32_t preview_w, uint32_t preview_h,
                         const void *preview_nv12, size_t preview_len,
                         const void *jpeg_data, size_t jpeg_len)
{
    FILE *fp;
    wearable_lcm_hdr_t hdr;

    if (!path || !preview_nv12 || !jpeg_data || preview_w == 0 || preview_h == 0)
        return -1;

    if (preview_len < wearable_lcm_preview_frame_bytes(preview_w, preview_h) || jpeg_len == 0)
        return -1;

    fp = fopen(path, "wb");
    if (!fp)
        return -1;

    init_header(&hdr, WEARABLE_LCM_TYPE_PHOTO, preview_w, preview_h);
    hdr.preview_size = (uint32_t)wearable_lcm_preview_frame_bytes(preview_w, preview_h);
    hdr.high_offset = hdr.preview_offset + hdr.preview_size;
    hdr.high_size = (uint32_t)jpeg_len;
    snprintf(hdr.high_format, sizeof(hdr.high_format), "jpeg");

    if (write_header(fp, &hdr) != 0 ||
        fwrite(preview_nv12, 1, hdr.preview_size, fp) != hdr.preview_size ||
        fwrite(jpeg_data, 1, jpeg_len, fp) != jpeg_len) {
        fclose(fp);
        unlink(path);
        return -1;
    }

    fclose(fp);
    log_info("lcm photo saved: %s preview=%ux%u high=%u bytes", path, preview_w, preview_h,
             hdr.high_size);
    return 0;
}

int wearable_lcm_video_begin(const char *path, uint32_t preview_w, uint32_t preview_h)
{
    wearable_lcm_hdr_t hdr;

    if (s_vw.active)
        return 0;

    if (!path || preview_w == 0 || preview_h == 0)
        return -1;

    memset(&s_vw, 0, sizeof(s_vw));
    snprintf(s_vw.path, sizeof(s_vw.path), "%s", path);
    snprintf(s_vw.high_tmp, sizeof(s_vw.high_tmp), "%s.high.tmp.mp4", path);
    s_vw.preview_w = preview_w;
    s_vw.preview_h = preview_h;

    s_vw.fp = fopen(path, "wb");
    if (!s_vw.fp)
        return -1;

    init_header(&hdr, WEARABLE_LCM_TYPE_VIDEO, preview_w, preview_h);
    if (write_header(s_vw.fp, &hdr) != 0) {
        fclose(s_vw.fp);
        s_vw.fp = NULL;
        unlink(path);
        return -1;
    }

    s_vw.active = 1;
    return 0;
}

int wearable_lcm_video_append_preview(const void *data, size_t len)
{
    size_t frame_bytes;

    if (!s_vw.active || !s_vw.fp || !data)
        return -1;

    frame_bytes = wearable_lcm_preview_frame_bytes(s_vw.preview_w, s_vw.preview_h);
    if (len < frame_bytes)
        return -1;

    if (fwrite(data, 1, frame_bytes, s_vw.fp) != frame_bytes)
        return -1;

    s_vw.preview_size += (uint32_t)frame_bytes;
    return 0;
}

static int append_file(FILE *dst, const char *src_path, uint32_t *out_size)
{
    FILE *src;
    char buf[8192];
    size_t n;
    uint32_t total = 0;

    src = fopen(src_path, "rb");
    if (!src)
        return -1;

    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            return -1;
        }
        total += (uint32_t)n;
    }

    fclose(src);
    if (out_size)
        *out_size = total;
    return 0;
}

int wearable_lcm_video_finish(const char *high_mp4_path)
{
    wearable_lcm_hdr_t hdr;
    uint32_t high_size = 0;
    const char *mp4 = high_mp4_path ? high_mp4_path : s_vw.high_tmp;

    if (!s_vw.active || !s_vw.fp)
        return -1;

    if (append_file(s_vw.fp, mp4, &high_size) != 0) {
        wearable_lcm_video_abort();
        return -1;
    }

    init_header(&hdr, WEARABLE_LCM_TYPE_VIDEO, s_vw.preview_w, s_vw.preview_h);
    hdr.preview_size = s_vw.preview_size;
    hdr.high_offset = hdr.preview_offset + hdr.preview_size;
    hdr.high_size = high_size;
    snprintf(hdr.high_format, sizeof(hdr.high_format), "mp4");

    fseek(s_vw.fp, 0, SEEK_SET);
    if (write_header(s_vw.fp, &hdr) != 0) {
        fclose(s_vw.fp);
        s_vw.fp = NULL;
        s_vw.active = 0;
        return -1;
    }

    fclose(s_vw.fp);
    s_vw.fp = NULL;
    s_vw.active = 0;

    if (mp4 && mp4[0])
        unlink(mp4);

    log_info("lcm video saved: %s preview=%u bytes high=%u bytes", s_vw.path,
             hdr.preview_size, hdr.high_size);
    return 0;
}

void wearable_lcm_video_abort(void)
{
    if (s_vw.fp) {
        fclose(s_vw.fp);
        s_vw.fp = NULL;
    }
    if (s_vw.path[0])
        unlink(s_vw.path);
    if (s_vw.high_tmp[0])
        unlink(s_vw.high_tmp);
    memset(&s_vw, 0, sizeof(s_vw));
}

const char *wearable_lcm_video_high_tmp_path(void)
{
    return s_vw.active ? s_vw.high_tmp : NULL;
}

int wearable_lcm_read_header(const char *path, wearable_lcm_hdr_t *hdr)
{
    FILE *fp;

    if (!path || !hdr)
        return -1;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fread(hdr, 1, sizeof(*hdr), fp) != sizeof(*hdr)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (memcmp(hdr->magic, WEARABLE_LCM_MAGIC, 4) != 0 || hdr->version != WEARABLE_LCM_VERSION)
        return -1;

    return 0;
}

int wearable_lcm_read_photo_preview(const char *path, void *buf, size_t buf_size,
                                uint32_t *width, uint32_t *height)
{
    FILE *fp;
    wearable_lcm_hdr_t hdr;
    size_t need;

    if (!path || !buf || !width || !height)
        return -1;

    if (wearable_lcm_read_header(path, &hdr) != 0 || hdr.type != WEARABLE_LCM_TYPE_PHOTO)
        return -1;

    need = wearable_lcm_preview_frame_bytes(hdr.preview_w, hdr.preview_h);
    if (buf_size < need || hdr.preview_size < need)
        return -1;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    fseek(fp, (long)hdr.preview_offset, SEEK_SET);
    if (fread(buf, 1, need, fp) != need) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *width = hdr.preview_w;
    *height = hdr.preview_h;
    return 0;
}

FILE *wearable_lcm_open_video_preview(const char *path, wearable_lcm_hdr_t *hdr)
{
    FILE *fp;

    if (!path || !hdr)
        return NULL;

    if (wearable_lcm_read_header(path, hdr) != 0 || hdr->type != WEARABLE_LCM_TYPE_VIDEO)
        return NULL;

    fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (fseek(fp, (long)hdr->preview_offset, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    return fp;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl, su;

    if (!s || !suffix)
        return 0;
    sl = strlen(s);
    su = strlen(suffix);
    if (sl < su)
        return 0;
    s += (sl - su);
    for (; *suffix; s++, suffix++) {
        if ((*s | 0x20) != (*suffix | 0x20))
            return 0;
    }
    return 1;
}

int wearable_lcm_is_media_name(const char *name)
{
    if (!name)
        return 0;

    if (!ends_with_ci(name, WEARABLE_PHOTO_EXT))
        return 0;

    return (strncmp(name, WEARABLE_PHOTO_PREFIX, strlen(WEARABLE_PHOTO_PREFIX)) == 0 ||
            strncmp(name, WEARABLE_VIDEO_PREFIX, strlen(WEARABLE_VIDEO_PREFIX)) == 0);
}
