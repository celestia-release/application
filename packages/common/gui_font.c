// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/common.
 *
 * Shared LVGL font helpers implementation.
 */
#define LOG_TAG "GUI_FONT"

#include "gui_font.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "app_globle_config.h"
#include "log.h"

#define LV_FONT_MIN_WEIGHT              12
#define LV_FONT_MAX_WEIGHT              73

#ifndef GUI_FONT_FILE_NAME
#define GUI_FONT_FILE_NAME              "gui_font.ttf"
#endif

/* NFS debug: fonts deployed under gui/font by scripts/nfs/deploy.sh */
#ifndef FONT_FILE_PATH_NFS
#define FONT_FILE_PATH_NFS              "/mnt/nfs/gui/font/gui_font.ttf"
#endif

#if LV_USE_FREETYPE
#include "lv_freetype.h"

static lv_font_t *font[LV_FONT_MAX_WEIGHT] = {0};
static char s_font_path[256];
#endif

static const char *gui_font_resolve_ttf_path(char *resolved, size_t resolved_size)
{
    const char *candidates[3];
    size_t i;
    size_t n = 0;

    if (!resolved || resolved_size == 0) {
        return NULL;
    }

    candidates[n++] = FONT_FILE_PATH;
    candidates[n++] = FONT_FILE_PATH_NFS;
    candidates[n++] = NULL;

    for (i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], F_OK) == 0) {
            snprintf(resolved, resolved_size, "%s", candidates[i]);
            return resolved;
        }
    }

    return NULL;
}

#if LV_USE_FREETYPE
static lv_font_t *gui_font_create_weight(uint16_t weight)
{
    lv_font_t *created;

    if (s_font_path[0] == '\0') {
        return NULL;
    }

    created = lv_freetype_font_create(s_font_path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                      weight, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!created) {
        log_error("failed to create font weight %u", (unsigned)weight);
    }
    return created;
}
#endif

lv_result_t gui_font_init(void)
{
#if LV_USE_FREETYPE
    char ttf_path[256] = {0};
    const char *use_ttf;

    s_font_path[0] = '\0';

    use_ttf = gui_font_resolve_ttf_path(ttf_path, sizeof(ttf_path));
    if (!use_ttf) {
        log_warn("gui font not found (%s / %s), CJK labels use Montserrat fallback",
                 FONT_FILE_PATH, FONT_FILE_PATH_NFS);
        return LV_RESULT_OK;
    }

#if LV_FREETYPE_USE_LVGL_PORT
    snprintf(s_font_path, sizeof(s_font_path), "%c:%s", LV_FS_STDIO_LETTER, use_ttf);
#else
    snprintf(s_font_path, sizeof(s_font_path), "%s", use_ttf);
#endif
    log_info("font file ready (lazy load): %s", s_font_path);
#endif

    return LV_RESULT_OK;
}

lv_result_t gui_font_release(void)
{
#if LV_USE_FREETYPE
    int i = 0;

    for (i = 1; i < LV_FONT_MAX_WEIGHT; i++) {
        if (font[i]) {
            lv_freetype_font_delete(font[i]);
            font[i] = NULL;
        }
    }
    s_font_path[0] = '\0';
#endif

    return LV_RESULT_OK;
}

lv_font_t *gui_font_get_freetype_font(uint16_t weight)
{
#if LV_USE_FREETYPE
    if ((LV_FONT_MIN_WEIGHT > weight) || (LV_FONT_MAX_WEIGHT < weight)) {
        LV_LOG_ERROR("not support this weight of font\n");
        return NULL;
    }

    if (!font[weight]) {
        font[weight] = gui_font_create_weight(weight);
    }
    return font[weight];
#else
    LV_LOG_ERROR("not support freetype font, because LV_USE_FREETYPE isn't set\n");
#endif

    return NULL;
}

lv_font_t *gui_font_get_cjk_font(uint16_t weight)
{
#if LV_USE_FREETYPE
    static const uint16_t fallback_weights[] = {30, 22, 20, 18, 16, 14, 0};
    lv_font_t *font;
    size_t i;

    if ((LV_FONT_MIN_WEIGHT <= weight) && (LV_FONT_MAX_WEIGHT >= weight)) {
        font = gui_font_get_freetype_font(weight);
        if (font) {
            return font;
        }
    }

    for (i = 0; fallback_weights[i] != 0; i++) {
        if (fallback_weights[i] == weight) {
            continue;
        }
        font = gui_font_get_freetype_font(fallback_weights[i]);
        if (font) {
            return font;
        }
    }
#endif

    return NULL;
}

lv_font_t *gui_font_get_montserrat_font(uint16_t weight)
{
    /*
     * Prefer the exact enabled size when present. Otherwise snap to the nearest
     * retained Nova size (typically 28/48) without referencing disabled symbols.
     */
    uint16_t w = weight;

    if (w % 2)
        w = (uint16_t)((w + 1) & (uint16_t)~1);
    if (w < 8)
        w = 8;
    if (w > 48)
        w = 48;

#if LV_FONT_MONTSERRAT_8
    if (w == 8) return (lv_font_t *)&lv_font_montserrat_8;
#endif
#if LV_FONT_MONTSERRAT_10
    if (w == 10) return (lv_font_t *)&lv_font_montserrat_10;
#endif
#if LV_FONT_MONTSERRAT_12
    if (w == 12) return (lv_font_t *)&lv_font_montserrat_12;
#endif
#if LV_FONT_MONTSERRAT_14
    if (w == 14) return (lv_font_t *)&lv_font_montserrat_14;
#endif
#if LV_FONT_MONTSERRAT_16
    if (w == 16) return (lv_font_t *)&lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_18
    if (w == 18) return (lv_font_t *)&lv_font_montserrat_18;
#endif
#if LV_FONT_MONTSERRAT_20
    if (w == 20) return (lv_font_t *)&lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_22
    if (w == 22) return (lv_font_t *)&lv_font_montserrat_22;
#endif
#if LV_FONT_MONTSERRAT_24
    if (w == 24) return (lv_font_t *)&lv_font_montserrat_24;
#endif
#if LV_FONT_MONTSERRAT_26
    if (w == 26) return (lv_font_t *)&lv_font_montserrat_26;
#endif
#if LV_FONT_MONTSERRAT_28
    if (w == 28) return (lv_font_t *)&lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_30
    if (w == 30) return (lv_font_t *)&lv_font_montserrat_30;
#endif
#if LV_FONT_MONTSERRAT_32
    if (w == 32) return (lv_font_t *)&lv_font_montserrat_32;
#endif
#if LV_FONT_MONTSERRAT_34
    if (w == 34) return (lv_font_t *)&lv_font_montserrat_34;
#endif
#if LV_FONT_MONTSERRAT_36
    if (w == 36) return (lv_font_t *)&lv_font_montserrat_36;
#endif
#if LV_FONT_MONTSERRAT_38
    if (w == 38) return (lv_font_t *)&lv_font_montserrat_38;
#endif
#if LV_FONT_MONTSERRAT_40
    if (w == 40) return (lv_font_t *)&lv_font_montserrat_40;
#endif
#if LV_FONT_MONTSERRAT_42
    if (w == 42) return (lv_font_t *)&lv_font_montserrat_42;
#endif
#if LV_FONT_MONTSERRAT_44
    if (w == 44) return (lv_font_t *)&lv_font_montserrat_44;
#endif
#if LV_FONT_MONTSERRAT_46
    if (w == 46) return (lv_font_t *)&lv_font_montserrat_46;
#endif
#if LV_FONT_MONTSERRAT_48
    if (w == 48) return (lv_font_t *)&lv_font_montserrat_48;
#endif

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
