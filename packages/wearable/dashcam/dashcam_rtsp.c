// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * RTSP server start/stop wrapper for Luna dashcam (sub/vs1 only).
 */
#define LOG_TAG "WEARABLE_DASHCAM_RTSP"

#include "dashcam_rtsp.h"

#include "log.h"
#include "rtsp_protocol.h"

/* Dashcam live URL is always vs1 (sub). Main/vs0 stays record-only. */
#define DASHCAM_RTSP_LIVE_CHANNEL  1

static rtsp_protocol_t *s_rtsp;
static int s_running;

int dashcam_rtsp_start(void)
{
    int ret;

    if (s_running)
        return 0;

    if (rtsp_protocol_open(&s_rtsp) != 0 || s_rtsp == NULL) {
        log_warn("rtsp protocol open failed");
        s_rtsp = NULL;
        return -1;
    }

    /* Dashcam live view is sub-only: reject vs0 (main/record) on DESCRIBE/PLAY. */
    if (rtsp_protocol_set_live_channel_only(s_rtsp, DASHCAM_RTSP_LIVE_CHANNEL) != 0) {
        log_warn("rtsp set live channel filter failed");
        (void)rtsp_protocol_close(s_rtsp);
        s_rtsp = NULL;
        return -1;
    }

    if (s_rtsp->ops == NULL || s_rtsp->ops->server == NULL || s_rtsp->ops->server->start == NULL) {
        log_warn("rtsp server ops unavailable");
        (void)rtsp_protocol_close(s_rtsp);
        s_rtsp = NULL;
        return -1;
    }

    ret = s_rtsp->ops->server->start((rtsp_protocol_context_t *)s_rtsp);
    if (ret != 0) {
        log_warn("rtsp server start failed (ret=%d)", ret);
        (void)rtsp_protocol_close(s_rtsp);
        s_rtsp = NULL;
        return -1;
    }

    s_running = 1;
    /* RTSP vs1 maps to role=sub (see rtsp_resolve_video_stream_id). */
    log_info("rtsp server started (live=vs1 sub only; vs0 rejected)");
    return 0;
}

void dashcam_rtsp_stop(void)
{
    if (!s_running)
        return;

    if (s_rtsp != NULL) {
        /* rtsp_protocol_close() stops the server internally; do not call stop() twice. */
        (void)rtsp_protocol_close(s_rtsp);
        s_rtsp = NULL;
    }

    s_running = 0;
    log_info("rtsp server stopped");
}

int dashcam_rtsp_is_running(void)
{
    return s_running;
}
