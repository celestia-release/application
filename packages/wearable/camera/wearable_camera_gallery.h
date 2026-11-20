// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * In-app photo/video gallery for Luna camera.
 */
#ifndef WEARABLE_CAMERA_GALLERY_H
#define WEARABLE_CAMERA_GALLERY_H

#include "lvgl.h"

typedef enum {
    WEARABLE_GALLERY_PHOTO = 0,
    WEARABLE_GALLERY_VIDEO = 1
} wearable_gallery_type_t;

typedef void (*wearable_gallery_close_cb_t)(void *user_data);

int wearable_camera_gallery_show(lv_obj_t *parent_hide, wearable_gallery_type_t initial_tab,
                             wearable_gallery_close_cb_t on_close, void *user_data);

void wearable_camera_gallery_close(void);

#endif /* WEARABLE_CAMERA_GALLERY_H */
