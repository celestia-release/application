// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_VOLC_H
#define APPLICATION_TABLET_VOLC_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_result_t tablet_volc_create(lv_obj_t *main);
int tablet_volc_init(void);

#ifdef __cplusplus
}
#endif

#endif
