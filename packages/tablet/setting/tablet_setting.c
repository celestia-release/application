// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Settings UI aligned with backup: application - 副本/packages/tablet/src/tablet_setting.c
 * Left tab bar + lazy tab content; system information is embedded in the right pane (no extra window).
 */
#define LOG_TAG "TABLET_SETTING"

#include "tablet_setting_ai.h"
#include "tablet_setting_wifi.h"
#include "tablet_setting_qr_scan.h"
#include "tablet_setting_storage.h"
#include "tablet_setting_apps.h"
#include "tablet_setting.h"

#ifdef CONFIG_APP_TABLET_SETTING

#include "log.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>

#include "tablet_font.h"
#include "tablet_info.h"

#define CONFIG_AI 1

/*
 * WLAN/QR tabs: config.h defines CONFIG_FW_NETWORK_WIFI as a feature flag (may be empty).
 * Do not require CONFIG_NETWORK_SUPPORTED (Makefile -D); that hid these tabs when unset.
 */
#ifdef CONFIG_FW_NETWORK_WIFI
#define CONFIG_NETWORK_WLAN 1
#else
#define CONFIG_NETWORK_WLAN 0
#endif

/* Storage tab: use app_globle_config.h (LV_DEMO_STORAGE), not bare config.mk Makefile vars.
 * Those vars are invisible to #ifdef unless passed as -DCFLAGS (see nova/Makefile). */
#ifdef CONFIG_APP_TABLET_LV_DEMO_STORAGE
#define CONFIG_SETTING_STORAGE 1
#else
#define CONFIG_SETTING_STORAGE 0
#endif

/* Bump when changing setting/storage UI so device logs can prove afht libsetting_app.so was refreshed. */
#define SETTING_BUILD_TAG "tablet_setting_storage_tab=20260624a"

#if CONFIG_NETWORK_WLAN
static uint32_t s_setting_last_tab;
#endif

/* Enough slots for AI + WLAN + QR + language + storage + apps + system info */
#define SETTING_TAB_MAX 8

static lv_result_t window_setting_init(lv_obj_t *parent);
static void event_handler_btn_back(lv_event_t *e);
static void event_handler_tabview_scroll(lv_event_t *e);
static void event_handler_tab_changed(lv_event_t *e);
static void event_handler_tab_btn_clicked(lv_event_t *e);
static void ensure_tab_content(lv_obj_t *tabview, uint32_t idx);
static void event_handler_tabview_size_changed(lv_event_t *e);
static void fill_tab_ai(lv_obj_t *tab);
static void fill_tab_wlan(lv_obj_t *tab);
static void fill_tab_qr(lv_obj_t *tab);
static void fill_tab_language(lv_obj_t *tab);
#if CONFIG_SETTING_STORAGE
static void fill_tab_storage(lv_obj_t *tab);
#endif
static void fill_tab_apps(lv_obj_t *tab);
#ifdef CONFIG_APP_TABLET_SETTING
static void fill_tab_info(lv_obj_t *tab);
#endif
#if CONFIG_NETWORK_WLAN
static void sync_setting_tab(uint32_t new_idx);
static uint32_t setting_qr_tab_index(void);
#endif

static lv_obj_t *WindowSetting = NULL;
static lv_obj_t *s_desktop_main = NULL;
static uint32_t s_setting_active_tab = 0;

static uint8_t s_tab_created[SETTING_TAB_MAX];
static lv_obj_t *s_tabview;

uint16_t selected_index = 0;
lv_obj_t *dropdown;

static void language_dropdown_event_handler(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    log_info("[setting] language_dropdown_event code=%u target=%p", (unsigned)code, (void *)dd);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_font_t *f = tablet_font_get_freetype_font(20);
        if (f)
            lv_obj_set_style_text_font(list, f, 0);
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        char selected_text[32];
        lv_dropdown_get_selected_str(dd, selected_text, sizeof(selected_text));
        selected_index = lv_dropdown_get_selected(dd);
        log_debug("selected_index=%d", selected_index);
        /* Apply now and mark settings dirty: the main process restarts when
         * this app exits (no device reboot). set_lan_index persists the
         * language and marks the restart marker. */
        set_lan_index(selected_index);
        LV_LOG_USER("Option selected: %s (index: %d)", selected_text, selected_index);
    }
}

lv_result_t tablet_setting_create(lv_obj_t *main)
{
    log_info("[setting] create enter, main=%p build_tag=%s CONFIG_SETTING_STORAGE=%d",
             (void *)main, SETTING_BUILD_TAG, (int)CONFIG_SETTING_STORAGE);
    if (NULL != WindowSetting) {
        log_error("[setting] last running not finish");
        return LV_RESULT_INVALID;
    }

    lv_obj_t *screen = lv_display_get_screen_active(lv_display_get_default());
    if (!screen) {
        log_error("[setting] no active screen");
        return LV_RESULT_INVALID;
    }

    WindowSetting = lv_obj_create(screen);
    if (!WindowSetting) {
        log_error("[setting] lv_obj_create window failed");
        return LV_RESULT_INVALID;
    }

    lv_obj_remove_style_all(WindowSetting);
    lv_obj_set_size(WindowSetting, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(WindowSetting, lv_color_hex(SETTING_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(WindowSetting, LV_OPA_COVER, 0);

    s_desktop_main = main;
    tablet_create_top(WindowSetting, lan("Settings"), event_handler_btn_back, main);
    lv_refr_now(lv_display_get_default());

    if (window_setting_init(WindowSetting) != LV_RESULT_OK) {
        log_error("[setting] window_setting_init failed");
        lv_obj_delete(WindowSetting);
        WindowSetting = NULL;
        return LV_RESULT_INVALID;
    }

    log_info("[setting] create done ok");
    return LV_RESULT_OK;
}

void tablet_setting_flat_container(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_outline_pad(obj, 0, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_shadow_spread(obj, 0, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
}

void tablet_setting_hide_for_restart(void)
{
    if (WindowSetting && lv_obj_is_valid(WindowSetting)) {
        lv_obj_add_flag(WindowSetting, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_result_t window_setting_init(lv_obj_t *parent)
{
    log_info("[setting] window_setting_init enter, parent=%p", (void *)parent);
    if (!parent)
        return LV_RESULT_INVALID;

    lv_obj_t *tabview = lv_tabview_create(parent);
    if (!tabview)
        return LV_RESULT_INVALID;

    lv_obj_set_size(tabview, lv_pct(100), lv_pct(90));
    lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(lv_tabview_get_content(tabview), event_handler_tabview_scroll, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_remove_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tab_cont = lv_tabview_get_content(tabview);
    lv_obj_remove_flag(tab_cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    uint32_t nch = lv_obj_get_child_count(tab_cont);
    for (uint32_t i = 0; i < nch; i++) {
        lv_obj_t *tab_page = lv_obj_get_child(tab_cont, i);
        lv_obj_remove_flag(tab_page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tab_page, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }

    lv_tabview_set_tab_bar_position(tabview, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tabview, 128);

    lv_obj_remove_flag(tabview, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(tabview, lv_color_hex(SETTING_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);

    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
    if (!tab_bar)
        return LV_RESULT_INVALID;

    lv_obj_set_width(tab_bar, 128);
    lv_obj_set_flex_grow(tab_bar, 0);
    lv_obj_set_style_min_width(tab_bar, 128, 0);
    lv_obj_set_style_max_width(tab_bar, 128, 0);

    lv_obj_t *tab_content = lv_tabview_get_content(tabview);
    if (tab_content) {
        lv_obj_set_flex_grow(tab_content, 1);
        lv_obj_set_style_min_width(tab_content, 0, 0);
        lv_obj_set_style_bg_color(tab_content, lv_color_hex(SETTING_CONTENT_BG), 0);
        lv_obj_set_style_bg_opa(tab_content, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(tabview, event_handler_tabview_size_changed, LV_EVENT_SIZE_CHANGED, NULL);
    }

    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(SETTING_SIDEBAR_BG), 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_row(tab_bar, 6, 0);
    lv_obj_set_style_pad_column(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 10, 0);

    lv_font_t *font = tablet_font_get_freetype_font(22);
    if (!font)
        font = tablet_font_get_freetype_font(20);
    if (!font)
        font = tablet_font_get_freetype_font(18);
    if (font)
        lv_obj_set_style_text_font(tab_bar, font, 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(SETTING_TEXT), 0);

    memset(s_tab_created, 0, sizeof(s_tab_created));
    s_tabview = tabview;

    lv_obj_t *tab;
    const char *title;
#if CONFIG_AI
    title = lan("AI");
    if (!title)
        title = "AI";
    tab = lv_tabview_add_tab(tabview, title);
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);
#endif

#if CONFIG_NETWORK_WLAN
    tab = lv_tabview_add_tab(tabview, lan("WLAN"));
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);

    tab = lv_tabview_add_tab(tabview, lan("QR Code Setup"));
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);
#endif

    tab = lv_tabview_add_tab(tabview, lan("language"));
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);

#if CONFIG_SETTING_STORAGE
    title = lan("Storage") ? lan("Storage") : "Storage";
    tab = lv_tabview_add_tab(tabview, title);
    log_info("[setting] add tab Storage title=%s", title ? title : "(null)");
    tablet_setting_storage_set_tab_index(lv_tabview_get_tab_count(tabview) - 1);
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);
#endif

    title = lan("Apps") ? lan("Apps") : "Apps";
    tab = lv_tabview_add_tab(tabview, title);
    tablet_setting_apps_set_tab_index(lv_tabview_get_tab_count(tabview) - 1);
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);

#ifdef CONFIG_APP_TABLET_SETTING
    tab = lv_tabview_add_tab(tabview, lan("System Information"));
    lv_obj_set_style_text_font(tab, tablet_font_get_freetype_font(20), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(SETTING_TEXT), 0);
#endif

    {
        lv_obj_t *tc = lv_tabview_get_content(tabview);
        if (tc) {
            tablet_setting_flat_container(tc);
            lv_obj_set_style_pad_all(tc, 0, 0);
            uint32_t tn = lv_obj_get_child_count(tc);
            for (uint32_t ti = 0; ti < tn; ti++) {
                lv_obj_t *tp = lv_obj_get_child(tc, ti);
                lv_obj_remove_flag(tp, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(tp, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
                tablet_setting_flat_container(tp);
                lv_obj_set_style_pad_all(tp, 0, 0);
            }
        }
        tablet_setting_flat_container(tabview);
    }

    lv_obj_add_event_cb(tabview, event_handler_tab_changed, LV_EVENT_VALUE_CHANGED, NULL);

    uint32_t tab_cnt = lv_tabview_get_tab_count(tabview);
    for (uint32_t i = 0; i < tab_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
        if (!btn)
            continue;
        /* Launcher-style selected pill: rounded frosted tile + accent text,
         * no hard border stripe. */
        lv_obj_set_style_bg_color(btn, lv_color_hex(SETTING_SIDEBAR_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(SETTING_SIDEBAR_SEL), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, GUI_SCALE(12), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(SETTING_TEXT_SEC), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(SETTING_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(btn, 10, 0);
        if (font)
            lv_obj_set_style_text_font(btn, font, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(SETTING_TEXT_SEC), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(SETTING_ACCENT), LV_STATE_CHECKED);
            if (font)
                lv_obj_set_style_text_font(label, font, 0);
        }
        lv_obj_add_event_cb(btn, event_handler_tab_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    {
        uint32_t ti = lv_tabview_get_tab_active(tabview);
#if CONFIG_NETWORK_WLAN
        s_setting_last_tab = ti;
#endif
        s_setting_active_tab = ti;
        ensure_tab_content(tabview, ti);
    }

    lv_obj_update_layout(tabview);
    {
        lv_obj_t *content = lv_tabview_get_content(tabview);
        if (content) {
            int32_t avail_w = lv_obj_get_content_width(tabview);
            int32_t cw = avail_w - 128;
            if (cw > 0)
                lv_obj_set_width(content, cw);
        }
    }
    log_info("[setting] window_setting_init done tab_count=%u storage_tab=%d build_tag=%s",
             (unsigned)lv_tabview_get_tab_count(tabview), (int)CONFIG_SETTING_STORAGE, SETTING_BUILD_TAG);
    return LV_RESULT_OK;
}

static void fill_tab_language(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_language");
    dropdown = lv_dropdown_create(tab);
    if (!dropdown) {
        log_error("[setting] dropdown_create failed");
        return;
    }
    lv_font_t *drop_font = tablet_font_get_freetype_font(20);
    if (drop_font)
        lv_obj_set_style_text_font(dropdown, drop_font, 0);
    lv_obj_set_size(dropdown, lv_pct(50), lv_pct(10));
    lv_obj_center(dropdown);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(SETTING_TEXT), 0);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(dropdown, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_border_width(dropdown, 1, 0);
    lv_obj_set_style_radius(dropdown, GUI_SCALE(12), 0);
    lv_obj_set_style_pad_hor(dropdown, GUI_SCALE(14), 0);
    lv_dropdown_set_options(dropdown, "English\n中文");
    lv_dropdown_set_selected(dropdown, get_lan_index());
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (list) {
        lv_obj_set_style_text_font(list, tablet_font_get_freetype_font(20), 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(SETTING_CARD_BG), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(SETTING_TEXT), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(list, SETTING_CARD_BORDER_OPA, 0);
        lv_obj_set_style_border_width(list, 1, 0);
        lv_obj_set_style_radius(list, GUI_SCALE(12), 0);
        lv_obj_set_style_shadow_width(list, GUI_SCALE(12), 0);
        lv_obj_set_style_shadow_color(list, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(list, LV_OPA_40, 0);
    }
    lv_obj_add_event_cb(dropdown, language_dropdown_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
}

#if CONFIG_SETTING_STORAGE
static void fill_tab_storage(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_storage");
    tablet_setting_storage_create(tab);
}
#endif

static void fill_tab_apps(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_apps");
    tablet_setting_apps_create(tab);
}

#if CONFIG_AI
static void fill_tab_ai(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_ai");
    tablet_setting_ai_create(tab);
}
#endif

#if CONFIG_NETWORK_WLAN
static void fill_tab_wlan(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_wlan");
    tablet_setting_wifi_create(tab);
}

static void fill_tab_qr(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_qr");
    tablet_setting_qr_scan_create(tab);
}

static uint32_t setting_qr_tab_index(void)
{
    uint32_t n = 0;
#if CONFIG_AI
    n++;
#endif
    n++; /* WLAN */
    return n; /* QR */
}

static void sync_setting_tab(uint32_t new_idx)
{
    if (!s_tabview)
        return;
#if CONFIG_SETTING_STORAGE
    if (!tablet_setting_storage_try_leave_to_tab(s_setting_active_tab, new_idx)) {
        lv_tabview_set_active(s_tabview, s_setting_active_tab, LV_ANIM_OFF);
        return;
    }
#endif
    {
        uint32_t apps_idx = tablet_setting_apps_tab_index();
        if (s_setting_active_tab == apps_idx && new_idx != apps_idx)
            tablet_setting_apps_on_leave();
    }
    uint32_t qr_idx = setting_qr_tab_index();
    if (s_setting_last_tab == qr_idx && new_idx != qr_idx) {
        log_info("[setting] leave QR tab -> qr_scan_close");
        tablet_setting_qr_scan_close();
        if (qr_idx < SETTING_TAB_MAX)
            s_tab_created[qr_idx] = 0;
    }
    s_setting_last_tab = new_idx;
    s_setting_active_tab = new_idx;
    ensure_tab_content(s_tabview, new_idx);
    if (new_idx == tablet_setting_apps_tab_index() && s_tab_created[new_idx])
        tablet_setting_apps_on_enter();
}
#endif /* CONFIG_NETWORK_WLAN */

#ifdef CONFIG_APP_TABLET_SETTING
static void fill_tab_info(lv_obj_t *tab)
{
    log_info("[setting] fill_tab_info (embedded)");
    tablet_info_embed_in_parent(tab);
}
#endif

static void ensure_tab_content(lv_obj_t *tabview, uint32_t idx)
{
    uint32_t tab_count = lv_tabview_get_tab_count(tabview);
    if (idx >= tab_count || idx >= SETTING_TAB_MAX)
        return;
    if (s_tab_created[idx])
        return;
    lv_obj_t *cont = lv_tabview_get_content(tabview);
    if (!cont)
        return;
    lv_obj_t *tab = lv_obj_get_child(cont, idx);
    if (!tab)
        return;

    uint32_t n = 0;
#if CONFIG_AI
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_ai(tab);
        return;
    }
#endif
#if CONFIG_NETWORK_WLAN
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_wlan(tab);
        return;
    }
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_qr(tab);
        return;
    }
#endif
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_language(tab);
        return;
    }
#if CONFIG_SETTING_STORAGE
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_storage(tab);
        return;
    }
#endif
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_apps(tab);
        return;
    }
#ifdef CONFIG_APP_TABLET_SETTING
    if (idx == n++) {
        s_tab_created[idx] = 1;
        fill_tab_info(tab);
        return;
    }
#endif
}

static void event_handler_tab_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *tabview = s_tabview;
    uint32_t new_idx;

    if (!tabview)
        return;
    new_idx = lv_tabview_get_tab_active(tabview);
#if CONFIG_SETTING_STORAGE
    if (!tablet_setting_storage_try_leave_to_tab(s_setting_active_tab, new_idx)) {
        lv_tabview_set_active(tabview, s_setting_active_tab, LV_ANIM_OFF);
        return;
    }
#endif
#if CONFIG_NETWORK_WLAN
    sync_setting_tab(new_idx);
#else
    {
        uint32_t apps_idx = tablet_setting_apps_tab_index();
        if (s_setting_active_tab == apps_idx && new_idx != apps_idx)
            tablet_setting_apps_on_leave();
    }
    s_setting_active_tab = new_idx;
    ensure_tab_content(tabview, new_idx);
    if (new_idx == tablet_setting_apps_tab_index() && s_tab_created[new_idx])
        tablet_setting_apps_on_enter();
#endif
}

static void event_handler_tab_btn_clicked(lv_event_t *e)
{
    uint32_t idx = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    if (!s_tabview)
        return;
#if CONFIG_SETTING_STORAGE
    if (!tablet_setting_storage_try_leave_to_tab(s_setting_active_tab, idx)) {
        return;
    }
#endif
    lv_tabview_set_active(s_tabview, idx, LV_ANIM_OFF);
#if CONFIG_NETWORK_WLAN
    sync_setting_tab(idx);
#else
    {
        uint32_t apps_idx = tablet_setting_apps_tab_index();
        if (s_setting_active_tab == apps_idx && idx != apps_idx)
            tablet_setting_apps_on_leave();
    }
    s_setting_active_tab = idx;
    ensure_tab_content(s_tabview, idx);
    if (idx == tablet_setting_apps_tab_index() && s_tab_created[idx])
        tablet_setting_apps_on_enter();
#endif
}

static void event_handler_btn_back(lv_event_t *e)
{
    log_info("[setting] btn_back enter");
#if CONFIG_SETTING_STORAGE
    if (!tablet_setting_storage_try_leave_settings()) {
        return;
    }
#endif
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *label = target ? lv_obj_get_child(target, 0) : NULL;
    lv_obj_t *obj = lv_event_get_user_data(e);
    lv_obj_t *desktop = NULL;

    if (label)
        lv_obj_set_style_text_color(label, lv_color_hex(SETTING_ACCENT), 0);

    if (obj && lv_obj_is_valid(obj))
        desktop = obj;
    else if (s_desktop_main && lv_obj_is_valid(s_desktop_main))
        desktop = s_desktop_main;
    if (desktop) {
        lv_obj_remove_flag(desktop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(desktop);
    }
    if (WindowSetting)
        lv_obj_add_flag(WindowSetting, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(lv_display_get_default());

    tablet_setting_qr_scan_close();
    tablet_setting_wifi_close();
    tablet_setting_apps_on_leave();
    tablet_setting_apps_reset_ui();

    if (WindowSetting) {
        lv_obj_delete(WindowSetting);
        WindowSetting = NULL;
        s_tabview = NULL;
    }
    log_info("[setting] btn_back done");
}

static void event_handler_tabview_scroll(lv_event_t *e)
{
    lv_anim_t *a = lv_event_get_param(e);
    if (a)
        a->duration = 0;
}

static void event_handler_tabview_size_changed(lv_event_t *e)
{
    lv_obj_t *tabview = lv_event_get_target(e);
    lv_obj_t *tab_content = lv_tabview_get_content(tabview);
    if (!tab_content)
        return;
    int32_t avail_w = lv_obj_get_content_width(tabview);
    int32_t cw = avail_w - 128;
    if (cw > 0)
        lv_obj_set_width(tab_content, cw);
}

#endif /* CONFIG_APP_TABLET_SETTING */
