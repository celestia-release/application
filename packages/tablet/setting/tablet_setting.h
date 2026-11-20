// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_SETTING_H
#define APPLICATION_TABLET_SETTING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"

/* Dark theme shared across all setting UI modules.
 * Tuned to the launcher (tablet_home.c): blue-black wallpaper ink instead of
 * pure grey, launcher ink/muted text, screen-cam blue accent, iOS status
 * colors. Cards use the launcher frosted recipe: lifted fill + white hairline
 * border (SETTING_CARD_BORDER_OPA) + soft shadow. */
#define SETTING_PAGE_BG     0x12141A  /* launcher wallpaper ink */
#define SETTING_CONTENT_BG  0x161A22  /* right pane, slightly lifted */
#define SETTING_SIDEBAR_BG  0x0E1016  /* left rail, darker than page */
#define SETTING_SIDEBAR_SEL 0x272B33  /* launcher icon-tile neutral */
#define SETTING_TEXT        0xF4F7FB  /* launcher ink */
#define SETTING_TEXT_SEC    0xC5D0DC  /* launcher muted */
#define SETTING_TEXT_DIM    0x8E9EAE  /* launcher wifi-off grey */
#define SETTING_ACCENT      0x5AC8FA  /* launcher screen-cam blue (grad top) */
#define SETTING_ACCENT_DEEP 0x2F7CF6  /* icon gradient bottom, for filled buttons */
#define SETTING_ACCENT_OK   0x34C759  /* iOS charging green (battery) */
#define SETTING_ERR         0xFF453A  /* toast error red */
#define SETTING_CARD_BG     0x1D222C  /* card fill */
#define SETTING_CARD_ITEM   0x272B33  /* nested row / input fill */
#define SETTING_BORDER      0x39404E  /* dark hairline for inputs/rows */
#define SETTING_CARD_BORDER_OPA ((lv_opa_t)38) /* white hairline on cards (launcher) */
#define SETTING_HIGHLIGHT_OPA   ((lv_opa_t)30) /* frosted white highlight */

#ifdef CONFIG_APP_TABLET_SETTING
void tablet_setting_flat_container(lv_obj_t *obj);
lv_result_t tablet_setting_create(lv_obj_t *main);
void tablet_setting_hide_for_restart(void);
#endif

#ifdef __cplusplus
}
#endif

#endif

