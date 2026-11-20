// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_INFO_H
#define APPLICATION_TABLET_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tablet_common.h"

#ifdef CONFIG_APP_TABLET_SETTING
lv_result_t tablet_info_create(lv_obj_t *main);
void tablet_info_embed_in_parent(lv_obj_t *parent);
#endif

#ifdef __cplusplus
}
#endif

#endif
