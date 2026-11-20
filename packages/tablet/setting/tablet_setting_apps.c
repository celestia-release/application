// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Settings tab: list all installed apps with enable checkboxes.
 * Migrated from gui/nova/setting/nova_setting_apps.c → packages/tablet.
 */
#define LOG_TAG "TABLET_SETTING_APPS"

#include "tablet_setting_apps.h"
#include "tablet_setting.h"

#include "tablet_app_registry.h"
#include "tablet_common.h"
#include "tablet_font.h"
#include "log.h"

#include <stdint.h>
#include <string.h>

#define APPS_TAB_INDEX_INVALID UINT32_MAX

static uint32_t s_apps_tab_index = APPS_TAB_INDEX_INVALID;
static lv_obj_t *s_list_box = NULL;
static int s_building = 0;

void tablet_setting_apps_set_tab_index(uint32_t idx)
{
    s_apps_tab_index = idx;
}

uint32_t tablet_setting_apps_tab_index(void)
{
    return s_apps_tab_index;
}

static void apps_apply_enabled(lv_obj_t *cb, int enabled)
{
    int index;

    if (!cb)
        return;

    index = (int)(intptr_t)lv_obj_get_user_data(cb);
    if (tablet_app_registry_set_enabled_by_index(index, enabled) != 0) {
        /* setting lock or invalid: restore UI */
        s_building = 1;
        lv_obj_add_state(cb, LV_STATE_CHECKED);
        s_building = 0;
        return;
    }
    if (tablet_app_registry_save() != 0)
        log_warn("apps: save installed_apps.json failed");
    /* App set changed: the desktop menu rebuilds when the main process
     * restarts on this app's exit. */
    tablet_app_settings_dirty_mark();
}

static void apps_checkbox_event_handler(lv_event_t *e)
{
    lv_obj_t *cb;
    int enabled;

    if (s_building)
        return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;

    cb = lv_event_get_target(e);
    if (!cb)
        return;

    enabled = lv_obj_has_state(cb, LV_STATE_CHECKED) ? 1 : 0;
    apps_apply_enabled(cb, enabled);
}

/* Left half of row toggles checkbox; right half stays non-clickable for list scroll. */
static void apps_left_hit_event_handler(lv_event_t *e)
{
    lv_obj_t *left;
    lv_obj_t *cb;
    int enabled;

    if (s_building)
        return;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    left = lv_event_get_current_target(e);
    if (!left)
        return;

    cb = (lv_obj_t *)lv_obj_get_user_data(left);
    if (!cb || !lv_obj_is_valid(cb))
        return;
    if (lv_obj_has_state(cb, LV_STATE_DISABLED))
        return;

    enabled = lv_obj_has_state(cb, LV_STATE_CHECKED) ? 0 : 1;
    s_building = 1;
    if (enabled)
        lv_obj_add_state(cb, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(cb, LV_STATE_CHECKED);
    s_building = 0;
    apps_apply_enabled(cb, enabled);
}

static void apps_clear_list(void)
{
    if (s_list_box && lv_obj_is_valid(s_list_box))
        lv_obj_clean(s_list_box);
}

static void apps_fill_list(void)
{
    const tablet_app_desc_t *apps;
    int count = 0;
    int i;
    lv_font_t *font18;
    lv_font_t *font16;

    if (!s_list_box || !lv_obj_is_valid(s_list_box))
        return;

    font18 = tablet_font_get_freetype_font(18);
    font16 = tablet_font_get_freetype_font(16);

    s_building = 1;
    apps_clear_list();

    apps = tablet_app_registry_list(&count);
    if (!apps || count <= 0) {
        lv_obj_t *empty = lv_label_create(s_list_box);
        lv_label_set_text(empty,
                          lan("No apps found") ? lan("No apps found") : "No apps found");
        if (font16)
            lv_obj_set_style_text_font(empty, font16, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(SETTING_TEXT_SEC), 0);
        s_building = 0;
        return;
    }

    for (i = 0; i < count; i++) {
        lv_obj_t *row;
        lv_obj_t *left;
        lv_obj_t *right;
        lv_obj_t *cb;
        lv_obj_t *label;
        const char *name;
        char shown_buf[64];
        const char *shown;
        int is_setting;

        row = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 48, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(SETTING_CARD_ITEM), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(SETTING_BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, GUI_SCALE(12), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        tablet_setting_flat_container(row);

        /* Left ~50%: toggle enable; right ~50%: leave for parent scroll gestures. */
        left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_width(left, LV_PCT(50));
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(left, 48, 0);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_left(left, 10, 0);
        lv_obj_set_style_pad_right(left, 6, 0);
        lv_obj_set_style_pad_ver(left, 8, 0);
        lv_obj_set_style_pad_column(left, 10, 0);
        lv_obj_set_style_bg_opa(left, LV_OPA_0, 0);
        tablet_setting_flat_container(left);

        right = lv_obj_create(row);
        lv_obj_remove_style_all(right);
        lv_obj_set_width(right, LV_PCT(50));
        lv_obj_set_height(right, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(right, 48, 0);
        lv_obj_set_style_bg_opa(right, LV_OPA_0, 0);
        lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
        tablet_setting_flat_container(right);

        cb = lv_checkbox_create(left);
        lv_checkbox_set_text(cb, "");
        lv_obj_set_user_data(cb, (void *)(intptr_t)apps[i].index);
        if (apps[i].enabled)
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(cb, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(cb, lv_color_hex(SETTING_TEXT), 0);
        lv_obj_set_style_bg_color(cb, lv_color_hex(SETTING_BORDER), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(cb, lv_color_hex(SETTING_ACCENT_OK),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(cb, 4, LV_PART_INDICATOR);
        lv_obj_set_style_transform_width(cb, 4, LV_PART_INDICATOR);
        lv_obj_set_style_transform_height(cb, 4, LV_PART_INDICATOR);

        is_setting = (strcmp(apps[i].app_id, "setting") == 0);
        if (is_setting) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
            lv_obj_add_state(cb, LV_STATE_DISABLED);
            lv_obj_set_style_text_color(cb, lv_color_hex(SETTING_TEXT_SEC), 0);
            lv_obj_set_style_bg_color(cb, lv_color_hex(SETTING_BORDER),
                                      LV_PART_INDICATOR | LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(cb, lv_color_hex(SETTING_ACCENT_OK),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_DISABLED);
            lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(left, cb);
            lv_obj_add_event_cb(left, apps_left_hit_event_handler, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(cb, apps_checkbox_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
        }

        name = apps[i].display_name[0] ? apps[i].display_name : apps[i].app_id;
        lan_app_name(name, shown_buf, sizeof(shown_buf));
        shown = shown_buf[0] ? shown_buf : name;
        label = lv_label_create(left);
        lv_label_set_text(label, shown);
        if (font18)
            lv_obj_set_style_text_font(label, font18, 0);
        lv_obj_set_style_text_color(label,
                                    lv_color_hex(is_setting ? SETTING_TEXT_SEC : SETTING_TEXT), 0);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }

    s_building = 0;
}

void tablet_setting_apps_on_enter(void)
{
    (void)tablet_app_registry_refresh();
    apps_fill_list();
}

void tablet_setting_apps_on_leave(void)
{
    (void)tablet_app_registry_refresh();
}

void tablet_setting_apps_reset_ui(void)
{
    s_list_box = NULL;
    s_building = 0;
}

lv_result_t tablet_setting_apps_create(lv_obj_t *parent)
{
    lv_obj_t *outer;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_font_t *font22;
    lv_font_t *font16;

    if (!parent)
        return LV_RESULT_INVALID;

    font22 = tablet_font_get_freetype_font(22);
    font16 = tablet_font_get_freetype_font(16);

    outer = lv_obj_create(parent);
    if (!outer)
        return LV_RESULT_INVALID;
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(outer, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(outer, GUI_SCALE(12), 0);
    lv_obj_set_style_pad_row(outer, 10, 0);
    tablet_setting_flat_container(outer);

    card = lv_obj_create(outer);
    if (!card)
        return LV_RESULT_INVALID;
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(card, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, GUI_SCALE(16), 0);
    lv_obj_set_style_shadow_width(card, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(card, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    tablet_setting_flat_container(card);

    title = lv_label_create(card);
    lv_label_set_text(title, lan("Apps") ? lan("Apps") : "Apps");
    if (font22)
        lv_obj_set_style_text_font(title, font22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(SETTING_TEXT), 0);

    subtitle = lv_label_create(card);
    lv_label_set_text(subtitle,
                      lan("Show or hide apps on the home screen")
                          ? lan("Show or hide apps on the home screen")
                          : "Show or hide apps on the home screen");
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    if (font16)
        lv_obj_set_style_text_font(subtitle, font16, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(SETTING_TEXT_SEC), 0);

    s_list_box = lv_obj_create(card);
    if (!s_list_box)
        return LV_RESULT_INVALID;
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_width(s_list_box, LV_PCT(100));
    lv_obj_set_flex_grow(s_list_box, 1);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 8, 0);
    lv_obj_set_style_bg_opa(s_list_box, LV_OPA_0, 0);
    lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_SCROLLABLE);
    tablet_setting_flat_container(s_list_box);

    tablet_setting_apps_on_enter();
    return LV_RESULT_OK;
}
