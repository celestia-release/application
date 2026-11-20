// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * RTSP server start/stop for headless ipcamera (vs0/vs1 allowed).
 */
#ifndef IPCAMERA_RTSP_H
#define IPCAMERA_RTSP_H

int ipc_cam_rtsp_start(void);
void ipc_cam_rtsp_stop(void);
int ipc_cam_rtsp_is_running(void);

#endif /* IPCAMERA_RTSP_H */
