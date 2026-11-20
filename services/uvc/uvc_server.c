// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/services/uvc.
 *
 * UVC server: USB Video Class gadget server, frame capture, config and stream control.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "UVC_SERVER"

#include "system_config.h"
#include "app_main.h"
#include "log.h"
#include "multimedia.h"
#include "system_time.h"
#include "uvc_media_compat.h"
#include "uvc_server.h"
#include "uvc_video.h"

typedef struct {
    int main_width;
    int main_height;
    int sub_width;
    int sub_height;
    int video_code;         /* 0-h264, 1-h265 */
    int fps;
    int iHandleV;           /* video handle */
    int iHandleA;           /* audio handle */
    multimedia_t *multimedia;
    int iVideoType;         /* 0-main stream 1-sub stream */
    int iAudioType;
} uvc_device_info_t;

static uvc_device_info_t g_uvcDevice = {0};

/* Save frame data to file (for debug). */
int uvc_save_frame_to_file(fw_frame_info *pframe, const char *prefix)
{
    if (!pframe || !pframe->pData || pframe->uiDataLen <= 0) {
        log_info("Invalid frame data");
        return -1;
    }
    char filename[256];
    long long ts = system_get_sys_run_time64();
    snprintf(filename, sizeof(filename), "/mnt/%lld_%dx%d.h264",
             prefix ? prefix : "frame", ts, pframe->iWidth, pframe->iHeight);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        log_info("Cannot create file: %s", filename);
        return -1;
    }
    size_t written = fwrite(pframe->pData, 1, pframe->uiDataLen, fp);
    if (written != pframe->uiDataLen) {
        log_info("Incomplete write: expected %u bytes, wrote %zu", (unsigned)pframe->uiDataLen, written);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    log_info("Frame saved: %s (size=%u, %dx%d, %s)",
                 filename, pframe->uiDataLen, pframe->iWidth, pframe->iHeight,
                 pframe->frame_type == 1 ? "I" : "P");
    return 0;
}

int frame_count = 0;
int app_uvc_get_frame_cb(int nHandle, fw_frame_info *pframe, void* pDataCbUser)
{
    if (g_uvcDevice.iHandleV < 0 || !g_uvcDevice.multimedia || !g_uvcDevice.multimedia->camera)
        return -1;
    media_frame_info_t mf = {0};
    int ret = g_uvcDevice.multimedia->camera->stream_get_frame(g_uvcDevice.iHandleV, &mf);
    if (ret || mf.data_len <= 0)
        return -1;
    pframe->code_type = mf.code_type;
    pframe->frame_type = mf.frame_type;
    pframe->uiDataLen = mf.data_len;
    pframe->seq_no = mf.seq_no;
    pframe->u64Time = mf.timestamp;
    pframe->iWidth = mf.width;
    pframe->iHeight = mf.height;
    pframe->pData = mf.data;
    return 0;
}

int app_uvc_set_resolution_cb(int nHandle, int width, int height)
{
    log_warn("set resolution handle:%d, width:%d, height:%d", nHandle, width, height);
    if (!g_uvcDevice.multimedia || !g_uvcDevice.multimedia->camera) return -1;
    media_video_enc_param_t mvenc = {0};
    g_uvcDevice.multimedia->camera->stream_get_enc_param(0, &mvenc);
    mvenc.width = width;
    mvenc.height = height;
    g_uvcDevice.multimedia->camera->stream_set_enc_param(0, &mvenc);
    return 0;
}

/* Minimal INI read for [Video] (no aas_ini_parser). Single-pass. */
static int uvc_info_config_read(char *path)
{
    char szFilePath[256] = {0};
    if (path == NULL) {
        if (config_build_file_path("app_uvc_config.ini", szFilePath, sizeof(szFilePath)) != 0) {
            return -1;
        }
    } else {
        snprintf(szFilePath, sizeof(szFilePath), "%sapp_uvc_config.ini", path);
    }
    FILE *fp = fopen(szFilePath, "r");
    if (!fp) {
        log_error("cannot open file: %s", szFilePath);
        return -1;
    }
    g_uvcDevice.main_width = 2560;
    g_uvcDevice.main_height = 1440;
    g_uvcDevice.video_code = 0;
    g_uvcDevice.fps = 30;
    int in_video = 0;
    char line[256];
    while (fgets(line, (int)sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (line[0] == '[') {
            in_video = (strncmp(line + 1, "Video]", 6) == 0) ? 1 : 0;
            continue;
        }
        if (!in_video) continue;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v = '\0';
        v++;
        while (*v == ' ' || *v == '\t') v++;
        if (strcmp(line, "main_width") == 0) g_uvcDevice.main_width = atoi(v);
        else if (strcmp(line, "main_height") == 0) g_uvcDevice.main_height = atoi(v);
        else if (strcmp(line, "video_code") == 0) g_uvcDevice.video_code = atoi(v);
        else if (strcmp(line, "fps") == 0) g_uvcDevice.fps = atoi(v);
    }
    fclose(fp);
    log_info("Video Config - Width:%d Height:%d VideoCode:%d FPS:%d",
             g_uvcDevice.main_width, g_uvcDevice.main_height, g_uvcDevice.video_code, g_uvcDevice.fps);
    return 0;
}


int app_uvc_init()
{
    log_warn("get venc param");
    multimedia_t *m = (multimedia_t *)app_main_get_multimedia();
    g_uvcDevice.multimedia = m;
    if (!m || !m->camera) return -1;

    media_video_enc_param_t mvenc = {0};
    m->camera->stream_get_enc_param(0, &mvenc);
    fw_video_enc_param venc = {0};
    venc.width = mvenc.width;
    venc.height = mvenc.height;
    venc.framerate = mvenc.framerate;
    if (mvenc.codec == MEDIA_CODEC_VIDEO_H264) venc.code_type = UVC_CODEC_TYPE_H264;
    else if (mvenc.codec == MEDIA_CODEC_VIDEO_H265) venc.code_type = UVC_CODEC_TYPE_H265;
    else venc.code_type = UVC_CODEC_TYPE_MJPEG;

    char codec_type[16] = {0};
    char cmd[512] = {0};
    switch(venc.code_type) {
        case 0: sprintf(codec_type, "%s", "h264"); snprintf(cmd, sizeof(cmd), "/usr/sbin/uvc.sh framebased %s %d %d %d", codec_type, venc.width, venc.height, venc.framerate); break;
        case 1: sprintf(codec_type, "%s", "mjpeg"); snprintf(cmd, sizeof(cmd), "/usr/sbin/uvc.sh mjpeg m %d %d %d", venc.width, venc.height, venc.framerate); break;
        case 2: sprintf(codec_type, "%s", "h265"); snprintf(cmd, sizeof(cmd), "/usr/sbin/uvc.sh framebased %s %d %d %d", codec_type, venc.width, venc.height, venc.framerate); break;
        default: sprintf(codec_type, "%s", "h264"); snprintf(cmd, sizeof(cmd), "/usr/sbin/uvc.sh framebased %s %d %d %d", codec_type, venc.width, venc.height, venc.framerate);
    }
    system(cmd);
    printf("%s_%d cmd=%s \n", __FUNCTION__, __LINE__, cmd);

    g_uvcDevice.iHandleV = 0;
    g_uvcDevice.iHandleA = 0;
    g_uvcDevice.iVideoType = 0;
    g_uvcDevice.iAudioType = -1;
    if (g_uvcDevice.iVideoType >= 0) {
        m->camera->stream_reg_client(g_uvcDevice.iVideoType, &g_uvcDevice.iHandleV, NULL, NULL);
        log_warn("reg client video ihandleV:%d", g_uvcDevice.iHandleV);
    }
    if (g_uvcDevice.iAudioType >= 0 && m->audio) {
        m->audio->stream_reg_client(g_uvcDevice.iAudioType, &g_uvcDevice.iHandleA, NULL, NULL);
        log_warn("reg client audio");
    }
    video_uvc_init(&venc, app_uvc_get_frame_cb, app_uvc_set_resolution_cb);
    return 0;
}



