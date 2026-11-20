// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * RTSP server start/stop wrapper for Luna dashcam (sub stream only on vs1;
 * vs0/main is rejected for any RTSP client while dashcam owns the server).
 */
#ifndef WEARABLE_DASHCAM_RTSP_H
#define WEARABLE_DASHCAM_RTSP_H

int dashcam_rtsp_start(void);
void dashcam_rtsp_stop(void);
int dashcam_rtsp_is_running(void);

#endif /* WEARABLE_DASHCAM_RTSP_H */
