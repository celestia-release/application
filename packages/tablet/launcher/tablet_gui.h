// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_GUI_H
#define APPLICATION_TABLET_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

int app_lv_open(const char *cfg_path);
int app_lv_close(void);
void app_lv_stop_gui_loop(void);
void tablet_run(void);

#ifdef __cplusplus
}
#endif

#endif
