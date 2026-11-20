// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * RTSP server start/stop for headless ipcamera.
 * Unlike wearable dashcam (vs1-only live), ipcamera allows all vsN so clients
 * can use the usual rtsp://IP:554/vs0 (main) URL printed at startup.
 */
#define LOG_TAG "IPCAMERA_RTSP"

#include "ipc_cam_rtsp.h"

#include "log.h"
#include "rtsp_protocol.h"

static rtsp_protocol_t *s_rtsp;
static int s_running;

int ipc_cam_rtsp_start(void)
{
    int ret;

    if (s_running)
        return 0;

    if (rtsp_protocol_open(&s_rtsp) != 0 || s_rtsp == NULL) {
        log_warn("rtsp protocol open failed");
        s_rtsp = NULL;
        return -1;
    }

    /* Allow all channels (default live_channel_only=-1). Do not inherit dashcam's
     * vs1-only filter — that rejects DESCRIBE/PLAY on /vs0 which clients use. */
    if (rtsp_protocol_set_live_channel_only(s_rtsp, -1) != 0)
        log_warn("rtsp clear live channel filter failed; keep open default");

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
    log_info("rtsp server started");
    rtsp_protocol_log_suggested_urls();
    return 0;
}

void ipc_cam_rtsp_stop(void)
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

int ipc_cam_rtsp_is_running(void)
{
    return s_running;
}
