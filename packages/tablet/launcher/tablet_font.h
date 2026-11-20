// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#ifndef APPLICATION_TABLET_FONT_H
#define APPLICATION_TABLET_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

lv_result_t tablet_font_init(void);
/** Load full Chinese.ttf after Home first frame (Scheme B). */
lv_result_t tablet_font_ensure_full(void);
lv_result_t tablet_font_release(void);
lv_font_t *tablet_font_get_freetype_font(uint16_t weight);
/** Same TTF rendered bold (FreeType synthetic embolden); NULL before full font ready. */
lv_font_t *tablet_font_get_freetype_bold_font(uint16_t weight);
lv_font_t *tablet_font_get_montserrat_font(uint16_t weight);

#ifdef __cplusplus
}
#endif

#endif
