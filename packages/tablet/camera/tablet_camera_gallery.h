// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * In-app photo/video gallery for Nova camera.
 */
#ifndef APPLICATION_TABLET_CAMERA_GALLERY_H
#define APPLICATION_TABLET_CAMERA_GALLERY_H

#include "lvgl.h"

typedef enum {
    TABLET_GALLERY_PHOTO = 0,
    TABLET_GALLERY_VIDEO = 1
} tablet_gallery_type_t;

typedef void (*tablet_gallery_close_cb_t)(void *user_data);

int tablet_camera_gallery_show(lv_obj_t *parent_hide, tablet_gallery_type_t initial_tab,
                               tablet_gallery_close_cb_t on_close, void *user_data);

void tablet_camera_gallery_close(void);

#endif /* APPLICATION_TABLET_CAMERA_GALLERY_H */
