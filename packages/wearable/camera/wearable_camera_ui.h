// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna camera preview UI (photo/record overlay on gui_camera stream).
 */
#ifndef WEARABLE_CAMERA_UI_H
#define WEARABLE_CAMERA_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

int wearable_camera_ui_create(lv_obj_t *launcher_main);
void wearable_camera_ui_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_CAMERA_UI_H */
