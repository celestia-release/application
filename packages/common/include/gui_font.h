// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/common.
 *
 * Shared LVGL font helpers for GUI applications.
 */
#ifndef APPLICATION_GUI_FONT_H
#define APPLICATION_GUI_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief Initialize font subsystem.
 * @return LV_RESULT_OK on success.
 */
lv_result_t gui_font_init(void);

/**
 * @brief Release font subsystem.
 * @return LV_RESULT_OK on success.
 */
lv_result_t gui_font_release(void);

/**
 * @brief Get FreeType font by weight.
 * @param weight Font weight.
 * @return Font pointer, or NULL.
 */
lv_font_t *gui_font_get_freetype_font(uint16_t weight);

/**
 * @brief Get FreeType font for CJK labels (Nova setting / weather pattern).
 * Tries @p weight first, then common sizes (30/22/20/18/16/14).
 * @param weight Preferred font weight.
 * @return Font pointer, or NULL if gui_font_init did not load a font file.
 */
lv_font_t *gui_font_get_cjk_font(uint16_t weight);

/**
 * @brief Get Montserrat font by weight.
 * @param weight Font weight.
 * @return Font pointer, or NULL.
 */
lv_font_t *gui_font_get_montserrat_font(uint16_t weight);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APPLICATION_GUI_FONT_H */
