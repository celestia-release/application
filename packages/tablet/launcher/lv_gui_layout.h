// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Shared GUI layout scaling for nova (anypi_nova 1024x600, anypi_nova_lite 800x480).
 * Pixel values are tuned on 800x480; scale horizontally/vertically from LCD_*_RES_MAX.
 */
#ifndef LV_GUI_LAYOUT_H
#define LV_GUI_LAYOUT_H

#include "app_globle_config.h"

#ifndef GUI_LAYOUT_REF_HOR
#define GUI_LAYOUT_REF_HOR  800
#endif
#ifndef GUI_LAYOUT_REF_VER
#define GUI_LAYOUT_REF_VER  480
#endif

#define GUI_SCALE(v)      ((int)(((long)(v) * LCD_HOR_RES_MAX + GUI_LAYOUT_REF_HOR / 2) / GUI_LAYOUT_REF_HOR))
#define GUI_SCALE_VER(v)  ((int)(((long)(v) * LCD_VER_RES_MAX + GUI_LAYOUT_REF_VER / 2) / GUI_LAYOUT_REF_VER))

#endif /* LV_GUI_LAYOUT_H */
