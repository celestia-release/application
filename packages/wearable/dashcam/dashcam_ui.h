// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna dashcam UI: centered preview + RTSP/record OSD; physical keypad ENTER toggles record.
 */
#ifndef WEARABLE_DASHCAM_UI_H
#define WEARABLE_DASHCAM_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

int dashcam_ui_create(lv_obj_t *launcher_main);
void dashcam_ui_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_DASHCAM_UI_H */
