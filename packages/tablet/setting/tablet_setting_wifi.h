// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_SETTING_WIFI_H
#define APPLICATION_TABLET_SETTING_WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"

lv_result_t tablet_setting_wifi_create(lv_obj_t *parent);
lv_result_t tablet_setting_wifi_close(void);

/**
 * Persist STA credentials to /etc/config/settings.json wifi{ssid,psk,auto_connect}.
 * Used by Settings WiFi list and QR pairing after a successful associate.
 * @return 0 on success, <0 on failure
 */
int tablet_setting_wifi_save_sta(const char *ssid, const char *psk);

#ifdef __cplusplus
}
#endif

#endif
