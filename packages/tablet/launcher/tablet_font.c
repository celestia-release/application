// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Nova font helpers — Scheme B: Home uses compiled bitmap fonts (no TTF I/O);
 * full Chinese.ttf is loaded lazily after Home / on demand.
 */
#define LOG_TAG "TABLET_FONT"

#include "tablet_font.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_globle_config.h"
#include "log.h"

#define LV_FONT_MIN_WEIGHT              12
/* Pointer-table guard only (weight is the index); 96 covers the screen-camera
 * hero clock. Raising it adds table slots, no runtime cost for old sizes. */
#define LV_FONT_MAX_WEIGHT              97

#ifndef FONT_FILE_PATH_INSTALL
#define FONT_FILE_PATH_INSTALL          "/mnt/install/gui/font/Chinese.ttf"
#endif

extern const lv_font_t lv_font_home_20;
extern const lv_font_t lv_font_home_28;

#if LV_USE_FREETYPE
#include "lv_freetype.h"

static lv_font_t *font[LV_FONT_MAX_WEIGHT] = {0};
static lv_font_t *font_bold[LV_FONT_MAX_WEIGHT] = {0};
static char s_font_lv_path[256];
static int s_font_path_ok;
static int s_full_font_ready;
static int s_ft_inited;
#endif

#if LV_USE_FREETYPE
static const char *tablet_font_resolve_path(void)
{
    if (access(FONT_FILE_PATH_INSTALL, F_OK) == 0)
        return FONT_FILE_PATH_INSTALL;
#ifdef FONT_FILE_PATH_AFHT
    if (access(FONT_FILE_PATH_AFHT, F_OK) == 0)
        return FONT_FILE_PATH_AFHT;
#endif
    if (access(FONT_FILE_PATH, F_OK) == 0)
        return FONT_FILE_PATH;
    return NULL;
}

static int tablet_font_ensure_ft_runtime(void)
{
    if (s_ft_inited)
        return 0;
    if (lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT) != LV_RESULT_OK) {
        log_error("lv_freetype_init failed");
        return -1;
    }
    s_ft_inited = 1;
    log_info("lv_freetype_init done (deferred after Home)");
    return 0;
}

static int tablet_font_prepare_full_path(void)
{
    const char *font_path;

    if (s_font_path_ok)
        return 0;

    font_path = tablet_font_resolve_path();
    if (!font_path) {
        log_error("failed to access full font file");
        return -1;
    }

#if LV_FREETYPE_USE_LVGL_PORT
    snprintf(s_font_lv_path, sizeof(s_font_lv_path), "%c:%s", LV_FS_STDIO_LETTER, font_path);
#else
    snprintf(s_font_lv_path, sizeof(s_font_lv_path), "%s", font_path);
#endif
    s_font_path_ok = 1;
    log_info("full font path ready: %s", s_font_lv_path);
    return 0;
}

static lv_font_t *tablet_font_create_size_style(uint16_t weight, uint32_t style,
                                                lv_font_t **table)
{
    if (weight < LV_FONT_MIN_WEIGHT || weight >= LV_FONT_MAX_WEIGHT)
        return NULL;
    if (table[weight])
        return table[weight];
    if (tablet_font_ensure_ft_runtime() != 0)
        return NULL;
    if (tablet_font_prepare_full_path() != 0)
        return NULL;

    table[weight] = lv_freetype_font_create(s_font_lv_path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                            weight, style);
    if (!table[weight])
        log_error("failed to create font size %u style %u", (unsigned)weight, (unsigned)style);
    else
        s_full_font_ready = 1;
    return table[weight];
}

static lv_font_t *tablet_font_create_size(uint16_t weight)
{
    return tablet_font_create_size_style(weight, LV_FREETYPE_FONT_STYLE_NORMAL, font);
}
#endif

lv_result_t tablet_font_init(void)
{
    /* Scheme B: Home critical path uses lv_font_home_* only — no Chinese.ttf open. */
    log_info("home bitmap fonts ready (lv_font_home_20/28); TTF deferred");
    return LV_RESULT_OK;
}

lv_result_t tablet_font_ensure_full(void)
{
#if LV_USE_FREETYPE
    /* Warm common non-home sizes used by settings/call pages. */
    static const uint16_t kWarm[] = {14, 16, 18, 20, 22, 30, 40, 96};
    int i;

    if (tablet_font_ensure_ft_runtime() != 0)
        return LV_RESULT_INVALID;
    if (tablet_font_prepare_full_path() != 0)
        return LV_RESULT_INVALID;

    for (i = 0; i < (int)(sizeof(kWarm) / sizeof(kWarm[0])); i++) {
        if (!tablet_font_create_size(kWarm[i]))
            return LV_RESULT_INVALID;
    }
    log_info("full Chinese.ttf fonts warmed");
    return LV_RESULT_OK;
#else
    return LV_RESULT_OK;
#endif
}

lv_result_t tablet_font_release(void)
{
#if LV_USE_FREETYPE
    int i;

    for (i = 1; i < LV_FONT_MAX_WEIGHT; i++) {
        if (font[i]) {
            lv_freetype_font_delete(font[i]);
            font[i] = NULL;
        }
        if (font_bold[i]) {
            lv_freetype_font_delete(font_bold[i]);
            font_bold[i] = NULL;
        }
    }
    s_font_path_ok = 0;
    s_full_font_ready = 0;
    s_font_lv_path[0] = '\0';
    if (s_ft_inited) {
        lv_freetype_uninit();
        s_ft_inited = 0;
    }
#endif
    return LV_RESULT_OK;
}

lv_font_t *tablet_font_get_freetype_font(uint16_t weight)
{
#if LV_USE_FREETYPE
    /* Before Chinese.ttf is warm: Home bitmap only (no TTF I/O on critical path). */
    if (!s_full_font_ready) {
        if (weight == 20)
            return (lv_font_t *)&lv_font_home_20;
        /* 28 = date/menu; 30 = weather (GUI_SCALE) — share home_28 until TTF ready. */
        if (weight == 28 || weight == 30)
            return (lv_font_t *)&lv_font_home_28;
    }

    if ((LV_FONT_MIN_WEIGHT > weight) || (LV_FONT_MAX_WEIGHT < weight)) {
        LV_LOG_ERROR("not support this weight of font\n");
        return NULL;
    }
    return tablet_font_create_size(weight);
#else
    if (weight == 20)
        return (lv_font_t *)&lv_font_home_20;
    if (weight == 28 || weight == 30)
        return (lv_font_t *)&lv_font_home_28;
    LV_LOG_ERROR("not support freetype font, because LV_USE_FREETYPE isn't set\n");
    return NULL;
#endif
}

lv_font_t *tablet_font_get_freetype_bold_font(uint16_t weight)
{
#if LV_USE_FREETYPE
    /* Bold is only served after the full TTF is warm (no bold home bitmap). */
    if (!s_full_font_ready)
        return NULL;
    if ((LV_FONT_MIN_WEIGHT > weight) || (LV_FONT_MAX_WEIGHT < weight)) {
        LV_LOG_ERROR("not support this weight of bold font\n");
        return NULL;
    }
    return tablet_font_create_size_style(weight, LV_FREETYPE_FONT_STYLE_BOLD, font_bold);
#else
    (void)weight;
    return NULL;
#endif
}

lv_font_t *tablet_font_get_montserrat_font(uint16_t weight)
{
    /*
     * Nova lv_conf enables only 28 + 48. Snap requested size to the nearer one.
     * Do not reference other lv_font_montserrat_* symbols — they are not linked.
     */
    uint16_t w = weight;

    if (w % 2)
        w = (uint16_t)((w + 1) & (uint16_t)~1);
    if (w < 8)
        w = 8;
    if (w > 48)
        w = 48;

#if LV_FONT_MONTSERRAT_28 && LV_FONT_MONTSERRAT_48
    if (w >= 38)
        return (lv_font_t *)&lv_font_montserrat_48;
    return (lv_font_t *)&lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_48
    return (lv_font_t *)&lv_font_montserrat_48;
#elif LV_FONT_MONTSERRAT_28
    return (lv_font_t *)&lv_font_montserrat_28;
#else
    (void)w;
    return (lv_font_t *)LV_FONT_DEFAULT;
#endif
}
