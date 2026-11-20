// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_CAMERA_H
#define APPLICATION_TABLET_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"

#ifdef CONFIG_APP_TABLET_CAMERA
lv_result_t tablet_camera_create(lv_obj_t *main);
#endif

#ifdef __cplusplus
}
#endif

#endif
