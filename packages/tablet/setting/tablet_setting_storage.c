// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Settings tab: switch between local SD+afht and USB disk (PC access) modes.
 */
#define LOG_TAG "TABLET_SETTING_STORAGE"

#include "tablet_setting_storage.h"
#include "tablet_setting.h"

#ifdef CONFIG_APP_TABLET_LV_DEMO_STORAGE

#include "tablet_common.h"
#include "tablet_font.h"
#include "tablet_gui.h"
#include "log.h"
#include "config.h"
#include "system_config.h"
#include "app_main.h"

#include "iniparser.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define STORAGE_MODE_CFG_PATH           "/etc/config/settings.json"
#define STORAGE_MODE_CFG_LEGACY         "/etc/config/storage_mode.ini"
#define STORAGE_MODE_SESSION_FLAG       "/tmp/storage_usb_session"
#define STORAGE_MODE_RESTART_PENDING      "/tmp/storage_restart_pending"
#define STORAGE_MODE_RESTART_UI_LOCK      "/tmp/storage_restart_ui_lock"
#define STORAGE_MODE_SWITCH_SCRIPT      "/usr/sbin/storage_mode_switch.sh"
/* Bump when storage UI logic changes; grep log for this tag to verify afht build. */
#define STORAGE_UI_BUILD_TAG            "tablet_storage_ui=20260626b"

#define STORAGE_MODE_LOCAL              0
#define STORAGE_MODE_USB_MSC            1

#define STORAGE_TAB_INDEX_INVALID       UINT32_MAX

static lv_obj_t *s_storage_dropdown;
static lv_obj_t *s_status_label;
static int s_storage_pending_index = -1;
static int s_session_usb_active = 0;
static int s_restore_dialog_open = 0;
static int s_storage_switching = 0;
static int s_confirmed_local_restart = 0;
static uint32_t s_storage_tab_index = STORAGE_TAB_INDEX_INVALID;

static void storage_apply_usb_session(void);
static void storage_show_restore_dialog(void);

static int storage_restart_in_progress(void);
static void storage_dropdown_event_handler(lv_event_t *e);
static void storage_apply_local_restart(void);
static void storage_request_local_restart(lv_obj_t *confirm_msgbox);

static int storage_ui_blocked(void)
{
    if (s_storage_switching || storage_restart_in_progress() || app_main_shutdown_requested()) {
        return 1;
    }
    if (access(STORAGE_MODE_RESTART_UI_LOCK, F_OK) == 0) {
        return 1;
    }
    return 0;
}

static void storage_mark_switching_started(void)
{
    FILE *fp;

    if (s_storage_switching) {
        return;
    }

    s_storage_switching = 1;
    s_confirmed_local_restart = 1;
    s_session_usb_active = 0;
    s_restore_dialog_open = 1;
    s_storage_pending_index = -1;
    (void)unlink(STORAGE_MODE_SESSION_FLAG);

    fp = fopen(STORAGE_MODE_RESTART_UI_LOCK, "w");
    if (fp) {
        (void)fputs("1\n", fp);
        (void)fclose(fp);
    }
}

static const char *storage_restart_wait_text(void)
{
    const char *text = lan("Main app is restarting, please wait.");

    if (!text) {
        text = "Main app is restarting, please wait.";
    }
    return text;
}

static void storage_begin_local_restart_ui(lv_obj_t *confirm_msgbox)
{
    if (confirm_msgbox && lv_obj_is_valid(confirm_msgbox)) {
        lv_msgbox_close(confirm_msgbox);
    }

    if (s_storage_dropdown && lv_obj_is_valid(s_storage_dropdown)) {
        lv_obj_remove_event_cb(s_storage_dropdown, storage_dropdown_event_handler);
    }

    log_info("storage: begin local_restart ui %s", STORAGE_UI_BUILD_TAG);
    /* Keep launcher/settings visible; only a centered msgbox, no full-screen mask. */
    msgbox_show_restart_waiting(storage_restart_wait_text());
}

static void storage_request_local_restart(lv_obj_t *confirm_msgbox)
{
    if (storage_restart_in_progress()) {
        log_info("storage: local_restart already in progress");
        return;
    }
    if (!s_storage_switching) {
        storage_mark_switching_started();
    }

    storage_begin_local_restart_ui(confirm_msgbox);
    msgbox_flush_restart_waiting_frame();
    /* Stop LVGL loop immediately: fb0 holds the last frame, no more rendering needed. */
    app_lv_stop_gui_loop();
    log_info("storage: restart wait frame flushed, GUI loop stopped, begin shutdown");
    storage_apply_local_restart();
}

static int storage_restart_in_progress(void)
{
    FILE *fp;
    char buf[32];
    long pid;

    if (access(STORAGE_MODE_RESTART_PENDING, F_OK) != 0) {
        return 0;
    }
    fp = fopen(STORAGE_MODE_RESTART_PENDING, "r");
    if (!fp) {
        return 1;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return 1;
    }
    fclose(fp);
    if (strncmp(buf, "starting", 8) == 0) {
        return 1;
    }
    pid = strtol(buf, NULL, 10);
    if (pid <= 0) {
        return 1;
    }
    if (kill((pid_t)pid, 0) == 0) {
        return 1;
    }
    return 0;
}

void tablet_setting_storage_set_tab_index(uint32_t idx)
{
    s_storage_tab_index = idx;
}

uint32_t tablet_setting_storage_tab_index(void)
{
    return s_storage_tab_index;
}

static int storage_mode_name_is_usb(const char *mode)
{
    if (!mode || !mode[0]) {
        return 0;
    }
    if (strcmp(mode, "1") == 0 ||
        strcmp(mode, "usb_msc") == 0 ||
        strcmp(mode, "usb") == 0) {
        return 1;
    }
    return 0;
}

static int storage_mode_read_saved_index(void)
{
    FILE *fp;
    long size;
    size_t nread;
    char *buf;
    cJSON *root;
    cJSON *storage;
    cJSON *mode;
    dictionary *ini;
    const char *mode_str;
    int idx = STORAGE_MODE_LOCAL;

    fp = fopen(STORAGE_MODE_CFG_PATH, "rb");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            size = ftell(fp);
            if (size > 0 && size <= (1024 * 1024)) {
                rewind(fp);
                buf = (char *)malloc((size_t)size + 1);
                if (buf) {
                    nread = fread(buf, 1, (size_t)size, fp);
                    buf[nread] = '\0';
                    root = cJSON_Parse(buf);
                    free(buf);
                    if (root) {
                        storage = cJSON_GetObjectItem(root, "storage");
                        mode = cJSON_IsObject(storage) ?
                               cJSON_GetObjectItem(storage, "mode") : NULL;
                        if (cJSON_IsNumber(mode)) {
                            idx = mode->valueint ? STORAGE_MODE_USB_MSC
                                                 : STORAGE_MODE_LOCAL;
                            cJSON_Delete(root);
                            fclose(fp);
                            return idx;
                        }
                        if (cJSON_IsString(mode) && mode->valuestring) {
                            idx = storage_mode_name_is_usb(mode->valuestring) ?
                                  STORAGE_MODE_USB_MSC : STORAGE_MODE_LOCAL;
                            cJSON_Delete(root);
                            fclose(fp);
                            return idx;
                        }
                        cJSON_Delete(root);
                    }
                }
            }
        }
        fclose(fp);
    }

    /* Legacy storage_mode.ini fallback */
    ini = iniparser_load(STORAGE_MODE_CFG_LEGACY);
    if (!ini) {
        return STORAGE_MODE_LOCAL;
    }
    mode_str = iniparser_getstring(ini, "storage:mode", "0");
    if (storage_mode_name_is_usb(mode_str)) {
        idx = STORAGE_MODE_USB_MSC;
    }
    iniparser_freedict(ini);
    return idx;
}

static int storage_mode_ui_index(void)
{
    if (s_storage_switching || storage_restart_in_progress()) {
        return STORAGE_MODE_LOCAL;
    }
    if (s_session_usb_active || access(STORAGE_MODE_SESSION_FLAG, F_OK) == 0) {
        return STORAGE_MODE_USB_MSC;
    }
    return storage_mode_read_saved_index();
}

int tablet_setting_storage_is_usb_active(void)
{
    if (storage_ui_blocked()) {
        return 0;
    }
    return storage_mode_ui_index() == STORAGE_MODE_USB_MSC;
}

static const char *storage_mode_status_text(int idx)
{
    if (idx == STORAGE_MODE_USB_MSC) {
        return lan("USB Disk Mode") ? lan("USB Disk Mode") : "USB Disk Mode";
    }
    return lan("Local Storage") ? lan("Local Storage") : "Local Storage";
}

static void storage_style_dropdown(lv_obj_t *dd, lv_font_t *font)
{
    if (!dd) {
        return;
    }
    if (font) {
        lv_obj_set_style_text_font(dd, font, 0);
    }
    lv_obj_set_style_bg_color(dd, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(SETTING_TEXT), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(dd, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, GUI_SCALE(10), 0);
    lv_obj_set_style_pad_hor(dd, 12, 0);
    lv_obj_set_style_pad_ver(dd, 10, 0);

    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        if (font) {
            lv_obj_set_style_text_font(list, font, 0);
        }
        lv_obj_set_style_bg_color(list, lv_color_hex(SETTING_CARD_BG), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(SETTING_TEXT), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(list, SETTING_CARD_BORDER_OPA, 0);
        lv_obj_set_style_border_width(list, 1, 0);
        lv_obj_set_style_radius(list, GUI_SCALE(10), 0);
        lv_obj_set_style_shadow_width(list, GUI_SCALE(12), 0);
        lv_obj_set_style_shadow_color(list, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(list, LV_OPA_40, 0);
    }
}

static void storage_update_status_label(int idx)
{
    const char *mode_text;
    char buf[96];

    if (!s_status_label) {
        return;
    }
    mode_text = storage_mode_status_text(idx);
    snprintf(buf, sizeof(buf), "%s: %s",
             lan("Current mode") ? lan("Current mode") : "Current mode",
             mode_text);
    lv_label_set_text(s_status_label, buf);
}

static void storage_sync_dropdown(void)
{
    if (s_storage_switching) {
        return;
    }
    int idx = storage_mode_ui_index();

    if (s_storage_dropdown) {
        lv_dropdown_set_selected(s_storage_dropdown, idx);
    }
    storage_update_status_label(idx);
    s_session_usb_active = (idx == STORAGE_MODE_USB_MSC) ? 1 : 0;
}

static void storage_close_inherited_fds(void)
{
    int fd;

    /* fork() copies the fd table; close before exec so shell/restart scripts do
     * not keep /dev/ipu open after ipc_main shuts down GPU/IPU. */
    for (fd = 3; fd < 256; fd++) {
        close(fd);
    }
}

static void storage_apply_local_restart(void)
{
    pid_t pid;

    if (storage_restart_in_progress()) {
        log_info("storage: local_restart already in progress");
        return;
    }
    if (!s_storage_switching) {
        storage_mark_switching_started();
    }

    log_info("storage: apply local_restart, request graceful shutdown");

    /*
     * wpa_supplicant is started via system() and may inherit /dev/ipu.
     * If it still holds IPU when we AK_MPI_IPU_Close(), some kernels wedge
     * (serial dies). Drop wifi daemons BEFORE GPU/IPU teardown.
     */
    (void)system("killall -9 wpa_supplicant >/dev/null 2>&1; "
                 "killall -9 udhcpc >/dev/null 2>&1; "
                 "killall -9 hostapd >/dev/null 2>&1");

    pid = fork();
    if (pid < 0) {
        log_info("storage: fork failed for local_restart");
        return;
    }
    if (pid == 0) {
        storage_close_inherited_fds();
        execl("/bin/sh", "sh", "-c",
              STORAGE_MODE_SWITCH_SCRIPT " apply local_restart", (char *)NULL);
        _exit(127);
    }

    app_main_request_shutdown();
}

static void storage_apply_usb_session(void)
{
    log_info("storage: apply usb_msc session");
    if (system(STORAGE_MODE_SWITCH_SCRIPT " apply usb_msc") == 0) {
        s_session_usb_active = 1;
        storage_sync_dropdown();
    } else {
        log_info("storage: apply usb_msc session failed");
        s_session_usb_active = 0;
        storage_sync_dropdown();
    }
}

static void storage_restore_dialog_handler(lv_obj_t *msgbox, uint32_t btn_id)
{
    if (btn_id == 0) {
        storage_request_local_restart(msgbox);
        return;
    }

    s_restore_dialog_open = 0;
    lv_msgbox_close(msgbox);
}

static void storage_show_restore_dialog(void)
{
    const char *hint;
    const char *footer_btns[] = { lan("ok"), lan("cancel"), NULL };
    lv_obj_t *scr;

    if (storage_ui_blocked() || s_confirmed_local_restart || s_restore_dialog_open) {
        return;
    }
    s_restore_dialog_open = 1;

    hint = lan("Will switch from USB disk mode to local storage and restart the app.");
    if (!hint) {
        hint = "Will switch from USB disk mode to local storage and restart the app.";
    }
    scr = lv_display_get_screen_active(lv_display_get_default());
    (void)msgbox_create_with_footer(scr, lan("note"), hint, footer_btns,
                                      storage_restore_dialog_handler, 0);
}

static void storage_auto_restore_on_leave(void)
{
    if (storage_ui_blocked() || s_confirmed_local_restart) {
        return;
    }
    if (!tablet_setting_storage_is_usb_active()) {
        return;
    }
    storage_show_restore_dialog();
}

int tablet_setting_storage_try_leave_to_tab(uint32_t current_tab_idx, uint32_t new_tab_idx)
{
    if (s_storage_tab_index == STORAGE_TAB_INDEX_INVALID) {
        return 1;
    }
    if (current_tab_idx != s_storage_tab_index || new_tab_idx == s_storage_tab_index) {
        return 1;
    }
    if (storage_ui_blocked() || s_confirmed_local_restart) {
        return 1;
    }
    if (!tablet_setting_storage_is_usb_active()) {
        return 1;
    }
    storage_auto_restore_on_leave();
    return 0;
}

int tablet_setting_storage_try_leave_settings(void)
{
    if (storage_ui_blocked() || s_confirmed_local_restart) {
        return 1;
    }
    if (!tablet_setting_storage_is_usb_active()) {
        return 1;
    }
    storage_auto_restore_on_leave();
    return 0;
}

static void storage_mode_footer_handler(lv_obj_t *msgbox, uint32_t btn_id)
{
    int pending = s_storage_pending_index;

    s_storage_pending_index = -1;

    if (btn_id != 0) {
        lv_msgbox_close(msgbox);
        storage_sync_dropdown();
        return;
    }

    if (pending == STORAGE_MODE_USB_MSC) {
        lv_msgbox_close(msgbox);
        storage_apply_usb_session();
        return;
    }

    if (pending == STORAGE_MODE_LOCAL) {
        storage_request_local_restart(msgbox);
    }
}

static void storage_dropdown_event_handler(lv_event_t *e)
{
    lv_obj_t *dd;
    lv_event_code_t code;
    int selected;
    int current;
    const char *hint;
    const char *footer_btns[] = { lan("ok"), lan("cancel"), NULL };

    dd = lv_event_get_target(e);
    code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED || !dd) {
        return;
    }

    selected = (int)lv_dropdown_get_selected(dd);
    current = storage_mode_ui_index();
    if (selected == current) {
        return;
    }
    if (storage_ui_blocked()) {
        storage_sync_dropdown();
        return;
    }

    s_storage_pending_index = selected;
    if (selected == STORAGE_MODE_USB_MSC) {
        hint = lan("Will switch from local storage to USB disk mode!");
        if (!hint) {
            hint = "Will switch from local storage to USB disk mode!";
        }
    } else {
        hint = lan("Will switch from USB disk mode to local storage and restart the app.");
        if (!hint) {
            hint = "Will switch from USB disk mode to local storage and restart the app.";
        }
    }

    lv_obj_t *scr = lv_display_get_screen_active(lv_display_get_default());
    (void)msgbox_create_with_footer(scr, lan("note"), hint, footer_btns,
                                      storage_mode_footer_handler, 0);
}

static lv_obj_t *storage_create_hint_row(lv_obj_t *parent, const char *text, lv_font_t *font)
{
    lv_obj_t *row;
    lv_obj_t *dot;
    lv_obj_t *label;

    row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);

    dot = lv_label_create(row);
    lv_label_set_text(dot, "•");
    lv_obj_set_style_text_color(dot, lv_color_hex(SETTING_ACCENT), 0);
    if (font) {
        lv_obj_set_style_text_font(dot, font, 0);
    }

    label = lv_label_create(row);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(SETTING_TEXT_SEC), 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    return row;
}

lv_result_t tablet_setting_storage_create(lv_obj_t *parent)
{
    lv_obj_t *outer;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *status_box;
    lv_obj_t *switch_row;
    lv_obj_t *switch_lbl;
    lv_obj_t *hint_box;
    lv_font_t *font22;
    lv_font_t *font18;
    lv_font_t *font16;
    int saved;

    log_info("[storage] create enter parent=%p", (void *)parent);
    if (!parent) {
        return LV_RESULT_INVALID;
    }

    font22 = tablet_font_get_freetype_font(22);
    font18 = tablet_font_get_freetype_font(18);
    font16 = tablet_font_get_freetype_font(16);
    saved = storage_mode_ui_index();
    s_session_usb_active = (saved == STORAGE_MODE_USB_MSC) ? 1 : 0;

    outer = lv_obj_create(parent);
    if (!outer) {
        return LV_RESULT_INVALID;
    }
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(outer, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(outer, GUI_SCALE(16), 0);
    tablet_setting_flat_container(outer);

    card = lv_obj_create(outer);
    if (!card) {
        return LV_RESULT_INVALID;
    }
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(92));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(card, 520, 0);
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
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    tablet_setting_flat_container(card);

    title = lv_label_create(card);
    lv_label_set_text(title, lan("Storage mode") ? lan("Storage mode") : "Storage mode");
    if (font22) {
        lv_obj_set_style_text_font(title, font22, 0);
    }
    lv_obj_set_style_text_color(title, lv_color_hex(SETTING_TEXT), 0);

    subtitle = lv_label_create(card);
    lv_label_set_text(subtitle,
                       lan("USB mode works only on this page; leaving restores local storage.")
                       ? lan("USB mode works only on this page; leaving restores local storage.")
                       : "USB mode works only on this page.");
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    if (font16) {
        lv_obj_set_style_text_font(subtitle, font16, 0);
    }
    lv_obj_set_style_text_color(subtitle, lv_color_hex(SETTING_TEXT_SEC), 0);

    status_box = lv_obj_create(card);
    lv_obj_remove_style_all(status_box);
    lv_obj_set_width(status_box, LV_PCT(100));
    lv_obj_set_height(status_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_box, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_bg_opa(status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(status_box, lv_color_hex(SETTING_BORDER), 0);
    lv_obj_set_style_border_width(status_box, 1, 0);
    lv_obj_set_style_radius(status_box, GUI_SCALE(10), 0);
    lv_obj_set_style_pad_hor(status_box, 14, 0);
    lv_obj_set_style_pad_ver(status_box, 12, 0);
    tablet_setting_flat_container(status_box);

    s_status_label = lv_label_create(status_box);
    if (font18) {
        lv_obj_set_style_text_font(s_status_label, font18, 0);
    }
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(SETTING_ACCENT), 0);
    storage_update_status_label(saved);

    switch_row = lv_obj_create(card);
    lv_obj_remove_style_all(switch_row);
    lv_obj_set_width(switch_row, LV_PCT(100));
    lv_obj_set_height(switch_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(switch_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(switch_row, 8, 0);
    lv_obj_set_style_bg_opa(switch_row, LV_OPA_0, 0);

    switch_lbl = lv_label_create(switch_row);
    lv_label_set_text(switch_lbl, lan("Switch to") ? lan("Switch to") : "Switch to");
    if (font18) {
        lv_obj_set_style_text_font(switch_lbl, font18, 0);
    }
    lv_obj_set_style_text_color(switch_lbl, lv_color_hex(SETTING_TEXT), 0);

    s_storage_dropdown = lv_dropdown_create(switch_row);
    if (!s_storage_dropdown) {
        return LV_RESULT_INVALID;
    }
    lv_obj_set_width(s_storage_dropdown, LV_PCT(100));
    lv_obj_set_height(s_storage_dropdown, GUI_SCALE_VER(48));
    storage_style_dropdown(s_storage_dropdown, font18);
    if (get_lan_index() == 0) {
        lv_dropdown_set_options(s_storage_dropdown, "Local Storage\nUSB Disk Mode");
    } else {
        lv_dropdown_set_options(s_storage_dropdown, "本地存储\nU盘模式");
    }
    lv_dropdown_set_selected(s_storage_dropdown, saved);
    lv_obj_add_event_cb(s_storage_dropdown, storage_dropdown_event_handler, LV_EVENT_VALUE_CHANGED, NULL);

    hint_box = lv_obj_create(card);
    lv_obj_remove_style_all(hint_box);
    lv_obj_set_width(hint_box, LV_PCT(100));
    lv_obj_set_height(hint_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hint_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hint_box, 6, 0);
    lv_obj_set_style_bg_opa(hint_box, LV_OPA_0, 0);
    lv_obj_set_style_pad_top(hint_box, 4, 0);

    storage_create_hint_row(hint_box,
                            lan("Running apps from SD card")
                            ? lan("Running apps from SD card")
                            : "Running apps from SD card",
                            font16);
    storage_create_hint_row(hint_box,
                            lan("Export SD to PC for file updates")
                            ? lan("Export SD to PC for file updates")
                            : "Export SD to PC for file updates",
                            font16);

    log_info("[storage] create done ui_mode_index=%d", saved);
    return LV_RESULT_OK;
}

#else /* CONFIG_APP_TABLET_LV_DEMO_STORAGE */

#include "lvgl.h"

lv_result_t tablet_setting_storage_create(lv_obj_t *parent)
{
    (void)parent;
    return LV_RESULT_OK;
}

void tablet_setting_storage_set_tab_index(uint32_t idx)
{
    (void)idx;
}

uint32_t tablet_setting_storage_tab_index(void)
{
    return UINT32_MAX;
}

int tablet_setting_storage_is_usb_active(void)
{
    return 0;
}

int tablet_setting_storage_try_leave_to_tab(uint32_t current_tab_idx, uint32_t new_tab_idx)
{
    (void)current_tab_idx;
    (void)new_tab_idx;
    return 1;
}

int tablet_setting_storage_try_leave_settings(void)
{
    return 1;
}

#endif /* CONFIG_APP_TABLET_LV_DEMO_STORAGE */
