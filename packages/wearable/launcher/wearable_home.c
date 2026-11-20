// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna watch-style home launcher: clock face in center, apps on left/right.
 */
#define LOG_TAG "WEARABLE_HOME"

#include "app_globle_config.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wearable_clock.h"
#include "wearable_home.h"
#include "wearable_app_registry.h"
#include "wearable_input.h"
#include "gui_common.h"
#include "gui_display.h"
#include "gui_font.h"
#include "gui_shell.h"
#include "log.h"
#include "system_thread.h"

typedef int (*wearable_app_entry_fn_t)(lv_obj_t *parent);
typedef void (*wearable_app_destroy_fn_t)(void);

typedef struct {
    void *handle;
    char so_path[512];
    int valid;
    time_t mtime;
    off_t size;
} wearable_app_so_cache_t;

typedef enum {
    WEARABLE_HOME_LAUNCHER,
    WEARABLE_HOME_IN_APP,
} wearable_home_state_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *tileview;
    lv_obj_t *app_root;
    lv_obj_t *dots_cont;
    unsigned int home_index;
    unsigned int active_index;
    unsigned int tile_count;
    unsigned int app_count;
    wearable_home_state_t state;
    char active_app_id[64];
} wearable_home_ctx_t;

static wearable_home_ctx_t s_home;
static gui_shell_app_desc_t s_home_apps[WEARABLE_APP_MAX_DYNAMIC];
static const gui_shell_app_desc_t *s_apps;
static wearable_app_so_cache_t s_so_cache[WEARABLE_APP_MAX_DYNAMIC];
static pthread_mutex_t s_so_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_point_t s_app_tile_press;
static int s_app_tile_press_valid;
static int s_app_tile_suppress_launch;
static uint32_t s_app_enter_tick;
static uint32_t s_tile_nav_tick;

#define WEARABLE_APP_SWIPE_UP_GRACE_MS  900
#define WEARABLE_TILE_NAV_GRACE_MS      500
/* Suppress LV_EVENT_CLICKED once finger moves past tap slop — matches
 * wearable_input: ambiguous drags must not launch the centered app. */
#define WEARABLE_APP_TILE_SWIPE_PX   12

static const char *wearable_home_gesture_name(gui_shell_gesture_t gesture)
{
    switch (gesture) {
    case GUI_SHELL_GESTURE_SWIPE_UP:
        return "UP";
    case GUI_SHELL_GESTURE_SWIPE_LEFT:
        return "LEFT";
    case GUI_SHELL_GESTURE_SWIPE_RIGHT:
        return "RIGHT";
    case GUI_SHELL_GESTURE_TAP:
        return "TAP";
    default:
        return "?";
    }
}

static const char *wearable_home_state_name(wearable_home_state_t state)
{
    return state == WEARABLE_HOME_IN_APP ? "IN_APP" : "LAUNCHER";
}

static unsigned int wearable_home_tile_count(unsigned int app_count)
{
    return app_count + 1;
}

static unsigned int wearable_home_calc_home_index(unsigned int app_count)
{
    return (app_count + 1) / 2;
}

static int wearable_home_tile_to_app(unsigned int tile_index)
{
    if (tile_index < s_home.home_index) {
        return (int)tile_index;
    }
    if (tile_index > s_home.home_index) {
        return (int)tile_index - 1;
    }
    return -1;
}

static unsigned int wearable_home_app_to_tile(unsigned int app_index)
{
    if (app_index < s_home.home_index) {
        return app_index;
    }
    return app_index + 1;
}

static unsigned int wearable_home_wrap_index(int idx)
{
    if (s_home.tile_count == 0) {
        return 0;
    }
    while (idx < 0) {
        idx += (int)s_home.tile_count;
    }
    return (unsigned int)idx % s_home.tile_count;
}

static int wearable_home_launch_app(unsigned int tile_index);

static void wearable_home_enable_event_bubble(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t count;

    if (!obj) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    count = lv_obj_get_child_count(obj);
    for (i = 0; i < count; i++) {
        wearable_home_enable_event_bubble(lv_obj_get_child(obj, (int32_t)i));
    }
}

static void wearable_home_icon_layout(int *box_w, int *box_h, int *icon_h, int *label_h)
{
    gui_shell_metrics_t metrics = {0};
    int hor = 240;
    int ver = 240;

    if (gui_display_get_metrics(&metrics) == 0 && metrics.hor_res > 0 && metrics.ver_res > 0) {
        hor = metrics.hor_res;
        ver = metrics.ver_res;
    }

    if (label_h) {
        *label_h = 28;
    }
    if (box_w) {
        *box_w = hor * 4 / 5;
    }
    if (box_h) {
        *box_h = ver * 3 / 5;
    }
    if (icon_h && box_h && label_h) {
        *icon_h = *box_h - *label_h - 6;
        if (*icon_h < hor / 3) {
            *icon_h = hor / 3;
        }
    }
}

static void wearable_home_set_app_icon(lv_obj_t *icon, const char *icon_path, const char *app_id)
{
    char resolved[384];
    char path[400];

    if (!icon || !icon_path || icon_path[0] == '\0') {
        return;
    }

    resolved[0] = '\0';
    if (app_id && app_id[0] != '\0') {
        snprintf(resolved, sizeof(resolved), "%s/apps/%s.png", RES_IMAGE_PATH, app_id);
        if (access(resolved, F_OK) != 0) {
            resolved[0] = '\0';
        }
    }
    if (resolved[0] == '\0') {
        if (icon_path[0] == '/') {
            snprintf(resolved, sizeof(resolved), "%s", icon_path);
        } else if (gui_shell_resolve_asset_path(resolved, sizeof(resolved), icon_path) != 0) {
            snprintf(resolved, sizeof(resolved), "%s", icon_path);
        }
    }
    if (access(resolved, F_OK) != 0) {
        log_warn("app icon missing: %s", resolved);
        return;
    }

    lv_obj_remove_style_all(icon);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_TRANSP, 0);

    snprintf(path, sizeof(path), "%c:%s", LV_FS_STDIO_LETTER, resolved);
    lv_image_set_src(icon, path);
}

static void wearable_home_icon_fit(lv_obj_t *icon, int max_w, int max_h)
{
    if (!icon || max_w <= 0 || max_h <= 0) {
        return;
    }

    lv_obj_set_width(icon, max_w);
    lv_obj_set_height(icon, max_h);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
}

static void wearable_home_update_dots(void)
{
    unsigned int i;

    if (!s_home.dots_cont) {
        return;
    }

    for (i = 0; i < lv_obj_get_child_count(s_home.dots_cont); i++) {
        lv_obj_t *dot = lv_obj_get_child(s_home.dots_cont, (int32_t)i);
        bool active = (i == s_home.active_index);

        lv_obj_set_size(dot, active ? 7 : 5, active ? 7 : 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot,
                                  lv_color_hex(active ? 0x05fbe2 : 0x444444), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
    }
}

static void wearable_home_show_tile(unsigned int index)
{
    unsigned int prev;

    if (!s_home.tileview || s_home.tile_count == 0) {
        return;
    }

    prev = s_home.active_index;
    s_home.active_index = wearable_home_wrap_index((int)index);
    log_debug("[nav] show_tile req=%u prev=%u -> active=%u home=%u state=%s",
             index, prev, s_home.active_index, s_home.home_index,
             wearable_home_state_name(s_home.state));
    lv_obj_stop_scroll_anim(s_home.tileview);
    lv_tileview_set_tile_by_index(s_home.tileview, s_home.active_index, 0, LV_ANIM_OFF);
    wearable_home_update_dots();
}

static const wearable_app_desc_t *wearable_home_find_app_desc(const char *app_id)
{
    int count = 0;
    const wearable_app_desc_t *apps;
    int i;

    if (!app_id || app_id[0] == '\0') {
        return NULL;
    }

    apps = wearable_app_registry_list(&count);
    for (i = 0; apps && i < count; i++) {
        if (strcmp(apps[i].app_id, app_id) == 0) {
            return &apps[i];
        }
    }
    return NULL;
}

static void wearable_home_invalidate_so_cache(int cache_index)
{
    if (cache_index < 0 || cache_index >= WEARABLE_APP_MAX_DYNAMIC)
        return;

    pthread_mutex_lock(&s_so_cache_lock);
    if (s_so_cache[cache_index].valid && s_so_cache[cache_index].handle) {
        dlclose(s_so_cache[cache_index].handle);
    }
    memset(&s_so_cache[cache_index], 0, sizeof(s_so_cache[cache_index]));
    pthread_mutex_unlock(&s_so_cache_lock);
}

static void *wearable_home_get_so_handle(int cache_index, const char *so_path)
{
    void *handle;
    struct stat st;

    if (!so_path || so_path[0] == '\0') {
        return NULL;
    }

    pthread_mutex_lock(&s_so_cache_lock);

    if (cache_index >= 0 && cache_index < WEARABLE_APP_MAX_DYNAMIC &&
        s_so_cache[cache_index].valid &&
        strcmp(s_so_cache[cache_index].so_path, so_path) == 0) {
        if (stat(so_path, &st) == 0 &&
            st.st_mtime == s_so_cache[cache_index].mtime &&
            st.st_size == s_so_cache[cache_index].size) {
            handle = s_so_cache[cache_index].handle;
            pthread_mutex_unlock(&s_so_cache_lock);
            return handle;
        }
        log_info("reload app so (nfs updated): %s", so_path);
        if (s_so_cache[cache_index].valid && s_so_cache[cache_index].handle) {
            dlclose(s_so_cache[cache_index].handle);
        }
        memset(&s_so_cache[cache_index], 0, sizeof(s_so_cache[cache_index]));
    }

    handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        log_warn("dlopen failed: %s (%s)", so_path, dlerror());
        pthread_mutex_unlock(&s_so_cache_lock);
        return NULL;
    }

    if (cache_index >= 0 && cache_index < WEARABLE_APP_MAX_DYNAMIC) {
        s_so_cache[cache_index].handle = handle;
        snprintf(s_so_cache[cache_index].so_path,
                 sizeof(s_so_cache[cache_index].so_path), "%s", so_path);
        s_so_cache[cache_index].valid = 1;
        if (stat(so_path, &st) == 0) {
            s_so_cache[cache_index].mtime = st.st_mtime;
            s_so_cache[cache_index].size = st.st_size;
        }
    }

    pthread_mutex_unlock(&s_so_cache_lock);
    return handle;
}

static void wearable_home_preload_apps(const gui_shell_app_desc_t *apps, unsigned int app_count)
{
    unsigned int i;

    for (i = 0; apps && i < app_count; i++) {
        if (apps[i].so_path && apps[i].so_path[0] != '\0') {
            (void)wearable_home_get_so_handle((int)i, apps[i].so_path);
        }
    }
}

static void *wearable_home_preload_thread(void *arg)
{
    (void)arg;

    (void)thread_set_current_name("home_preload");
    wearable_home_preload_apps(s_apps, s_home.app_count);
    return NULL;
}

static void wearable_home_start_preload(void)
{
    pthread_t tid;

    if (pthread_create(&tid, NULL, wearable_home_preload_thread, NULL) != 0) {
        log_warn("app preload thread create failed");
        return;
    }
    (void)pthread_detach(tid);
}

static lv_obj_t *wearable_home_create_clock_tile(lv_obj_t *tileview, unsigned int tile_index)
{
    lv_obj_t *tile = lv_tileview_add_tile(tileview, (uint8_t)tile_index, 0, LV_DIR_HOR);

    lv_obj_set_size(tile, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(tile, 0, 0);

    if (!wearable_clock_create(tile)) {
        log_error("clock tile create failed");
    }

    return tile;
}

static void wearable_home_app_tile_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();

    (void)e;
    s_app_tile_suppress_launch = 0;
    s_app_tile_press_valid = 0;
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    lv_indev_get_point(indev, &s_app_tile_press);
    s_app_tile_press_valid = 1;
}

static void wearable_home_app_tile_released(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    int dx;
    int dy;

    (void)e;
    if (!indev || !s_app_tile_press_valid) {
        s_app_tile_suppress_launch = 1;
        s_app_tile_press_valid = 0;
        return;
    }

    lv_indev_get_point(indev, &point);
    dx = point.x - s_app_tile_press.x;
    dy = point.y - s_app_tile_press.y;
    s_app_tile_press_valid = 0;
    /* Suppress launch on any real drag (not only clear horizontal swipe). */
    if (abs(dx) >= WEARABLE_APP_TILE_SWIPE_PX || abs(dy) >= WEARABLE_APP_TILE_SWIPE_PX) {
        s_app_tile_suppress_launch = 1;
    }
    if (wearable_input_had_horizontal_swipe()) {
        s_app_tile_suppress_launch = 1;
    }
    log_debug("[nav] app_tile RELEASED tile=%u dx=%d dy=%d suppress=%d hor=%d",
             (unsigned)(uintptr_t)lv_event_get_user_data(e), dx, dy,
             s_app_tile_suppress_launch, wearable_input_had_horizontal_swipe());
}

static void wearable_home_app_tile_clicked(lv_event_t *e)
{
    uintptr_t tile_index;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    tile_index = (uintptr_t)lv_event_get_user_data(e);
    if (s_app_tile_suppress_launch || wearable_input_had_horizontal_swipe()) {
        log_debug("[nav] app_tile CLICKED tile=%u suppressed (suppress=%d hor=%d grace=%ums)",
                 (unsigned)tile_index, s_app_tile_suppress_launch,
                 wearable_input_had_horizontal_swipe(),
                 (unsigned)(lv_tick_get() - s_tile_nav_tick));
        s_app_tile_suppress_launch = 0;
        return;
    }
    if (s_tile_nav_tick != 0 && lv_tick_get() - s_tile_nav_tick < WEARABLE_TILE_NAV_GRACE_MS) {
        log_debug("[nav] app_tile CLICKED tile=%u ignored (nav grace %ums)",
                 (unsigned)tile_index,
                 (unsigned)(lv_tick_get() - s_tile_nav_tick));
        return;
    }

    log_debug("[nav] app_tile CLICKED tile=%u -> launch", (unsigned)tile_index);
    (void)wearable_home_launch_app((unsigned int)tile_index);
}

static lv_obj_t *wearable_home_create_app_tile(lv_obj_t *tileview,
                                           unsigned int tile_index,
                                           const gui_shell_app_desc_t *app)
{
    int box_w = 160;
    int box_h = 120;
    int icon_h = 88;
    int label_h = 28;
    lv_obj_t *tile = lv_tileview_add_tile(tileview, (uint8_t)tile_index, 0, LV_DIR_HOR);
    lv_obj_t *container;
    lv_obj_t *icon;
    lv_obj_t *label;
    lv_font_t *font_label;

    wearable_home_icon_layout(&box_w, &box_h, &icon_h, &label_h);

    lv_obj_set_size(tile, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(tile, 0, 0);

    container = lv_obj_create(tile);
    lv_obj_set_size(container, box_w, box_h);
    lv_obj_center(container);
    lv_obj_set_style_bg_opa(container, LV_OPA_0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(container, wearable_home_app_tile_pressed, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)tile_index);
    lv_obj_add_event_cb(container, wearable_home_app_tile_released, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)tile_index);
    lv_obj_add_event_cb(container, wearable_home_app_tile_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)tile_index);

    icon = lv_image_create(container);
    wearable_home_set_app_icon(icon, app->icon_path, app->id);
    wearable_home_icon_fit(icon, box_w, icon_h);

    label = lv_label_create(container);
    font_label = gui_font_get_cjk_font(18);
    if (!font_label) {
        font_label = (lv_font_t *)&lv_font_montserrat_22;
    }
    lv_obj_set_size(label, box_w, label_h);
    lv_obj_set_style_text_font(label, font_label, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x05fbe2), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, app->label ? app->label : app->id);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);

    return tile;
}

static void wearable_home_create_dots(lv_obj_t *parent)
{
    unsigned int i;

    s_home.dots_cont = lv_obj_create(parent);
    lv_obj_set_size(s_home.dots_cont, lv_pct(100), 24);
    lv_obj_align(s_home.dots_cont, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(s_home.dots_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_home.dots_cont, 0, 0);
    lv_obj_set_style_pad_all(s_home.dots_cont, 0, 0);
    lv_obj_set_flex_flow(s_home.dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_home.dots_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_home.dots_cont, 6, 0);
    lv_obj_remove_flag(s_home.dots_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home.dots_cont, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (i = 0; i < s_home.tile_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_home.dots_cont);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void wearable_home_invoke_app_teardown(void)
{
    const wearable_app_desc_t *app;
    wearable_app_destroy_fn_t destroy;

    app = wearable_home_find_app_desc(s_home.active_app_id);
    if (app && app->destroy_symbol[0] != '\0') {
        destroy = (wearable_app_destroy_fn_t)dlsym(RTLD_DEFAULT, app->destroy_symbol);
        if (destroy) {
            destroy();
            return;
        }
        log_warn("destroy symbol missing: %s", app->destroy_symbol);
    }

    /* Legacy fallback for camera before app.json carries destroy_symbol. */
    destroy = (wearable_app_destroy_fn_t)dlsym(RTLD_DEFAULT, "wearable_camera_ui_destroy");
    if (destroy) {
        destroy();
    }
}

static void wearable_home_close_app(void)
{
    if (s_home.state != WEARABLE_HOME_IN_APP) {
        return;
    }

    log_info("[nav] close_app active_tile=%u app=%s -> home tile %u",
             s_home.active_index, s_home.active_app_id, s_home.home_index);
    s_home.state = WEARABLE_HOME_LAUNCHER;
    wearable_home_invoke_app_teardown();

    if (s_home.app_root) {
        lv_obj_delete(s_home.app_root);
        s_home.app_root = NULL;
    }
    if (s_home.tileview) {
        lv_obj_remove_flag(s_home.tileview, LV_OBJ_FLAG_HIDDEN);
    }

    s_home.active_app_id[0] = '\0';
    wearable_clock_set_suspended(0);
    wearable_home_show_tile(s_home.home_index);
}

static int wearable_home_launch_app(unsigned int tile_index)
{
    int app_index;
    const gui_shell_app_desc_t *app;
    void *handle;
    wearable_app_entry_fn_t entry;
    int ret;

    if (s_home.state == WEARABLE_HOME_IN_APP) {
        return 0;
    }

    if (tile_index == s_home.home_index) {
        return 0;
    }

    app_index = wearable_home_tile_to_app(tile_index);
    if (app_index < 0 || (unsigned int)app_index >= s_home.app_count) {
        return -1;
    }

    app = &s_apps[app_index];
    if (!app->id || app->id[0] == '\0' ||
        !app->entry_symbol || app->entry_symbol[0] == '\0') {
        log_error("launch failed: invalid app descriptor at index %d (tile %u)",
                  app_index, tile_index);
        return -1;
    }
    log_info("launch app %s from tile %u (%s)", app->id, tile_index,
             app->so_path ? app->so_path : "(none)");

    handle = wearable_home_get_so_handle(app_index, app->so_path);
    entry = NULL;
    if (handle) {
        entry = (wearable_app_entry_fn_t)dlsym(handle, app->entry_symbol);
    }
    if (!entry) {
        entry = (wearable_app_entry_fn_t)dlsym(RTLD_DEFAULT, app->entry_symbol);
    }
    if (!entry) {
        log_error("launch failed: %s (%s)", app->entry_symbol, dlerror());
        return -1;
    }

    s_home.state = WEARABLE_HOME_IN_APP;
    s_home.active_index = tile_index;
    snprintf(s_home.active_app_id, sizeof(s_home.active_app_id), "%s",
             app->id ? app->id : "");

    wearable_clock_set_suspended(1);
    lv_obj_add_flag(s_home.tileview, LV_OBJ_FLAG_HIDDEN);
    s_home.app_root = lv_obj_create(lv_obj_get_parent(s_home.tileview));
    lv_obj_set_size(s_home.app_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_border_width(s_home.app_root, 0, 0);
    lv_obj_set_style_pad_all(s_home.app_root, 0, 0);
    lv_obj_center(s_home.app_root);
    wearable_home_enable_event_bubble(s_home.app_root);

    ret = entry(s_home.app_root);
    if (ret != 0) {
        log_error("app entry failed: %s", app->id);
        wearable_home_invoke_app_teardown();
        lv_obj_delete(s_home.app_root);
        s_home.app_root = NULL;
        lv_obj_remove_flag(s_home.tileview, LV_OBJ_FLAG_HIDDEN);
        s_home.active_app_id[0] = '\0';
        s_home.state = WEARABLE_HOME_LAUNCHER;
        wearable_clock_set_suspended(0);
        return -1;
    }

    wearable_input_reset_gesture();
    s_app_enter_tick = lv_tick_get();
    return 0;
}

int wearable_home_create(lv_obj_t *parent,
                     const gui_shell_app_desc_t *apps,
                     unsigned int app_count)
{
    unsigned int tile_index;
    unsigned int i;

    if (!parent) {
        log_error("null parent");
        return -1;
    }

    memset(&s_home, 0, sizeof(s_home));
    memset(s_so_cache, 0, sizeof(s_so_cache));
    memset(s_home_apps, 0, sizeof(s_home_apps));
    if (apps && app_count > 0) {
        if (app_count > WEARABLE_APP_MAX_DYNAMIC) {
            app_count = WEARABLE_APP_MAX_DYNAMIC;
        }
        memcpy(s_home_apps, apps, app_count * sizeof(gui_shell_app_desc_t));
    }
    s_apps = s_home_apps;
    s_home.app_count = app_count;
    s_home.tile_count = wearable_home_tile_count(app_count);
    s_home.home_index = wearable_home_calc_home_index(app_count);
    s_home.root = parent;
    s_home.state = WEARABLE_HOME_LAUNCHER;

    s_home.tileview = lv_tileview_create(parent);
    lv_obj_set_size(s_home.tileview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_home.tileview, lv_color_hex(0x000000), 0);
    lv_obj_set_style_anim_duration(s_home.tileview, 0, 0);
    lv_obj_set_scroll_snap_x(s_home.tileview, LV_SCROLL_SNAP_CENTER);
    /* Programmatic page flip only: no slow finger-drag / throw scrolling. */
    lv_obj_remove_flag(s_home.tileview, LV_OBJ_FLAG_SCROLLABLE);

    tile_index = 0;
    for (i = 0; i < s_home.home_index; i++) {
        wearable_home_create_app_tile(s_home.tileview, tile_index++, &s_home_apps[i]);
    }
    wearable_home_create_clock_tile(s_home.tileview, tile_index++);
    for (i = s_home.home_index; i < app_count; i++) {
        wearable_home_create_app_tile(s_home.tileview, tile_index++, &s_home_apps[i]);
    }

    wearable_home_create_dots(parent);
    if (s_home.tile_count <= 1 && s_home.dots_cont) {
        lv_obj_add_flag(s_home.dots_cont, LV_OBJ_FLAG_HIDDEN);
    }

    wearable_home_enable_event_bubble(s_home.tileview);

    wearable_home_show_tile(s_home.home_index);
    wearable_home_start_preload();
    return 0;
}

int wearable_home_on_gesture(gui_shell_gesture_t gesture)
{
    unsigned int prev;

    if (s_home.tile_count == 0) {
        return -1;
    }

    prev = s_home.active_index;
    log_debug("[nav] on_gesture %s state=%s active=%u home=%u",
             wearable_home_gesture_name(gesture), wearable_home_state_name(s_home.state),
             prev, s_home.home_index);

    if (s_home.state == WEARABLE_HOME_IN_APP) {
        if (gesture == GUI_SHELL_GESTURE_SWIPE_UP) {
            uint32_t now = lv_tick_get();
            if (now - s_app_enter_tick < WEARABLE_APP_SWIPE_UP_GRACE_MS) {
                log_debug("[nav] ignore swipe-up close (grace %ums)", (unsigned)WEARABLE_APP_SWIPE_UP_GRACE_MS);
                return 0;
            }
            wearable_home_close_app();
        }
        return 0;
    }

    switch (gesture) {
    case GUI_SHELL_GESTURE_SWIPE_LEFT:
    case GUI_SHELL_GESTURE_SWIPE_RIGHT: {
        uint32_t now = lv_tick_get();
        int target = (gesture == GUI_SHELL_GESTURE_SWIPE_LEFT) ?
                     (int)s_home.active_index + 1 : (int)s_home.active_index - 1;

        if (s_tile_nav_tick != 0 && now - s_tile_nav_tick < WEARABLE_TILE_NAV_GRACE_MS) {
            log_debug("[nav] ignore %s (nav grace %ums, active=%u target=%d)",
                     wearable_home_gesture_name(gesture),
                     (unsigned)(now - s_tile_nav_tick), prev, target);
            return 0;
        }
        s_tile_nav_tick = now;
        log_debug("[nav] %s active=%u -> %d",
                 gesture == GUI_SHELL_GESTURE_SWIPE_LEFT ? "swipe-left" : "swipe-right",
                 prev, target);
        wearable_home_show_tile((unsigned int)target);
        return 0;
    }
    case GUI_SHELL_GESTURE_TAP:
        /* Launch the currently-centered app tile. wearable_input classifies each
         * gesture as exactly one of TAP / SWIPE_* from the raw finger movement, so
         * a page-flip swipe never arrives here as a TAP — this removes the old
         * swipe-triggers-launch and next-page-phantom-click races. A short grace
         * after a page flip drops any stray tap. */
        if (s_tile_nav_tick != 0 && lv_tick_get() - s_tile_nav_tick < WEARABLE_TILE_NAV_GRACE_MS) {
            log_debug("[nav] tap ignored (nav grace %ums)",
                     (unsigned)(lv_tick_get() - s_tile_nav_tick));
            return 0;
        }
        log_debug("[nav] tap -> launch active tile %u", s_home.active_index);
        (void)wearable_home_launch_app(s_home.active_index);
        return 0;
    case GUI_SHELL_GESTURE_SWIPE_UP:
    default:
        return 0;
    }
}

lv_obj_t *wearable_home_get_root(void)
{
    return s_home.root;
}

void wearable_home_return_to_launcher(void)
{
    wearable_home_close_app();
}

int wearable_home_launch_app_by_id(const char *app_id)
{
    unsigned int i;

    if (!app_id || app_id[0] == '\0') {
        return -1;
    }
    for (i = 0; i < s_home.app_count; i++) {
        if (s_apps[i].id && strcmp(s_apps[i].id, app_id) == 0) {
            return wearable_home_launch_app(wearable_home_app_to_tile(i));
        }
    }
    log_error("launch by id failed: app %s not in registry", app_id);
    return -1;
}
