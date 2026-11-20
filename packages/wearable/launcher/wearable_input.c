// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Touch swipe / tap forwarding for watch-style launcher.
 *
 * One contact → exactly one gesture, decided at RELEASED from cumulative
 * displacement of accepted samples. The KM01W panel flickers DOWN/UP and
 * teleports (often to 0,0); those samples are ignored so taps do not become
 * phantom swipes and mid-press noise does not abort a horizontal flip.
 */
#define LOG_TAG "WEARABLE_INPUT"

#include <stdlib.h>

#include "app_product_config.h"
#include "gui_shell.h"
#include "input_keymap.h"
#include "wearable_home.h"
#include "wearable_input.h"
#include "log.h"

/* Tap slop must stay below swipe threshold so the two never overlap.
 * Ambiguous mid-range (slop < move < swipe) must NOT become TAP — that was
 * turning short page-flips into accidental app launches. */
#define WEARABLE_TAP_SLOP_PX         12
#define WEARABLE_SWIPE_THRESHOLD_PX  20
/* Reject one-sample jumps larger than this (ghost second track / lift glitch).
 * Must stay above a normal 40ms swipe step on 240px panel (~50–70). */
#define WEARABLE_TELEPORT_PX         80
/* After a swipe, suppress app-local taps briefly (contact flicker at zone edges). */
#define WEARABLE_SWIPE_TAP_SUPPRESS_MS 400
/* If only ghost samples arrive after a real stroke, finalize from peak. */
#define WEARABLE_GHOST_STUCK_MS        160

typedef enum {
    WEARABLE_GESTURE_NONE = 0,
    WEARABLE_GESTURE_TAP,
    WEARABLE_GESTURE_SWIPE_LEFT,
    WEARABLE_GESTURE_SWIPE_RIGHT,
    WEARABLE_GESTURE_SWIPE_UP,
} wearable_gesture_kind_t;

static lv_point_t s_press_point;
static lv_point_t s_last_good;
static int s_press_valid;
static int s_have_good;
static int s_horizontal_swipe;
static int s_swipe_up_fired;
static wearable_gesture_kind_t s_latched;
static int s_peak_dx;
static int s_peak_dy;
static int s_peak_mag; /* max |dx|+|dy| from a single sample (paired peaks) */
static uint32_t s_swipe_suppress_until;
static int s_drop_log_burst; /* rate-limit teleport drop spam */
static uint32_t s_drop_since_tick;

static void wearable_input_attach_keypad_group(lv_obj_t *root);
static void wearable_input_key_cb(lv_event_t *e);
static void wearable_input_mark_swipe_suppress(void);
static void wearable_input_classify(int dx, int dy);
static void wearable_input_emit_latched(void);
static void wearable_input_event_cb(lv_event_t *e);
static int wearable_input_point_ok(const lv_point_t *p, const lv_point_t *prev, int have_prev);

static void wearable_input_attach_keypad_group(lv_obj_t *root)
{
    lv_indev_t *indev = NULL;

    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_group_t *group;

        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) {
            continue;
        }

        group = lv_group_get_default();
        if (!group) {
            group = lv_group_create();
            lv_group_set_default(group);
        }
        lv_group_add_obj(group, root);
        lv_indev_set_group(indev, group);
        break;
    }
}

static const char *wearable_input_gesture_name(wearable_gesture_kind_t kind)
{
    switch (kind) {
    case WEARABLE_GESTURE_TAP: return "TAP";
    case WEARABLE_GESTURE_SWIPE_LEFT: return "SWIPE_LEFT";
    case WEARABLE_GESTURE_SWIPE_RIGHT: return "SWIPE_RIGHT";
    case WEARABLE_GESTURE_SWIPE_UP: return "SWIPE_UP";
    default: return "NONE";
    }
}

static int wearable_input_point_ok(const lv_point_t *p, const lv_point_t *prev, int have_prev)
{
    int jump;

    if (!p)
        return 0;

    /* Origin / top-edge during a contact is the KM01W ghost track. */
    if (p->y <= 8) {
        if (!have_prev || prev->y > 16 || (p->x == 0 && p->y == 0))
            return 0;
    }

    if (!have_prev || !prev)
        return 1;

    jump = abs(p->x - prev->x) + abs(p->y - prev->y);
    if (jump > WEARABLE_TELEPORT_PX)
        return 0;
    return 1;
}

static void wearable_input_log_drop(const char *phase, const lv_point_t *p)
{
    /* One info line per press; the rest stay debug (panel can spam ~25Hz). */
    if (s_drop_log_burst == 0) {
        log_debug("touch %s drop xy=%d,%d (teleport/ghost; further drops suppressed)",
                 phase, p->x, p->y);
        s_drop_log_burst = 1;
    } else {
        log_debug("touch %s drop xy=%d,%d", phase, p->x, p->y);
    }
}

static void wearable_input_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    const char *action = "ignore";

    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    /*
     * KM01W talk_key (linux 116) → LV_KEY_ENTER via settings.json input.keys.
     * LVGL keypad also synthesizes PRESSED/PRESSING/RELEASED on the focused
     * object for ENTER; those must NOT be treated as touch (see event_cb).
     */
    switch (key) {
    case LV_KEY_ENTER:
        action = "TAP(enter-app)";
        wearable_home_on_gesture(GUI_SHELL_GESTURE_TAP);
        break;
    case LV_KEY_LEFT:
        action = "SWIPE_RIGHT(prev)";
        wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_RIGHT);
        break;
    case LV_KEY_RIGHT:
        action = "SWIPE_LEFT(next)";
        wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_LEFT);
        break;
    case LV_KEY_ESC:
        action = "SWIPE_UP(exit)";
        wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_UP);
        break;
    default:
        break;
    }
    log_debug("key_cb lv_key=%u action=%s", (unsigned)key, action);
}

/**
 * @brief Classify gesture from pointer displacement relative to touch-down.
 *        Axis with larger absolute displacement wins. Peak is updated from the
 *        same sample (not independent axes) so vertical flicker cannot invent a
 *        diagonal that cancels a real horizontal page-flip.
 */
static void wearable_input_update_peak(int dx, int dy)
{
    int mag = abs(dx) + abs(dy);

    if (mag > s_peak_mag) {
        s_peak_mag = mag;
        s_peak_dx = dx;
        s_peak_dy = dy;
    }
}

static void wearable_input_classify(int dx, int dy)
{
    int adx = abs(dx);
    int ady = abs(dy);

    wearable_input_update_peak(dx, dy);

    /* Horizontal launcher: on near-ties prefer left/right over up. */
    if (adx >= ady && adx >= WEARABLE_SWIPE_THRESHOLD_PX) {
        s_latched = (dx < 0) ? WEARABLE_GESTURE_SWIPE_LEFT
                             : WEARABLE_GESTURE_SWIPE_RIGHT;
        return;
    }
    if (ady > adx && dy < -WEARABLE_SWIPE_THRESHOLD_PX) {
        s_latched = WEARABLE_GESTURE_SWIPE_UP;
        return;
    }

    if (adx <= WEARABLE_TAP_SLOP_PX && ady <= WEARABLE_TAP_SLOP_PX)
        s_latched = WEARABLE_GESTURE_TAP;
    else
        /* Dragged, but not far enough for a swipe — ignore (do not launch). */
        s_latched = WEARABLE_GESTURE_NONE;
}

static void wearable_input_mark_swipe_suppress(void)
{
    s_swipe_suppress_until = lv_tick_get() + WEARABLE_SWIPE_TAP_SUPPRESS_MS;
}

static void wearable_input_emit_latched(void)
{
    log_debug("gesture emit %s hor=%d peak=(%d,%d)",
             wearable_input_gesture_name(s_latched), s_horizontal_swipe,
             s_peak_dx, s_peak_dy);

    switch (s_latched) {
    case WEARABLE_GESTURE_SWIPE_UP:
        if (!s_swipe_up_fired) {
            s_swipe_up_fired = 1;
            wearable_input_mark_swipe_suppress();
            wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_UP);
        }
        break;
    case WEARABLE_GESTURE_SWIPE_LEFT:
        s_horizontal_swipe = 1;
        wearable_input_mark_swipe_suppress();
        wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_LEFT);
        break;
    case WEARABLE_GESTURE_SWIPE_RIGHT:
        s_horizontal_swipe = 1;
        wearable_input_mark_swipe_suppress();
        wearable_home_on_gesture(GUI_SHELL_GESTURE_SWIPE_RIGHT);
        break;
    case WEARABLE_GESTURE_TAP:
        wearable_home_on_gesture(GUI_SHELL_GESTURE_TAP);
        break;
    case WEARABLE_GESTURE_NONE:
    default:
        break;
    }
}

static void wearable_input_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev;
    lv_point_t point;
    lv_indev_type_t indev_type;

    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) {
        return;
    }

    indev = lv_indev_active();
    if (!indev) {
        return;
    }
    /*
     * Keypad ENTER synthesizes PRESSED/PRESSING/RELEASED on the focused
     * object (see lv_indev keypad path). Those events share this callback via
     * the root group; lv_indev_get_point() on a keypad is meaningless and was
     * turning talk_key clicks into phantom horizontal swipes / page flips.
     * Physical keys are handled only in wearable_input_key_cb.
     */
    indev_type = lv_indev_get_type(indev);
    if (indev_type != LV_INDEV_TYPE_POINTER) {
        log_debug("touch_cb skip non-pointer indev_type=%d code=%d",
                 (int)indev_type, (int)code);
        return;
    }

    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        if (s_press_valid)
            return;
        if (!wearable_input_point_ok(&point, NULL, 0)) {
            wearable_input_log_drop("DOWN", &point);
            return;
        }
        s_press_point = point;
        s_last_good = point;
        s_press_valid = 1;
        s_have_good = 1;
        s_horizontal_swipe = 0;
        s_swipe_up_fired = 0;
        s_latched = WEARABLE_GESTURE_TAP;
        s_peak_dx = 0;
        s_peak_dy = 0;
        s_peak_mag = 0;
        s_drop_log_burst = 0;
        s_drop_since_tick = 0;
        log_debug("touch DOWN xy=%d,%d", point.x, point.y);
        return;
    }

    if (!s_press_valid) {
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        int dx;
        int dy;

        if (!wearable_input_point_ok(&point, &s_last_good, s_have_good)) {
            wearable_input_log_drop("PRESSING", &point);
            if (s_drop_since_tick == 0)
                s_drop_since_tick = lv_tick_get();
            /* Ghost track after a real swipe: emit from peak so nav isn't stuck
             * waiting for a RELEASED that may never come until indev timeout. */
            if ((lv_tick_get() - s_drop_since_tick) >= WEARABLE_GHOST_STUCK_MS &&
                (abs(s_peak_dx) >= WEARABLE_SWIPE_THRESHOLD_PX ||
                 abs(s_peak_dy) >= WEARABLE_SWIPE_THRESHOLD_PX)) {
                wearable_input_classify(s_peak_dx, s_peak_dy);
                log_debug("touch ghost-stuck finalize peak=(%d,%d) -> %s",
                         s_peak_dx, s_peak_dy,
                         wearable_input_gesture_name(s_latched));
                wearable_input_emit_latched();
                s_press_valid = 0;
                s_have_good = 0;
                s_drop_since_tick = 0;
            }
            return;
        }
        s_drop_since_tick = 0;
        s_last_good = point;
        s_have_good = 1;
        dx = point.x - s_press_point.x;
        dy = point.y - s_press_point.y;
        wearable_input_classify(dx, dy);
        /* Do not emit mid-press: early SWIPE_UP from flicker aborted real
         * left/right swipes and left LAUNCHER with a no-op UP. */
        return;
    }

    /* RELEASED: prefer peak (full stroke); end sample is often a lift glitch. */
    {
        lv_point_t use = s_have_good ? s_last_good : point;
        int dx = use.x - s_press_point.x;
        int dy = use.y - s_press_point.y;
        wearable_gesture_kind_t from_peak;
        wearable_gesture_kind_t from_end;

        /* Peak first: same-sample max stroke survives end flicker / ghost track. */
        wearable_input_classify(s_peak_dx, s_peak_dy);
        from_peak = s_latched;

        if (from_peak == WEARABLE_GESTURE_SWIPE_LEFT ||
            from_peak == WEARABLE_GESTURE_SWIPE_RIGHT ||
            from_peak == WEARABLE_GESTURE_SWIPE_UP) {
            from_end = WEARABLE_GESTURE_NONE;
        } else {
            wearable_input_classify(dx, dy);
            from_end = s_latched;
            /* Peak already saw a drag past tap slop → never promote to TAP.
             * That was the swipe→enter-app bug on short page flips. */
            if (from_peak == WEARABLE_GESTURE_NONE ||
                abs(s_peak_dx) > WEARABLE_TAP_SLOP_PX ||
                abs(s_peak_dy) > WEARABLE_TAP_SLOP_PX) {
                if (s_latched == WEARABLE_GESTURE_TAP)
                    s_latched = WEARABLE_GESTURE_NONE;
            }
            /* Still NONE after end: leave as NONE (no launch). */
        }
        log_debug("touch UP xy=%d,%d use=%d,%d d=(%d,%d) peak=(%d,%d) peak_cls=%s end_cls=%s -> %s",
                 point.x, point.y, use.x, use.y, dx, dy, s_peak_dx, s_peak_dy,
                 wearable_input_gesture_name(from_peak),
                 wearable_input_gesture_name(from_end),
                 wearable_input_gesture_name(s_latched));
        wearable_input_emit_latched();
        s_press_valid = 0;
        s_have_good = 0;
        s_drop_log_burst = 0;
        s_drop_since_tick = 0;
    }
}

int wearable_input_had_horizontal_swipe(void)
{
    return s_horizontal_swipe;
}

int wearable_input_swipe_active(void)
{
    if (s_latched == WEARABLE_GESTURE_SWIPE_UP ||
        s_latched == WEARABLE_GESTURE_SWIPE_LEFT ||
        s_latched == WEARABLE_GESTURE_SWIPE_RIGHT ||
        s_horizontal_swipe ||
        s_swipe_up_fired) {
        return 1;
    }
    if (s_swipe_suppress_until != 0 &&
        lv_tick_get() < s_swipe_suppress_until) {
        return 1;
    }
    return 0;
}

void wearable_input_reset_gesture(void)
{
    s_press_valid = 0;
    s_have_good = 0;
    s_horizontal_swipe = 0;
    s_swipe_up_fired = 0;
    s_latched = WEARABLE_GESTURE_NONE;
    s_peak_dx = 0;
    s_peak_dy = 0;
    s_peak_mag = 0;
    /* Keep s_swipe_suppress_until: a fresh app open after swipe-up must still
     * ignore a phantom tap from contact flicker. */
}

void wearable_input_init(lv_obj_t *root)
{
    if (!root) {
        log_error("wearable_input_init: null root");
        return;
    }

    input_keymap_load(GUI_INPUT_CONFIG_PATH);

    s_press_valid = 0;
    s_have_good = 0;
    s_horizontal_swipe = 0;
    s_swipe_up_fired = 0;
    s_latched = WEARABLE_GESTURE_NONE;
    s_peak_dx = 0;
    s_peak_dy = 0;
    s_peak_mag = 0;
    s_swipe_suppress_until = 0;
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, wearable_input_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, wearable_input_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, wearable_input_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, wearable_input_key_cb, LV_EVENT_KEY, NULL);
    wearable_input_attach_keypad_group(root);
}
