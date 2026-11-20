// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/services/uvc.
 *
 * UVC server: init and run UVC thread (h264/h265/mjpeg). Requires WiFi driver closed when in use.
 */
#ifndef APPLICATION_UVC_SERVER_H
#define APPLICATION_UVC_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UVC server and run UVC thread.
 * @details Supports h264, h265, mjpeg. Need to close WiFi driver before use.
 * @return 0 on success, negative error code on failure.
 */
int app_uvc_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_UVC_SERVER_H */
