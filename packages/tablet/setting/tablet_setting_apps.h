// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Settings tab: enable/disable installed apps (installed_apps.json).
 * Migrated from gui/nova/setting/nova_setting_apps.h → packages/tablet.
 */
#ifndef APPLICATION_TABLET_SETTING_APPS_H
#define APPLICATION_TABLET_SETTING_APPS_H

#include "lvgl.h"

#include <stdint.h>

lv_result_t tablet_setting_apps_create(lv_obj_t *parent);

void tablet_setting_apps_set_tab_index(uint32_t idx);
uint32_t tablet_setting_apps_tab_index(void);

/* Sync registry with disk when entering/leaving the Apps tab or Settings. */
void tablet_setting_apps_on_enter(void);
void tablet_setting_apps_on_leave(void);

/* Invalidate UI pointers after Settings window is deleted. */
void tablet_setting_apps_reset_ui(void);

#endif /* APPLICATION_TABLET_SETTING_APPS_H */
