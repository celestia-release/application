// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna dashcam UI on 240x240:
 *   [0..28)   RTSP URL
 *   [28..212) centered preview (240x184, same size as camera preview)
 *   [212..240) recording status + duration
 * Physical ENTER (116) toggles record; HOME (90) triggers still capture.
 * Preview + RTSP stay on. Swipe-up exits. Touch does not drive these keys
 * (keypad indev only).
 */
#define LOG_TAG "WEARABLE_DASHCAM_UI"

#include "dashcam_ui.h"
#include "dashcam_core.h"
#include "dashcam_record.h"
#include "dashcam_capture.h"
#include "dashcam_preview.h"

#include "app_main.h"
#include "gui_common.h"
#include "gui_font.h"
#include "gui_shell.h"
#include "log.h"
#include "wearable_home.h"

#if defined(CONFIG_NETWORK_SUPPORTED) && CONFIG_NETWORK_SUPPORTED
#include "network.h"
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

#define DASHCAM_COLOR_BG        0x000000
#define DASHCAM_COLOR_ACCENT    0x05fbe2
#define DASHCAM_COLOR_REC       0xff3b30
#define DASHCAM_COLOR_TEXT      0xffffff
#define DASHCAM_COLOR_DIM       0x888888

/* Match camera preview height: 240 - 56 bottom bar == 184; here margins are
 * split top/bottom (28 each) for RTSP / record OSD outside the canvas. */
#define DASHCAM_BAND_H          28
#define DASHCAM_PREVIEW_W       240
#define DASHCAM_PREVIEW_H       (240 - (DASHCAM_BAND_H * 2))
/* Poll keypad RELEASED so a new press can fire after the physical key lifts. */
#define DASHCAM_KEY_LATCH_POLL_MS   50

typedef struct {
    lv_obj_t *root;
    lv_obj_t *preview_area;
    lv_obj_t *rec_dot;
    lv_obj_t *rec_label;
    lv_obj_t *datetime_label; /* OSD on preview top-right */
    lv_obj_t *time_label;     /* session record duration */
    lv_obj_t *rtsp_label;
    lv_timer_t *timer;
    lv_timer_t *preview_kick_timer;
    lv_timer_t *key_latch_timer;
    int blink;
    int created;
    int preview_started;
} dashcam_ui_ctx_t;

static dashcam_ui_ctx_t s_ui;
/* Blocks LVGL long-press-repeat KEY floods until the physical key is released. */
static int s_key_latched;

static int dashcam_is_zh(void)
{
    return get_lan_index() == 1;
}

static void dashcam_get_ip(char *ip, size_t len)
{
    if (!ip || len == 0)
        return;
    ip[0] = '\0';

#if defined(CONFIG_NETWORK_SUPPORTED) && CONFIG_NETWORK_SUPPORTED
    {
        network_t *net = (network_t *)app_main_get_network();
        static const char *const ifaces[] = {
            "vnet0", "wlan0", "wlan1", "eth0", "usb0", NULL
        };
        int i;

        if (net == NULL)
            return;

        if (net->wifi && net->wifi->station) {
            network_wifi_station_state_e state;

            if (net->wifi->station->get_state(&state) == NETWORK_OK &&
                state == NETWORK_WIFI_STATION_STATE_CONNECTED &&
                net->wifi->station->get_ip_address(ip, len) == NETWORK_OK &&
                ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
                return;
            ip[0] = '\0';
        }

        if (net->ethernet && net->ethernet->get_ip) {
            if (net->ethernet->get_ip(ip) == NETWORK_OK &&
                ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
                return;
            ip[0] = '\0';
        }

        if (net->ethernet && net->ethernet->get_ip_by_name) {
            for (i = 0; ifaces[i] != NULL; i++) {
                if (net->ethernet->get_ip_by_name(ifaces[i], ip, len) == NETWORK_OK &&
                    ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
                    return;
                ip[0] = '\0';
            }
        }
    }
#endif
}

static lv_font_t *dashcam_font(int size)
{
    lv_font_t *f = gui_font_get_cjk_font(size);

    if (!f)
        f = (lv_font_t *)&lv_font_montserrat_14;
    return f;
}

static lv_obj_t *dashcam_make_label(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_style_text_font(label, dashcam_font(size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void dashcam_ui_refresh(lv_timer_t *timer);

/* Start NV12 preview on the LVGL thread once record/RTSP bring-up finishes. */
static void dashcam_ui_preview_kick(lv_timer_t *timer)
{
    (void)timer;

    if (!s_ui.created)
        return;

    if (s_ui.preview_started)
        goto done;

    if (dashcam_core_is_starting())
        return;

    if (!dashcam_core_is_running()) {
        log_warn("dashcam: preview kick aborted (pipeline not running)");
        goto done;
    }

    if (!dashcam_preview_available())
        goto done;

    if (dashcam_preview_start() != 0) {
        log_warn("dashcam: preview start failed");
        return; /* retry on next tick */
    }

    s_ui.preview_started = 1;
    if (s_ui.datetime_label)
        lv_obj_move_foreground(s_ui.datetime_label);
    log_info("dashcam: preview kicked on LVGL thread");

done:
    if (s_ui.preview_kick_timer) {
        lv_timer_delete(s_ui.preview_kick_timer);
        s_ui.preview_kick_timer = NULL;
    }
}

/* Bubble touch so launcher luna_input still sees swipe-up to exit. */
static void dashcam_enable_event_bubble(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t n;

    if (!obj)
        return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    n = lv_obj_get_child_count(obj);
    for (i = 0; i < n; i++)
        dashcam_enable_event_bubble(lv_obj_get_child(obj, (int32_t)i));
}

static void dashcam_format_hms(char *buf, size_t len, int total_s)
{
    int h;
    int m;
    int s;

    if (!buf || len == 0)
        return;
    if (total_s < 0)
        total_s = 0;
    h = total_s / 3600;
    m = (total_s % 3600) / 60;
    s = total_s % 60;
    snprintf(buf, len, "%02d:%02d:%02d", h, m, s);
}

static void dashcam_format_datetime(char *buf, size_t len)
{
    time_t now;
    struct tm tmv;
    struct tm *tm;

    if (!buf || len == 0)
        return;

    now = time(NULL);
    tm = localtime_r(&now, &tmv);
    if (!tm) {
        snprintf(buf, len, "----/--/-- --:--:--");
        return;
    }
    /* Preview corner OSD: e.g. 2026/07/16 09:54:55 */
    snprintf(buf, len, "%04d/%02d/%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static int dashcam_key_from_keypad(void)
{
    lv_indev_t *indev = lv_indev_active();

    return indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD;
}

static lv_indev_t *dashcam_keypad_indev(void)
{
    lv_indev_t *indev = NULL;

    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
            return indev;
    }
    return NULL;
}

static void dashcam_key_latch_stop_timer(void)
{
    if (s_ui.key_latch_timer) {
        lv_timer_delete(s_ui.key_latch_timer);
        s_ui.key_latch_timer = NULL;
    }
}

static void dashcam_key_unlatch_if_released(void)
{
    lv_indev_t *indev;

    if (!s_key_latched)
        return;

    indev = dashcam_keypad_indev();
    if (indev == NULL || lv_indev_get_state(indev) != LV_INDEV_STATE_RELEASED)
        return;

    s_key_latched = 0;
    dashcam_key_latch_stop_timer();
}

static void dashcam_key_latch_poll(lv_timer_t *timer)
{
    (void)timer;
    dashcam_key_unlatch_if_released();
}

static void dashcam_key_latch_arm(void)
{
    s_key_latched = 1;
    if (s_ui.key_latch_timer == NULL)
        s_ui.key_latch_timer = lv_timer_create(dashcam_key_latch_poll,
                                               DASHCAM_KEY_LATCH_POLL_MS, NULL);
}

static void dashcam_toggle_record(void)
{
    int want_record = dashcam_record_is_paused() ? 1 : 0;

    dashcam_core_set_active(want_record);
    dashcam_ui_refresh(NULL);
}

static void dashcam_on_key(lv_event_t *e)
{
    uint32_t key;

    if (!s_ui.created || lv_event_get_code(e) != LV_EVENT_KEY)
        return;

    key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        (void)wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_UP);
        return;
    }
    if (!dashcam_core_is_running())
        return;

    /* Same path as enter(116): keypad LV_EVENT_KEY only (not touch). */
    if (key != LV_KEY_ENTER && key != LV_KEY_HOME)
        return;
    if (!dashcam_key_from_keypad())
        return;

    /*
     * LVGL re-sends non-ENTER keys (incl. HOME/snap) on long-press-repeat
     * (~100ms). Edge-trigger: act once per physical press, ignore until release.
     */
    if (s_key_latched)
        return;
    dashcam_key_latch_arm();

    if (key == LV_KEY_ENTER) {
        dashcam_toggle_record();
        return;
    }

    /* LV_KEY_HOME ← snap_code (90) */
    if (dashcam_capture_busy()) {
        log_info("dashcam: snap skipped (busy)");
        return;
    }
    if (dashcam_capture_trigger() != 0)
        log_warn("dashcam: snap skipped (trigger failed)");
    else
        log_info("dashcam: snap key (LV_KEY_HOME)");
}

static void dashcam_ui_attach_keypad(lv_obj_t *obj)
{
    lv_group_t *group = lv_group_get_default();
    lv_indev_t *indev = NULL;

    if (!obj)
        return;

    if (!group) {
        group = lv_group_create();
        if (!group)
            return;
        lv_group_set_default(group);
    }

    lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);

    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, group);
            break;
        }
    }
}

static void dashcam_ui_refresh(lv_timer_t *timer)
{
    dashcam_record_status_t st;
    char buf[96];

    (void)timer;
    if (!s_ui.created)
        return;

    dashcam_record_get_status(&st);
    s_ui.blink = !s_ui.blink;

    if (s_ui.datetime_label) {
        dashcam_format_datetime(buf, sizeof(buf));
        lv_label_set_text(s_ui.datetime_label, buf);
        /* Preview image is created after this label; keep time above the frame. */
        lv_obj_move_foreground(s_ui.datetime_label);
    }

    if (s_ui.time_label) {
        dashcam_format_hms(buf, sizeof(buf), st.session_elapsed_s);
        lv_label_set_text(s_ui.time_label, buf);
    }

    if (st.card_missing) {
        if (s_ui.rec_dot)
            lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_DIM), 0);
        if (s_ui.rec_label)
            lv_label_set_text(s_ui.rec_label, dashcam_is_zh() ? "无 T 卡" : "No T card");
    } else if (st.storage_full) {
        if (s_ui.rec_dot) {
            lv_obj_set_style_bg_opa(s_ui.rec_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_REC), 0);
        }
        if (s_ui.rec_label)
            lv_label_set_text(s_ui.rec_label, dashcam_is_zh() ? "存储已满" : "Card full");
    } else if (st.paused || !dashcam_core_is_active()) {
        if (s_ui.rec_dot) {
            lv_obj_set_style_bg_opa(s_ui.rec_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_DIM), 0);
        }
        if (s_ui.rec_label)
            lv_label_set_text(s_ui.rec_label, dashcam_is_zh() ? "已停止" : "Stopped");
    } else if (st.recording) {
        if (s_ui.rec_dot)
            lv_obj_set_style_bg_opa(s_ui.rec_dot,
                                    s_ui.blink ? LV_OPA_COVER : LV_OPA_30, 0);
        if (s_ui.rec_dot)
            lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_REC), 0);
        if (s_ui.rec_label)
            lv_label_set_text(s_ui.rec_label, dashcam_is_zh() ? "录像中" : "REC");
    } else {
        if (s_ui.rec_dot) {
            lv_obj_set_style_bg_opa(s_ui.rec_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_DIM), 0);
        }
        if (s_ui.rec_label)
            lv_label_set_text(s_ui.rec_label, dashcam_is_zh() ? "待机" : "Idle");
    }

    /* RTSP stays up for the whole app session; always show URL / net state. */
    if (s_ui.rtsp_label) {
        char ip[64];

        dashcam_get_ip(ip, sizeof(ip));
        if (ip[0] != '\0')
            snprintf(buf, sizeof(buf), "rtsp://%s:554/vs1", ip);
        else
            snprintf(buf, sizeof(buf), "%s",
                     dashcam_is_zh() ? "RTSP: 无网络" : "RTSP: no network");
        lv_label_set_text(s_ui.rtsp_label, buf);
    }
}

int dashcam_ui_create(lv_obj_t *launcher_main)
{
    lv_obj_t *top;
    lv_obj_t *bottom;
    lv_obj_t *row;

    if (!launcher_main)
        return -1;

    if (s_ui.created)
        dashcam_ui_destroy();

    memset(&s_ui, 0, sizeof(s_ui));
    s_key_latched = 0;

    s_ui.root = lv_obj_create(launcher_main);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_set_size(s_ui.root, lv_pct(100), lv_pct(100));
    lv_obj_center(s_ui.root);
    lv_obj_set_style_bg_color(s_ui.root, lv_color_hex(DASHCAM_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    /* Default lv_obj is CLICKABLE; clear so touch focus/click does not interact
     * with the keypad group. Record toggle is physical ENTER only. */
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* Top band: RTSP */
    top = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), DASHCAM_BAND_H);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.rtsp_label = dashcam_make_label(top, 12, DASHCAM_COLOR_DIM);
    lv_label_set_long_mode(s_ui.rtsp_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_ui.rtsp_label, 232);
    lv_label_set_text(s_ui.rtsp_label, "rtsp://--");
    lv_obj_center(s_ui.rtsp_label);

    /* Centered preview canvas (same size as camera preview window). */
    s_ui.preview_area = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(s_ui.preview_area);
    lv_obj_set_size(s_ui.preview_area, DASHCAM_PREVIEW_W, DASHCAM_PREVIEW_H);
    lv_obj_align(s_ui.preview_area, LV_ALIGN_TOP_MID, 0, DASHCAM_BAND_H);
    lv_obj_set_style_bg_color(s_ui.preview_area, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(s_ui.preview_area, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ui.preview_area, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.datetime_label = dashcam_make_label(s_ui.preview_area, 12, DASHCAM_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.datetime_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_ui.datetime_label, "----/--/-- --:--:--");
    lv_obj_align(s_ui.datetime_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    /* Bottom band: record status + duration */
    bottom = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_size(bottom, lv_pct(100), DASHCAM_BAND_H);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    row = lv_obj_create(bottom);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.rec_dot = lv_obj_create(row);
    lv_obj_remove_style_all(s_ui.rec_dot);
    lv_obj_set_size(s_ui.rec_dot, 10, 10);
    lv_obj_set_style_radius(s_ui.rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.rec_dot, lv_color_hex(DASHCAM_COLOR_DIM), 0);
    lv_obj_set_style_bg_opa(s_ui.rec_dot, LV_OPA_COVER, 0);

    s_ui.rec_label = dashcam_make_label(row, 14, DASHCAM_COLOR_TEXT);
    lv_label_set_text(s_ui.rec_label, "--");

    s_ui.time_label = dashcam_make_label(row, 14, DASHCAM_COLOR_ACCENT);
    lv_label_set_text(s_ui.time_label, "00:00:00");

    s_ui.created = 1;

    dashcam_enable_event_bubble(s_ui.root);
    lv_obj_add_event_cb(s_ui.root, dashcam_on_key, LV_EVENT_KEY, NULL);
    dashcam_ui_attach_keypad(s_ui.root);

    /* Preview LVGL objects must be created on this thread (not the core worker). */
    if (dashcam_preview_available() &&
        dashcam_preview_init(s_ui.preview_area) != 0)
        log_warn("dashcam: preview init failed");

    if (dashcam_core_start(s_ui.preview_area) != 0)
        log_warn("dashcam: core start failed");

    if (s_ui.datetime_label)
        lv_obj_move_foreground(s_ui.datetime_label);

    s_ui.preview_kick_timer = lv_timer_create(dashcam_ui_preview_kick, 50, NULL);
    s_ui.timer = lv_timer_create(dashcam_ui_refresh, 1000, NULL);
    dashcam_ui_refresh(NULL);

    log_info("dashcam UI created (preview %dx%d, ENTER=record HOME=snap)",
             DASHCAM_PREVIEW_W, DASHCAM_PREVIEW_H);
    return 0;
}

APP_ENTRY_EXPORT void dashcam_ui_destroy(void)
{
    lv_obj_t *home_root;

    if (!s_ui.created)
        return;

    log_info("dashcam UI destroy (flush record + release pipeline)");

    if (s_ui.preview_kick_timer) {
        lv_timer_delete(s_ui.preview_kick_timer);
        s_ui.preview_kick_timer = NULL;
    }

    if (s_ui.timer) {
        lv_timer_delete(s_ui.timer);
        s_ui.timer = NULL;
    }

    dashcam_key_latch_stop_timer();
    s_key_latched = 0;

    /* Blocks until current MP4 segment is finalized and RTSP/preview stop. */
    dashcam_core_stop();
    s_ui.preview_started = 0;

    if (s_ui.root && lv_obj_is_valid(s_ui.root)) {
        lv_group_t *group = lv_group_get_default();

        if (group)
            lv_group_remove_obj(s_ui.root);
        lv_obj_delete(s_ui.root);
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_key_latched = 0;

    /* Return keypad focus to launcher so ESC/back keep working on home. */
    home_root = wearable_home_get_root();
    if (home_root)
        dashcam_ui_attach_keypad(home_root);

    log_info("dashcam UI destroyed");
}

static void (*const s_keep_dashcam_ui_destroy)(void) = dashcam_ui_destroy;
