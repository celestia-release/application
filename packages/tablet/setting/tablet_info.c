// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo info/about screen implementation.
 */
#define LOG_TAG "TABLET_INFO"

/*********************
 *      INCLUDES
 *********************/
#include "tablet_info.h"

#include "config.h"

#ifdef CONFIG_APP_TABLET_SETTING

#if ((LVGL_VERSION_MAJOR == 9) && (LVGL_VERSION_MINOR >= 2))
#include "src/widgets/span/lv_span_private.h"
#endif

#include "log.h"

#include "tablet_font.h"
#include "tablet_setting.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
/**
 * @brief   Fullscreen body under title bar, or embedded in settings right pane.
 */
static void tablet_info_build_content(lv_obj_t *parent, int embedded);
static void event_handler_btn_back(lv_event_t *e);

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t *WindowInfo = NULL;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * @brief   create system information window.
 * @param   [IN]main: parent object
 * @return  lv_result_t
 * @retval  see definition of lv_result_t
*/
lv_result_t tablet_info_create(lv_obj_t *main)
{
    if (NULL != WindowInfo)
    {
        log_error("last running not finish\n");
        return LV_RESULT_INVALID;
    }

    WindowInfo = lv_obj_create(lv_display_get_screen_active(lv_display_get_default()));
    lv_obj_remove_style_all(WindowInfo);
    lv_obj_set_size(WindowInfo, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(WindowInfo, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(WindowInfo, LV_OPA_COVER, 0);

    /* Application title bar */
    tablet_create_top(WindowInfo, lan("System Information"), event_handler_btn_back, main);

    tablet_info_build_content(WindowInfo, 0);

    return LV_RESULT_OK;
}

/**
 * @brief   Build system information list inside an existing container (e.g. settings tab).
 */
void tablet_info_embed_in_parent(lv_obj_t *parent)
{
    if (!parent)
        return;
    tablet_info_build_content(parent, 1);
}

/**********************
 *  STATIC FUNCTIONS
 **********************/

/**
 * @brief   Build list + footer spans (fullscreen: below title bar; embedded: fill parent).
 */
static void tablet_info_build_content(lv_obj_t *parent, int embedded)
{
    lv_obj_t *obj_layout = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_layout);
    if (embedded) {
        lv_obj_set_size(obj_layout, LV_PCT(100), LV_PCT(100));
        lv_obj_align(obj_layout, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(obj_layout, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_set_size(obj_layout, LV_PCT(100), LV_PCT(90));
        lv_obj_align(obj_layout, LV_ALIGN_TOP_MID, 0, LV_PCT(10));
    }
    lv_obj_set_layout(obj_layout, LV_LAYOUT_FLEX);
    lv_obj_set_style_base_dir(obj_layout, LV_BASE_DIR_RTL, 0);
    lv_obj_set_flex_flow(obj_layout, LV_FLEX_FLOW_COLUMN);


    lv_obj_t *info_list = lv_list_create(obj_layout);
    lv_obj_set_size(info_list, LV_PCT(100), LV_PCT(80));
    lv_obj_set_style_bg_opa(info_list, LV_OPA_0, 0);
    lv_obj_set_style_border_opa(info_list, LV_OPA_0, 0);
    lv_obj_set_style_pad_row(info_list, 5, 0);

    lv_font_t *font_big = tablet_font_get_montserrat_font(30);
    lv_font_t *font_min = tablet_font_get_montserrat_font(20);

    lv_obj_t *btn = NULL;

    /* Section title */
    static lv_style_t style_title;
    lv_style_init(&style_title);
    lv_style_set_bg_opa(&style_title, LV_OPA_0);
    lv_style_set_text_font(&style_title, font_big);
    lv_style_set_text_color(&style_title, lv_color_hex(SETTING_TEXT_SEC));
    lv_style_set_pad_top(&style_title, 20);

    /* Button style */
    static lv_style_t style_btn;
    lv_style_init(&style_btn);
    lv_style_set_bg_opa(&style_btn, LV_OPA_0);
    lv_style_set_text_font(&style_btn, font_min);
    lv_style_set_text_color(&style_btn, lv_color_hex(SETTING_TEXT));

    /* Section title */
    lv_obj_t *title_1 = lv_list_add_text(info_list, "Hardware Specifications");
    lv_obj_add_style(title_1, &style_title, 0);

    btn = lv_list_add_button(info_list, "\xef" "KM02", "CHIP_SERIES");
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    btn = lv_list_add_button(info_list, "\xef" "128M/256M", "DRAM");
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    /* Section title */
    lv_obj_t *title_2 = lv_list_add_text(info_list, "Contact us");
    lv_obj_add_style(title_2, &style_title, 0);

    btn = lv_list_add_button(info_list, "\xef" "sales@anyka.com", "E-mail");
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    btn = lv_list_add_button(info_list, "\xef" "(86)-20-3221-9000", "Phone");
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    btn = lv_list_add_button(info_list, "\xef" "(86)-20-3221-9499", "Sales Hot-line");
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);


    lv_obj_t *obj_info = lv_obj_create(obj_layout);
    lv_obj_set_size(obj_info, LV_PCT(100), LV_PCT(20));
    lv_obj_set_style_bg_opa(obj_info, LV_OPA_0, 0);
    lv_obj_set_style_border_opa(obj_info, LV_OPA_0, 0);

    lv_obj_t *spans = lv_spangroup_create(obj_info);
    lv_obj_set_size(spans, LV_PCT(100), LV_PCT(100));

    lv_spangroup_set_align(spans, LV_TEXT_ALIGN_LEFT);
    lv_spangroup_set_overflow(spans, LV_SPAN_OVERFLOW_CLIP);
    lv_spangroup_set_indent(spans, 20);
    lv_spangroup_set_mode(spans, LV_SPAN_MODE_BREAK);

    lv_span_t *span1 = lv_spangroup_add_span(spans);
    lv_span_set_text_static(span1, "\nhttp://www.anyka.com");
    lv_style_set_text_color(&span1->style, lv_color_hex(SETTING_TEXT_SEC));
    lv_style_set_text_decor(&span1->style, LV_TEXT_DECOR_UNDERLINE);
    lv_style_set_text_opa(&span1->style, LV_OPA_70);

    lv_span_t *span2 = lv_spangroup_add_span(spans);
    lv_span_set_text_static(span2, "\nCopyright @ 2026 Guangzhou Anyka Microelectronics Co., Ltd. All Rights Reserved.");
    lv_style_set_text_color(&span2->style, lv_color_hex(SETTING_TEXT_SEC));
    lv_style_set_text_decor(&span2->style, LV_TEXT_DECOR_UNDERLINE);
    lv_style_set_text_opa(&span2->style, LV_OPA_70);
}

/**
 * @brief : The event_handler of btn_back
 * @param e
 * @return
 */
static void event_handler_btn_back(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(target, 0);
    lv_obj_t *obj = lv_event_get_user_data(e);

    lv_obj_set_style_text_color(label, lv_color_hex(SETTING_ACCENT), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);

    lv_obj_delete(WindowInfo);
    WindowInfo = NULL;
}

#endif /* CONFIG_APP_TABLET_SETTING */
