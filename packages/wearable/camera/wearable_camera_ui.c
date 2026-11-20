// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Phone-camera layout on 240x240:
 *   [0..184)  preview (top-aligned, top-crop)
 *   [184..240) [Gallery] | [Shutter] | [Mode label]
 * Mode button only switches display; shutter triggers capture/record.
 */
#define LOG_TAG "LUNA CAMERA UI"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_camera.h"
#include "gui_common.h"
#include "wearable_camera_capture.h"
#include "wearable_camera_gallery.h"
#include "wearable_camera_tfcard.h"
#include "wearable_camera_ui.h"
#include "log.h"
#include "lvgl.h"
#include "system_thread.h"

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

#define WEARABLE_CAM_BOTTOM_H     56
#define WEARABLE_CAM_PREVIEW_H    (240 - WEARABLE_CAM_BOTTOM_H)
/* Exit swipe must beat KM01W teleport noise (often 50–100px in one sample). */
#define WEARABLE_CAM_SWIPE_PX      36
#define WEARABLE_CAM_TELEPORT_PX   100
#define WEARABLE_CAM_SWIPE_ARM_MS 300
#define WEARABLE_CAM_SHUTTER_SZ   44
#define WEARABLE_CAM_SHUTTER_INNER_SZ 34
#define WEARABLE_CAM_SHUTTER_BORDER 3
/* Bottom bar is split into three full-height hit columns (no dead gaps). */
#define WEARABLE_CAM_HIT_COL_W    80

#define CAM_COLOR_BG          0x000000
#define CAM_COLOR_ACTIVE      0xffcc00
#define CAM_COLOR_RECORD      0xff3b30
#define CAM_COLOR_RECORDING   0x34c759
#define CAM_COLOR_BUSY        0x8e8e93
#define CAM_COLOR_GALLERY_BG  0x2c2c2e

typedef enum { WEARABLE_MODE_PHOTO = 0, WEARABLE_MODE_RECORD = 1 } wearable_mode_t;

typedef struct {
    lv_obj_t *launcher_main;
    lv_obj_t *preview_window;
    lv_obj_t *bottom_bar;
    lv_obj_t *record_dot;
    lv_obj_t *lb_record;
    lv_obj_t *btn_gallery;
    lv_obj_t *btn_shutter;
    lv_obj_t *shutter_disc_white;
    lv_obj_t *shutter_disc_red;
    lv_obj_t *shutter_disc_green;
    lv_obj_t *shutter_disc_gray;
    lv_obj_t *btn_mode;
    lv_obj_t *lb_mode;
    lv_obj_t *swipe_zone;
    wearable_mode_t mode;
    int gallery_open;
} wearable_camera_ctx_t;

static wearable_camera_ctx_t s_ctx;
static lv_point_t s_swipe_press;
static lv_point_t s_swipe_last_good;
static int s_swipe_press_valid;
static int s_swipe_have_good;
static int s_swipe_up_fired;
static int s_swipe_peak_dx;
static int s_swipe_peak_dy;
static uint32_t s_swipe_arm_tick;
static uint32_t s_mode_toggle_tick;
static uint32_t s_record_start_tick;
static lv_timer_t *s_record_timer;

/* Ignore mode-toggle clicks arriving within this window of the last accepted
 * toggle. The 5ms defer timer only collapses same-tick bursts; a physical tap
 * can still deliver several CLICKED events tens of ms apart, so latch here like
 * the launcher's tile-nav grace. */
#define WEARABLE_CAM_MODE_TOGGLE_GRACE_MS  400

static void restore_screen_bg(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *screen = disp ? lv_display_get_screen_active(disp) : NULL;

    if (screen)
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

static void return_to_launcher(void)
{
    typedef void (*wearable_home_return_fn_t)(void);
    wearable_home_return_fn_t return_fn;

    return_fn = (wearable_home_return_fn_t)dlsym(RTLD_DEFAULT, "wearable_home_return_to_launcher");
    if (return_fn)
        return_fn();
    else if (s_ctx.launcher_main)
        lv_obj_remove_flag(s_ctx.launcher_main, LV_OBJ_FLAG_HIDDEN);
}

static void exit_camera(void)
{
    if (s_ctx.gallery_open)
        return;
    if (wearable_camera_record_is_active()) {
        show_msgbox_note(get_lan_index() == 1 ? "请先停止录像" : "Stop recording first");
        return;
    }
    wearable_camera_ui_destroy();
    return_to_launcher();
}

/* ---- Safe deferred dispatch: never touch LVGL objects inside event callbacks ---- */
typedef enum { DEFER_EXIT, DEFER_MODE_TOGGLE, DEFER_SHUTTER } defer_action_t;
static defer_action_t s_defer_action;
static lv_timer_t *s_defer_timer;
static char s_shutter_note[96];

/* Worker thread: performs blocking capture/record off the LVGL thread,
 * then signals completion via s_worker_done flag. LVGL polling timer
 * picks up the result and safely restarts preview / shows msgbox. */
static pthread_t s_worker_thread;
static volatile int s_worker_state; /* 0=idle 1=busy 2=done_ok 3=done_fail 4=no_tfcard */
static char s_worker_path[320];

static void show_shutter_note_timer(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_shutter_note[0] != '\0')
        show_msgbox_note(s_shutter_note);
    s_shutter_note[0] = '\0';
}

static void queue_shutter_note(const char *text)
{
    if (!text)
        return;
    snprintf(s_shutter_note, sizeof(s_shutter_note), "%s", text);
}

static void refresh_mode_label_text(void);
static void refresh_shutter_discs(void);
static void refresh_record_status(void);

/* Async 0-delay UI refresh — called from a fresh LVGL timer to avoid re-entry. */
static void async_mode_ui_refresh_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    refresh_mode_label_text();
    refresh_shutter_discs();
    log_info("camera UI: async mode UI refresh done");
}

static void async_shutter_ui_refresh_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    refresh_mode_label_text();
    refresh_shutter_discs();
    refresh_record_status();

    /*
     * Preview is kept alive across photo / record start / stop by the framework now:
     * config_load_video_config no longer reloads (zeroes) the live camera streams_cfg/
     * broadcasts at record start, so the preview broadcast client and HAL stream priv
     * stay valid. No GUI-side preview restart is needed here (restarting would add a
     * visible preview blip on every shutter press). If a future teardown path is found
     * to drop the preview client, recover it at the framework level, not here.
     */

    if (s_shutter_note[0] != '\0')
        lv_timer_create(show_shutter_note_timer, 100, NULL);
}

/*
 * Worker thread: runs blocking framework calls (capture_picture / recorder_start).
 * DOES NOT touch LVGL objects.  LVGL poll timer picks up result.
 */
static void *shutter_worker_thread(void *arg)
{
    (void)arg;

    /* Raw pthread: name it so top -H shows the camera module instead of the
     * inherited "LunaGui" name. */
    (void)thread_set_current_name("cam_shutter");

    if (s_ctx.mode == WEARABLE_MODE_PHOTO) {
        int ret;

        if (!wearable_tfcard_is_ready()) {
            s_worker_state = 4;
        } else {
            ret = wearable_camera_capture_to_tfcard(s_worker_path, sizeof(s_worker_path));
            if (ret == WEARABLE_TFCARD_ERR_NOT_READY)
                s_worker_state = 4;
            else
                s_worker_state = (ret == 0) ? 2 : 3;
        }
    } else if (wearable_camera_record_is_active()) {
        wearable_camera_record_stop();
        s_worker_state = 2;
    } else {
        int ret;

        if (!wearable_tfcard_is_ready()) {
            s_worker_state = 4;
        } else {
            ret = wearable_camera_record_start();
            if (ret == WEARABLE_TFCARD_ERR_NOT_READY)
                s_worker_state = 4;
            else
                s_worker_state = (ret == 0) ? 2 : 3;
        }
    }
    log_info("worker: exit mode=%d state=%d", s_ctx.mode, s_worker_state);
    return NULL;
}

/*
 * LVGL timer: polls s_worker_state and shows result note on LVGL thread.
 * Preview sub-channel is not torn down; no stream restart after photo/record.
 */
static void shutter_poll_cb(lv_timer_t *t)
{
    if (s_worker_state == 1) /* still busy */
        return;

    lv_timer_delete(t);
    log_info("poll: done state=%d mode=%d", s_worker_state, s_ctx.mode);

    if (s_worker_state == 2) {
        if (s_ctx.mode == WEARABLE_MODE_PHOTO)
            queue_shutter_note(get_lan_index() == 1 ? "拍照成功" : "Photo saved");
        else if (wearable_camera_record_is_active())
            queue_shutter_note(get_lan_index() == 1 ? "开始录像" : "Recording");
        else
            queue_shutter_note(get_lan_index() == 1 ? "录像已保存" : "Video saved");
    } else if (s_worker_state == 4) {
        wearable_tfcard_show_missing_hint();
    } else {
        if (s_ctx.mode == WEARABLE_MODE_PHOTO)
            queue_shutter_note(get_lan_index() == 1 ? "拍照失败" : "Capture failed");
        else
            queue_shutter_note(get_lan_index() == 1 ? "开始录像失败" : "Failed to start recording");
    }

    lv_timer_create(async_shutter_ui_refresh_cb, 16, NULL);
}

static void defer_cb(lv_timer_t *timer)
{
    s_defer_timer = NULL;
    lv_timer_delete(timer);
    if (s_defer_action == DEFER_EXIT) {
        exit_camera();
        return;
    }
    if (s_defer_action == DEFER_MODE_TOGGLE) {
        if (wearable_camera_record_is_active())
            return;
        s_ctx.mode = (s_ctx.mode == WEARABLE_MODE_PHOTO) ? WEARABLE_MODE_RECORD : WEARABLE_MODE_PHOTO;
        log_info("camera UI: mode toggle -> %s", s_ctx.mode == WEARABLE_MODE_PHOTO ? "photo" : "video");
        /* Defer all LVGL UI updates to a fresh 0-delay timer (avoids LVGL crash). */
        lv_timer_create(async_mode_ui_refresh_cb, 16, NULL);
        return;
    }
    if (s_defer_action == DEFER_SHUTTER) {
        if (s_worker_state == 1)
            return; /* already busy, ignore double-tap */

        /* Sub-channel preview keeps running to LCD; worker only adds main encode/save. */
        s_worker_state = 1;
        refresh_shutter_discs(); /* gray busy feedback immediately */
        if (pthread_create(&s_worker_thread, NULL, shutter_worker_thread, NULL) != 0) {
            s_worker_state = 3;
            refresh_shutter_discs();
        } else {
            pthread_detach(s_worker_thread);
        }
        lv_timer_create(shutter_poll_cb, 50, NULL);
        return;
    }
}

static void defer_action(defer_action_t action)
{
    if (s_defer_timer != NULL)
        return;
    s_defer_action = action;
    s_defer_timer = lv_timer_create(defer_cb, 5, NULL);
}

static void camera_back_cb(lv_obj_t *main, void *user_data)
{
    (void)main;
    (void)user_data;
    defer_action(DEFER_EXIT);
}

/*
 * Pre-built shutter discs — hide/show only (no runtime style changes).
 * Priority: busy(gray) > recording(green) > video idle(red) > photo(white).
 */
static void refresh_shutter_discs(void)
{
    enum { DISC_WHITE = 0, DISC_RED, DISC_GREEN, DISC_GRAY } which;
    lv_obj_t *discs[4];
    int i;

    if (s_worker_state == 1)
        which = DISC_GRAY;
    else if (wearable_camera_record_is_active())
        which = DISC_GREEN;
    else if (s_ctx.mode == WEARABLE_MODE_RECORD)
        which = DISC_RED;
    else
        which = DISC_WHITE;

    discs[DISC_WHITE] = s_ctx.shutter_disc_white;
    discs[DISC_RED] = s_ctx.shutter_disc_red;
    discs[DISC_GREEN] = s_ctx.shutter_disc_green;
    discs[DISC_GRAY] = s_ctx.shutter_disc_gray;

    for (i = 0; i < 4; i++) {
        if (!discs[i])
            continue;
        if (i == (int)which)
            lv_obj_remove_flag(discs[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(discs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_mode_label_text(void)
{
    if (!s_ctx.lb_mode)
        return;

    lv_label_set_text(s_ctx.lb_mode,
                       s_ctx.mode == WEARABLE_MODE_PHOTO ? "Photo" : "Video");
}

static void refresh_record_status(void)
{
    char buf[20];

    if (!s_ctx.record_dot)
        return;

    if (!wearable_camera_record_is_active()) {
        lv_obj_add_flag(s_ctx.record_dot, LV_OBJ_FLAG_HIDDEN);
        s_record_start_tick = 0;
        return;
    }

    if (s_record_start_tick == 0)
        s_record_start_tick = lv_tick_get();

    lv_obj_remove_flag(s_ctx.record_dot, LV_OBJ_FLAG_HIDDEN);

    if (!s_ctx.lb_record)
        return;

    {
        uint32_t elapsed_ms = lv_tick_get() - s_record_start_tick;
        int sec = (int)(elapsed_ms / 1000U);

        snprintf(buf, sizeof(buf), "REC %02d:%02d", sec / 60, sec % 60);
        lv_label_set_text(s_ctx.lb_record, buf);
    }
}

static void record_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_record_status();
}

static void event_mode_toggle(lv_event_t *e)
{
    uint32_t now = lv_tick_get();

    (void)e;
    if (s_mode_toggle_tick != 0 &&
        now - s_mode_toggle_tick < WEARABLE_CAM_MODE_TOGGLE_GRACE_MS) {
        log_debug("camera UI: mode toggle ignored (grace %ums)",
                 (unsigned)(now - s_mode_toggle_tick));
        return;
    }
    s_mode_toggle_tick = now;
    defer_action(DEFER_MODE_TOGGLE);
}

static void event_action(lv_event_t *e)
{
    (void)e;
    defer_action(DEFER_SHUTTER);
}

static void on_gallery_closed(void *user_data)
{
    (void)user_data;
    s_ctx.gallery_open = 0;
}

static void event_gallery(lv_event_t *e)
{
    wearable_gallery_type_t tab;

    (void)e;
    if (!s_ctx.preview_window || wearable_camera_record_is_active())
        return;
    tab = (s_ctx.mode == WEARABLE_MODE_RECORD) ? WEARABLE_GALLERY_VIDEO : WEARABLE_GALLERY_PHOTO;
    s_ctx.gallery_open = 1;
    wearable_camera_gallery_show(s_ctx.preview_window, tab, on_gallery_closed, NULL);
}

static int camera_swipe_point_ok(const lv_point_t *p, const lv_point_t *prev, int have_prev)
{
    int jump;

    if (!p)
        return 0;
    if (p->x == 0 && p->y == 0) {
        if (!have_prev || prev->x != 0 || prev->y != 0)
            return 0;
    }
    if (!have_prev || !prev)
        return 1;
    jump = abs(p->x - prev->x) + abs(p->y - prev->y);
    return jump <= WEARABLE_CAM_TELEPORT_PX;
}

static void camera_swipe_track(const lv_point_t *point)
{
    int dx;
    int dy;

    if (!camera_swipe_point_ok(point, &s_swipe_last_good, s_swipe_have_good))
        return;

    s_swipe_last_good = *point;
    s_swipe_have_good = 1;
    dx = point->x - s_swipe_press.x;
    dy = point->y - s_swipe_press.y;
    /* Keep the strongest upward displacement (most negative dy), not just
     * max |dy| — a later bounce down must not erase the exit stroke. */
    if (dy < s_swipe_peak_dy)
        s_swipe_peak_dy = dy;
    if (abs(dx) > abs(s_swipe_peak_dx))
        s_swipe_peak_dx = dx;
}

static void camera_swipe_try_exit(int dx, int dy)
{
    int adx = abs(dx);
    int ady = abs(dy);

    if (s_swipe_up_fired)
        return;
    if (lv_tick_get() < s_swipe_arm_tick) {
        log_debug("camera UI: swipe-up ignored (arm)");
        return;
    }
    /*
     * KM01W often drifts to x=0 while swiping up, so |dx| can rival |dy|.
     * Accept any clear upward stroke; only reject when horizontal clearly wins.
     */
    if (dy >= 0 || ady < WEARABLE_CAM_SWIPE_PX) {
        log_debug("camera UI: swipe-up reject d=(%d,%d) (need up>=%d)",
                 dx, dy, WEARABLE_CAM_SWIPE_PX);
        return;
    }
    if (adx > ady + (WEARABLE_CAM_SWIPE_PX / 2)) {
        log_debug("camera UI: swipe-up reject d=(%d,%d) (horizontal)", dx, dy);
        return;
    }

    log_debug("camera UI: swipe-up exit d=(%d,%d) peak=(%d,%d)",
             dx, dy, s_swipe_peak_dx, s_swipe_peak_dy);
    s_swipe_up_fired = 1;
    defer_action(DEFER_EXIT);
}

static void event_swipe_zone(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev;
    lv_point_t point;

    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
        return;
    indev = lv_indev_active();
    if (!indev)
        return;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        if (!camera_swipe_point_ok(&point, NULL, 0))
            return;
        s_swipe_press = point;
        s_swipe_last_good = point;
        s_swipe_press_valid = 1;
        s_swipe_have_good = 1;
        s_swipe_up_fired = 0;
        s_swipe_peak_dx = 0;
        s_swipe_peak_dy = 0;
        return;
    }
    if (!s_swipe_press_valid)
        return;
    if (code == LV_EVENT_PRESSING) {
        camera_swipe_track(&point);
        /* Early exit once the stroke is clearly upward — release often lands
         * on a ghost (0,*) sample that used to fail the old dominance check. */
        if (!s_swipe_up_fired &&
            s_swipe_peak_dy <= -WEARABLE_CAM_SWIPE_PX) {
            camera_swipe_try_exit(s_swipe_peak_dx, s_swipe_peak_dy);
            if (s_swipe_up_fired)
                s_swipe_press_valid = 0;
        }
        return;
    }

    camera_swipe_track(&point);
    if (!s_swipe_up_fired) {
        if (s_swipe_peak_dy <= -WEARABLE_CAM_SWIPE_PX ||
            abs(s_swipe_peak_dx) >= WEARABLE_CAM_SWIPE_PX)
            camera_swipe_try_exit(s_swipe_peak_dx, s_swipe_peak_dy);
        else if (s_swipe_have_good)
            camera_swipe_try_exit(s_swipe_last_good.x - s_swipe_press.x,
                                  s_swipe_last_good.y - s_swipe_press.y);
    }
    s_swipe_press_valid = 0;
    s_swipe_have_good = 0;
}

static void create_swipe_zone(lv_obj_t *parent)
{
    lv_obj_t *zone = lv_obj_create(parent);

    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, 240, WEARABLE_CAM_PREVIEW_H);
    lv_obj_align(zone, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_0, 0);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(zone, event_swipe_zone, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(zone, event_swipe_zone, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(zone, event_swipe_zone, LV_EVENT_RELEASED, NULL);
    s_ctx.swipe_zone = zone;
}

static int s_init_step;

static void create_controls_bottom_bar(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(CAM_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s_ctx.record_dot = lv_obj_create(parent);
    lv_obj_set_size(s_ctx.record_dot, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_ctx.record_dot, 6, 0);
    lv_obj_set_style_pad_ver(s_ctx.record_dot, 3, 0);
    lv_obj_align(s_ctx.record_dot, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_radius(s_ctx.record_dot, 4, 0);
    lv_obj_set_style_bg_color(s_ctx.record_dot, lv_color_hex(CAM_COLOR_RECORD), 0);
    lv_obj_set_style_bg_opa(s_ctx.record_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ctx.record_dot, 0, 0);
    lv_obj_remove_flag(s_ctx.record_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ctx.record_dot, LV_OBJ_FLAG_CLICKABLE);
    s_ctx.lb_record = lv_label_create(s_ctx.record_dot);
    lv_label_set_text(s_ctx.lb_record, "REC 00:00");
    lv_obj_set_style_text_color(s_ctx.lb_record, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_ctx.lb_record, &lv_font_montserrat_12, 0);
    lv_obj_center(s_ctx.lb_record);
    lv_obj_add_flag(s_ctx.record_dot, LV_OBJ_FLAG_HIDDEN);

    if (!s_record_timer)
        s_record_timer = lv_timer_create(record_status_timer_cb, 500, NULL);

    s_ctx.bottom_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ctx.bottom_bar);
    lv_obj_set_size(s_ctx.bottom_bar, lv_pct(100), WEARABLE_CAM_BOTTOM_H);
    lv_obj_align(s_ctx.bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ctx.bottom_bar, lv_color_hex(CAM_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ctx.bottom_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ctx.bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_controls_gallery(void)
{
    lv_obj_t *gallery_icon;

    /* Left third of bottom bar — full column hit target. */
    s_ctx.btn_gallery = lv_button_create(s_ctx.bottom_bar);
    lv_obj_remove_style_all(s_ctx.btn_gallery);
    lv_obj_set_size(s_ctx.btn_gallery, WEARABLE_CAM_HIT_COL_W, WEARABLE_CAM_BOTTOM_H);
    lv_obj_align(s_ctx.btn_gallery, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ctx.btn_gallery, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.btn_gallery, 0, 0);
    lv_obj_set_style_shadow_width(s_ctx.btn_gallery, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.btn_gallery, 0, 0);
    lv_obj_add_event_cb(s_ctx.btn_gallery, event_gallery, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ctx.btn_gallery, event_gallery, LV_EVENT_LONG_PRESSED, NULL);

    gallery_icon = lv_obj_create(s_ctx.btn_gallery);
    lv_obj_set_size(gallery_icon, 40, 40);
    lv_obj_align(gallery_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(gallery_icon, 6, 0);
    lv_obj_set_style_bg_color(gallery_icon, lv_color_hex(CAM_COLOR_GALLERY_BG), 0);
    lv_obj_set_style_bg_opa(gallery_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gallery_icon, 1, 0);
    lv_obj_set_style_border_color(gallery_icon, lv_color_hex(0x48484a), 0);
    lv_obj_clear_flag(gallery_icon, LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t *sym = lv_label_create(gallery_icon);
        lv_label_set_text(sym, LV_SYMBOL_IMAGE);
        lv_obj_set_style_text_color(sym, lv_color_hex(0xffffff), 0);
        lv_obj_center(sym);
    }
}

static lv_obj_t *create_shutter_disc(lv_obj_t *parent, lv_color_t fill)
{
    lv_obj_t *disc = lv_obj_create(parent ? parent : s_ctx.btn_shutter);

    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, WEARABLE_CAM_SHUTTER_INNER_SZ, WEARABLE_CAM_SHUTTER_INNER_SZ);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, fill, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(disc, LV_ALIGN_CENTER, 0, 0);
    return disc;
}

static void create_controls_shutter(void)
{
    /* Center third — wide hit column; ring drawn centered inside. */
    s_ctx.btn_shutter = lv_button_create(s_ctx.bottom_bar);
    lv_obj_remove_style_all(s_ctx.btn_shutter);
    lv_obj_set_size(s_ctx.btn_shutter, WEARABLE_CAM_HIT_COL_W, WEARABLE_CAM_BOTTOM_H);
    lv_obj_align(s_ctx.btn_shutter, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.btn_shutter, 0, 0);
    lv_obj_set_style_bg_opa(s_ctx.btn_shutter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.btn_shutter, 0, 0);
    lv_obj_set_style_shadow_width(s_ctx.btn_shutter, 0, 0);
    lv_obj_add_event_cb(s_ctx.btn_shutter, event_action, LV_EVENT_CLICKED, NULL);
    /* Finger often rests >400ms; LVGL then skips CLICKED and only sends LONG_PRESSED. */
    lv_obj_add_event_cb(s_ctx.btn_shutter, event_action, LV_EVENT_LONG_PRESSED, NULL);

    {
        lv_obj_t *ring = lv_obj_create(s_ctx.btn_shutter);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, WEARABLE_CAM_SHUTTER_SZ, WEARABLE_CAM_SHUTTER_SZ);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, WEARABLE_CAM_SHUTTER_BORDER, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0xffffff), 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

        s_ctx.shutter_disc_white = create_shutter_disc(ring, lv_color_hex(0xffffff));
        s_ctx.shutter_disc_red = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_RECORD));
        s_ctx.shutter_disc_green = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_RECORDING));
        s_ctx.shutter_disc_gray = create_shutter_disc(ring, lv_color_hex(CAM_COLOR_BUSY));
        lv_obj_add_flag(s_ctx.shutter_disc_red, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.shutter_disc_green, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.shutter_disc_gray, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_controls_mode_label(void)
{
    /* Right third — full column hit target. */
    s_ctx.btn_mode = lv_button_create(s_ctx.bottom_bar);
    lv_obj_remove_style_all(s_ctx.btn_mode);
    lv_obj_set_size(s_ctx.btn_mode, WEARABLE_CAM_HIT_COL_W, WEARABLE_CAM_BOTTOM_H);
    lv_obj_align(s_ctx.btn_mode, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ctx.btn_mode, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.btn_mode, 0, 0);
    lv_obj_set_style_shadow_width(s_ctx.btn_mode, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.btn_mode, 0, 0);
    lv_obj_add_event_cb(s_ctx.btn_mode, event_mode_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ctx.btn_mode, event_mode_toggle, LV_EVENT_LONG_PRESSED, NULL);

    s_ctx.lb_mode = lv_label_create(s_ctx.btn_mode);
    lv_label_set_text(s_ctx.lb_mode, "Photo");
    lv_obj_set_width(s_ctx.lb_mode, WEARABLE_CAM_HIT_COL_W);
    lv_label_set_long_mode(s_ctx.lb_mode, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_ctx.lb_mode, lv_color_hex(CAM_COLOR_ACTIVE), 0);
    lv_obj_set_style_text_align(s_ctx.lb_mode, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ctx.lb_mode, LV_ALIGN_CENTER, 0, 0);
}

static void camera_init_step_timer(lv_timer_t *timer)
{
    lv_obj_t *parent = s_ctx.preview_window;

    if (!parent) {
        lv_timer_delete(timer);
        return;
    }

    switch (s_init_step) {
    case 0:
        create_swipe_zone(parent);
        break;
    case 1:
        create_controls_bottom_bar(parent);
        break;
    case 2:
        create_controls_gallery();
        break;
    case 3:
        create_controls_shutter();
        break;
    case 4:
        create_controls_mode_label();
        /* Preview image is created first (non-clickable). Raise swipe above it,
         * then keep bottom chrome on top for button hits. */
        if (s_ctx.swipe_zone)
            lv_obj_move_foreground(s_ctx.swipe_zone);
        if (s_ctx.bottom_bar)
            lv_obj_move_foreground(s_ctx.bottom_bar);
        s_swipe_arm_tick = lv_tick_get() + WEARABLE_CAM_SWIPE_ARM_MS;
        break;
    case 5:
        if (gui_camera_start_preview() != 0) {
            log_error("camera UI: start preview failed");
            lv_timer_delete(timer);
            return;
        }
        break;
    case 6:
        gui_camera_refresh_preview();
        lv_timer_delete(timer);
        log_info("camera UI: created");
        return;
    default:
        lv_timer_delete(timer);
        return;
    }

    s_init_step++;
}

static void camera_finish_init_timer(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    if (!s_ctx.preview_window)
        return;

    log_info("camera UI: finish init begin");
    s_init_step = 0;
    {
        lv_timer_t *st = lv_timer_create(camera_init_step_timer, 16, NULL);
        if (st)
            lv_timer_set_repeat_count(st, 7);
    }
}

int wearable_camera_ui_create(lv_obj_t *launcher_main)
{
    gui_camera_opts_t opts;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.launcher_main = launcher_main;
    s_ctx.mode = WEARABLE_MODE_PHOTO;
    s_swipe_arm_tick = lv_tick_get() + WEARABLE_CAM_SWIPE_ARM_MS;
    s_mode_toggle_tick = 0;
    s_record_start_tick = 0;

    (void)wearable_tfcard_ensure_init();

    /* Black screen immediately — avoids launcher flash during transition. */
    if (launcher_main) {
        lv_obj_set_style_bg_color(launcher_main, lv_color_hex(CAM_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(launcher_main, LV_OPA_COVER, 0);
    }

    memset(&opts, 0, sizeof(opts));
    opts.on_back = camera_back_cb;
    opts.out_window = &s_ctx.preview_window;
    opts.hide_top_bar = 1;
    opts.preview_cover = 0;
    opts.preview_align_top = 1;
    opts.preview_display_w = 240;
    opts.preview_display_h = WEARABLE_CAM_PREVIEW_H;
    opts.preview_y_offset = 0;
    opts.preview_rgb565 = 1;
    opts.preview_nv21 = 0;

    if (gui_camera_create_ex(launcher_main, &opts) != LV_RESULT_OK)
        return -1;
    if (!s_ctx.preview_window)
        return -1;

    lv_obj_set_style_bg_color(s_ctx.preview_window, lv_color_hex(CAM_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ctx.preview_window, LV_OPA_COVER, 0);

    {
        lv_timer_t *t = lv_timer_create(camera_finish_init_timer, 1, NULL);
        if (t)
            lv_timer_set_repeat_count(t, 1);
    }
    log_info("camera UI create: scheduled finish init");
    return 0;
}

APP_ENTRY_EXPORT void wearable_camera_ui_destroy(void)
{
    if (s_defer_timer != NULL) {
        lv_timer_delete(s_defer_timer);
        s_defer_timer = NULL;
    }
    if (s_record_timer != NULL) {
        lv_timer_delete(s_record_timer);
        s_record_timer = NULL;
    }
    s_record_start_tick = 0;
    s_worker_state = 0;
    if (wearable_camera_record_is_active())
        wearable_camera_record_stop();
    wearable_camera_gallery_close();
    gui_camera_set_preview_paused(0);
    gui_camera_shutdown();

    if (s_ctx.preview_window) {
        lv_obj_delete(s_ctx.preview_window);
        s_ctx.preview_window = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    restore_screen_bg();
}

static void (*const s_keep_wearable_camera_ui_destroy)(void) = wearable_camera_ui_destroy;
