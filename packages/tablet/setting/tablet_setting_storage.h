// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Settings: local SD mount vs USB mass-storage mode (tablet_lite ActionCam).
 */
#ifndef TABLET_SETTING_STORAGE_H
#define TABLET_SETTING_STORAGE_H

#include "lvgl.h"

#include <stdint.h>

lv_result_t tablet_setting_storage_create(lv_obj_t *parent);

void tablet_setting_storage_set_tab_index(uint32_t idx);
uint32_t tablet_setting_storage_tab_index(void);

/* True while Settings > Storage USB session is active (SD exported to PC). */
int tablet_setting_storage_is_usb_active(void);

/* Leaving storage tab/settings while USB session active auto-restores local mode.
 * Dropdown switch to local also skips confirm; USB MSC still uses one confirm. */
int tablet_setting_storage_try_leave_to_tab(uint32_t current_tab_idx, uint32_t new_tab_idx);
int tablet_setting_storage_try_leave_settings(void);

#endif /* TABLET_SETTING_STORAGE_H */
