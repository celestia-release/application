// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo main: home screen, menu, time/date cards, and sub-demo entry points.
 */
#define LOG_TAG "TABLET_HOME"

/*********************
 *      INCLUDES
 *********************/
#include "tablet_home.h"

#include "config.h"
#include "tablet_font.h"
#include "tablet_common.h"
#include "tablet_battery.h"
#include "app_main.h"
#include "log.h"
#include "notification.h"
#include "tablet_app_registry.h"
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#if CONFIG_NETWORK_SUPPORTED
#include "network.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#endif
#if defined(CONFIG_APP_WEATHER) && CONFIG_NETWORK_SUPPORTED
#include "weather_service.h"
#endif
#include "ntp_sync.h"
#include "system_time.h"

/*********************
 *      DEFINES
 *********************/
/* Launcher tuned on 800x480; scaled via GUI_SCALE() for 1024x600 */
#define MENU_ICON_WIDTH         GUI_SCALE(120)
#define MENU_ICON_HEIGHT        GUI_SCALE_VER(132)
#define MENU_ICON_IMG_SIZE      GUI_SCALE(88)
#define MENU_ICON_RADIUS        GUI_SCALE(22)
#define MENU_ICON_LABEL_GAP     GUI_SCALE_VER(12)
#define LAUNCHER_PAD_X          GUI_SCALE(16)   /* card/menu area left/right inset */
#define LAUNCHER_PAD_Y          GUI_SCALE_VER(8)   /* flex row gap between icon rows */
#define LAUNCHER_TOP_H_PCT      10
#define LAUNCHER_CARDS_TOP_PCT  12  /* widget card row: time + date */
#define LAUNCHER_CARDS_H_PCT    24
#define LAUNCHER_CARD_W_PCT     48  /* each card; 4% total for gap+insets */
#define LAUNCHER_CARD_RADIUS    GUI_SCALE(24)
#define LAUNCHER_CARD_PAD_X     GUI_SCALE(18)
#define LAUNCHER_MENU_TOP_PCT   40   /* below card row */
#define LAUNCHER_MENU_H_PCT     58   /* 2 rows: 132+8+132=272px in ~278px */
#define LAUNCHER_FONT_TIME      GUI_SCALE(80)  /* rendered size; Montserrat 48 scaled up */
#define LAUNCHER_FONT_DATE      GUI_SCALE(24)  /* weekday+month line on the date card */
#define LAUNCHER_FONT_DAY       GUI_SCALE(72)  /* big bold day-of-month, fills lower card */
#define LAUNCHER_FONT_LUNAR     GUI_SCALE(14)  /* 农历, vertical column beside the day */
#define LAUNCHER_FONT_WEATHER   GUI_SCALE(20)  /* minimal weather text on the date card */
#define LAUNCHER_FONT_MENU      GUI_SCALE(20)
/* Ink on the dark wallpaper. Menu stays 20 so the home bitmap font is used. */
#define LAUNCHER_COLOR_INK          0xF4F7FB
#define LAUNCHER_COLOR_MUTED        0xC5D0DC
#define LAUNCHER_COLOR_WIFI_OFF     0x8E9EAE
/* Frosted-glass widget cards on the dark wallpaper.
 * LVGL 9.3 has no LV_OPA_12/15: use the explicit 8-bit values (≈12%, ≈15%). */
#define LAUNCHER_COLOR_CARD_BG      0xFFFFFF
#define LAUNCHER_CARD_BG_OPA        ((lv_opa_t)31)
#define LAUNCHER_CARD_BORDER_OPA    ((lv_opa_t)38)
/* Minimal weather text: fetch on connect, refresh hourly (NTP_SYN_PIEROD). */
#define TABLET_WEATHER_POLL_SEC 3600

/* Phone-style battery (Luna wearable_clock). Digits use lv_font_home_20 (Montserrat-12 disabled on Nova). */
#define TABLET_COLOR_BAT_OK             0xeeeeee
#define TABLET_COLOR_BAT_LOW_FILL       0x7a2020
#define TABLET_COLOR_BAT_CHARGE         0x34c759  /* iPhone charging green */
#define TABLET_COLOR_BAT_BOLT           0x12351b  /* bolt ink on the green fill */
#define TABLET_BAT_BODY_W               GUI_SCALE(36)
#define TABLET_BAT_BODY_H               GUI_SCALE_VER(22)
#define TABLET_BAT_TIP_W                GUI_SCALE(2)
#define TABLET_BAT_TIP_H                GUI_SCALE_VER(10)
#define TABLET_BAT_PAD                  GUI_SCALE(1)
#define TABLET_BAT_FILL_MAX_W           (TABLET_BAT_BODY_W - 2 - 2 * TABLET_BAT_PAD)
#define TABLET_BAT_ICON_W               (TABLET_BAT_BODY_W + TABLET_BAT_TIP_W)
#define TABLET_BAT_GAP                  GUI_SCALE(16)
#define TABLET_BAT_RIGHT_PAD            GUI_SCALE(6)
/* WiFi left of battery via align_to; GAP is clear space between glyph boxes. */

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    lv_obj_t *label_wlan;
    lv_obj_t *label_battery;
    lv_obj_t *label_charge_bolt;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_body;
    lv_obj_t *battery_fill;
    lv_obj_t *battery_tip;
} tablet_home_top_t;

typedef struct {
    lv_obj_t *card_time;        /* left widget card: big clock HH:MM */
    lv_obj_t *label_time;
    lv_obj_t *label_sec;        /* small seconds, bottom right of the clock card */
    lv_obj_t *card_date;        /* right widget card: date + lunar + weather */
    lv_obj_t *label_date_top;   /* e.g. "#4DA3FF 星期二# 11月" */
    lv_obj_t *label_date_day;   /* e.g. "3" (bold) */
    lv_obj_t *label_lunar;      /* e.g. 九/月/廿/五 vertical, beside the day */
    lv_obj_t *label_weather;    /* e.g. "晴 25℃" small, bottom right of the card */
} tablet_home_date_time_t;

typedef struct {
    lv_obj_t *main;
    lv_obj_t *cards;        /* widget row: time card + date card */
    lv_obj_t *menu_outer;
    lv_obj_t *menu_flex;
    tablet_home_date_time_t DateTime;
    tablet_home_top_t top;
} tablet_home_context_t;

/**********************
 *  EXTERN PROTOTYPES
 **********************/
typedef int (*tablet_call_msg_callback_fn_t)(int enable, void *pUser);
static tablet_call_msg_callback_fn_t s_tablet_call_msg_callback_fn = NULL;

static void tablet_ensure_call_msg_callback(void)
{
    if (s_tablet_call_msg_callback_fn)
        return;
    /* Resolve symbol from loaded app SO (dlopen uses RTLD_GLOBAL). */
    s_tablet_call_msg_callback_fn = (tablet_call_msg_callback_fn_t)dlsym(RTLD_DEFAULT, "tablet_call_msg_callback");
}

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void tablet_top_init(lv_obj_t *parent, tablet_home_context_t *ctx);
static void tablet_battery_init(lv_obj_t *parent, tablet_home_context_t *ctx);
static void tablet_update_battery_indicator(tablet_home_context_t *ctx);
/** Load icon/background.qoi under main; keep pure black if file missing. */
static void tablet_home_try_set_background(lv_obj_t *main);

static void tablet_time_init(lv_obj_t *parent, tablet_home_context_t *ctx);
static void tablet_date_card_init(lv_obj_t *card, tablet_home_context_t *ctx);

/** Frosted-glass widget card (time / date) on the dark wallpaper. */
static lv_obj_t *launcher_create_card(lv_obj_t *parent, int32_t w, int32_t h);

static void tablet_menu_init(lv_obj_t *parent, tablet_home_context_t *ctx);
static void tablet_dump_launcher_layout(tablet_home_context_t *ctx);
static lv_obj_t *tablet_create_menu(lv_obj_t *parent, const char *label);
static void launcher_icon_fill(lv_obj_t *box);
static void launcher_apply_generated_icon(lv_obj_t *box, const tablet_app_desc_t *app);
static lv_obj_t *tablet_create_menu_for_app(lv_obj_t *parent, const tablet_app_desc_t *app);
static void tablet_resolve_entry_so(const tablet_app_desc_t *app, char *out, size_t out_len);
static void tablet_entry_so_lib_fallback(const tablet_app_desc_t *app, char *out, size_t out_len);
static void event_callback_menu_app(lv_event_t *e);

static void tablet_home_deferred_images_cb(lv_timer_t *timer);

/** Layout-only container: do not steal touches from child menu cells. */
static void launcher_make_layout_only(lv_obj_t *obj)
{
    if (obj == NULL)
        return;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
}

static lv_obj_t *launcher_create_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    if (card == NULL)
        return NULL;
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(LAUNCHER_COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LAUNCHER_CARD_BG_OPA, 0);
    lv_obj_set_style_radius(card, LAUNCHER_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(LAUNCHER_COLOR_CARD_BG), 0);
    lv_obj_set_style_border_opa(card, LAUNCHER_CARD_BORDER_OPA, 0);
    launcher_make_layout_only(card);
    return card;
}

static void second_classes_timer(lv_timer_t *timer);

static void tablet_home_try_set_background(lv_obj_t *main)
{
    char bg_path[384];

    if (main == NULL)
        return;

    if (tablet_resolve_image_path(ICON_BACKGROUND, bg_path, sizeof(bg_path)) != 0) {
        log_info("launcher wallpaper missing, use black bg (tried %s)", bg_path);
        return;
    }

    lv_obj_t *bg = lv_image_create(main);
    if (bg == NULL)
        return;

    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_obj_align(bg, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(bg);

    if (tablet_image_set_src(bg, ICON_BACKGROUND) != LV_RESULT_OK) {
        lv_obj_delete(bg);
        log_warn("launcher wallpaper load failed, keep black bg");
        return;
    }
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_move_background(bg);

    log_info("launcher wallpaper: %s", bg_path);
}

#ifdef CONFIG_APP_TABLET_CAMERA
/* launcher 只通过 JSON/app manifest 启动应用，不做静态调用 */
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

static tablet_home_context_t s_context = {0};
static tablet_home_context_t *s_ctx = &s_context;

static notification_t *get_notification(void)
{
    return (notification_t *)app_main_get_notification();
}

#if CONFIG_NETWORK_SUPPORTED
/* Same criterion as ntp_sync.c: shell wifi_driver/udhcpc may own wlan0 without FW station. */
static bool tablet_iface_has_valid_ip(const char *iface)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    bool ok = false;

    if (!iface || getifaddrs(&ifaddr) != 0)
        return false;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, iface) != 0)
            continue;
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr != 0 && sin->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            ok = true;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return ok;
}

/**
 * @brief True when framework WiFi station is connected, or wlan0/eth0 has a real IP.
 */
static bool network_is_connected(void)
{
    network_t *s_network = (network_t *)app_main_get_network();

    if (s_network && s_network->wifi && s_network->wifi->station) {
        network_wifi_station_state_e state;
        char ip[64] = {0};

        if (s_network->wifi->station->get_state(&state) == NETWORK_OK
                && state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
            s_network->wifi->station->get_ip_address(ip, sizeof(ip));
            if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
                return true;
        }
    }

    if (tablet_iface_has_valid_ip("wlan0") || tablet_iface_has_valid_ip("eth0"))
        return true;

    return false;
}

#if defined(CONFIG_APP_WEATHER)
/* curl in weather service must not block LVGL timer; apply text on timer tick. */
static pthread_mutex_t s_weather_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_weather_busy = 0;
static int s_weather_ui_pending = 0;
static char s_weather_pending_text[128];

static int tablet_weather_store_text(const char *image_path, const char *lable_text)
{
    (void)image_path;
    pthread_mutex_lock(&s_weather_mtx);
    if (lable_text && lable_text[0])
        snprintf(s_weather_pending_text, sizeof(s_weather_pending_text), "%s", lable_text);
    else
        s_weather_pending_text[0] = '\0';
    s_weather_ui_pending = 1;
    pthread_mutex_unlock(&s_weather_mtx);
    return 0;
}

static void *tablet_weather_fetch_thread(void *arg)
{
    (void)arg;
    (void)app_weather_update_request(tablet_weather_store_text);
    pthread_mutex_lock(&s_weather_mtx);
    s_weather_busy = 0;
    pthread_mutex_unlock(&s_weather_mtx);
    return NULL;
}

static void tablet_weather_request_async(void)
{
    pthread_t tid;

    pthread_mutex_lock(&s_weather_mtx);
    if (s_weather_busy) {
        pthread_mutex_unlock(&s_weather_mtx);
        return;
    }
    s_weather_busy = 1;
    pthread_mutex_unlock(&s_weather_mtx);

    if (pthread_create(&tid, NULL, tablet_weather_fetch_thread, NULL) == 0) {
        (void)pthread_detach(tid);
        return;
    }

    pthread_mutex_lock(&s_weather_mtx);
    s_weather_busy = 0;
    pthread_mutex_unlock(&s_weather_mtx);
    log_warn("weather fetch thread create failed");
}

static void tablet_weather_apply_pending_text(void)
{
    char text[128];
    int pending = 0;

    pthread_mutex_lock(&s_weather_mtx);
    pending = s_weather_ui_pending;
    if (pending) {
        snprintf(text, sizeof(text), "%s", s_weather_pending_text);
        s_weather_ui_pending = 0;
    }
    pthread_mutex_unlock(&s_weather_mtx);

    if (!pending)
        return;
    if (text[0]) {
        /* Only show once a fetch actually delivered something (no network ->
         * label stays hidden). */
        lv_label_set_text(s_ctx->DateTime.label_weather, text);
        lv_obj_clear_flag(s_ctx->DateTime.label_weather, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif /* CONFIG_APP_WEATHER */

#endif /* CONFIG_NETWORK_SUPPORTED */

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * @brief : main entry function
 * @return
 */
int tablet_home(void)
{
    tablet_home_context_t *ctx = s_ctx;
    memset(ctx, 0, sizeof(tablet_home_context_t));

    /* create main window */
    lv_obj_t *main = tablet_create_obj(lv_display_get_screen_active(lv_display_get_default()), LV_HOR_RES, LV_VER_RES);
    ctx->main = main;
    /* Same ink as icon/background.qoi, so the first frame matches the wallpaper. */
    lv_obj_set_style_bg_color(main, lv_color_hex(0x12141A), 0);
    lv_obj_set_style_bg_opa(main, LV_OPA_COVER, 0);
    launcher_make_layout_only(main);
    /* Background loads after the skeleton frame (deferred timer below). */

    lv_obj_t *top = tablet_create_obj(main, LV_PCT(100), LV_PCT(LAUNCHER_TOP_H_PCT));
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_left(top, LAUNCHER_PAD_X, 0);
    lv_obj_set_style_pad_right(top, LAUNCHER_PAD_X, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_0, 0);
    launcher_make_layout_only(top);
    tablet_top_init(top, ctx);

    lv_obj_t *cards = tablet_create_obj(main, LV_PCT(100), LV_PCT(LAUNCHER_CARDS_H_PCT));
    lv_obj_align(cards, LV_ALIGN_TOP_MID, 0, LV_PCT(LAUNCHER_CARDS_TOP_PCT));
    lv_obj_set_style_pad_left(cards, LAUNCHER_PAD_X + GUI_SCALE(8), 0);
    lv_obj_set_style_pad_right(cards, LAUNCHER_PAD_X + GUI_SCALE(8), 0);
    lv_obj_set_style_bg_opa(cards, LV_OPA_0, 0);
    launcher_make_layout_only(cards);
    ctx->cards = cards;

    ctx->DateTime.card_time = launcher_create_card(cards, LV_PCT(LAUNCHER_CARD_W_PCT), LV_PCT(100));
    if (ctx->DateTime.card_time)
        lv_obj_align(ctx->DateTime.card_time, LV_ALIGN_LEFT_MID, 0, 0);
    tablet_time_init(ctx->DateTime.card_time, ctx);

    ctx->DateTime.card_date = launcher_create_card(cards, LV_PCT(LAUNCHER_CARD_W_PCT), LV_PCT(100));
    if (ctx->DateTime.card_date)
        lv_obj_align(ctx->DateTime.card_date, LV_ALIGN_RIGHT_MID, 0, 0);
    tablet_date_card_init(ctx->DateTime.card_date, ctx);

    lv_obj_t *menu = tablet_create_obj(main, LV_PCT(100), LV_PCT(LAUNCHER_MENU_H_PCT));
    lv_obj_align(menu, LV_ALIGN_TOP_MID, 0, LV_PCT(LAUNCHER_MENU_TOP_PCT));
    lv_obj_set_style_pad_left(menu, LAUNCHER_PAD_X, 0);
    lv_obj_set_style_pad_right(menu, LAUNCHER_PAD_X, 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_0, 0);
    launcher_make_layout_only(menu);
    ctx->menu_outer = menu;
    tablet_menu_init(menu, ctx);

    lv_obj_update_layout(main);
    lv_refr_now(lv_display_get_default());
    (void)system("touch /tmp/ipc_gui_ready 2>/dev/null");
    log_info("HOME_FIRST_FRAME t_ms=%lld", system_get_sys_run_time64());
#ifdef TABLET_HOME_DUMP_LAYOUT
    tablet_dump_launcher_layout(ctx);
#endif

    /* QOI wallpaper + full fonts land after the skeleton frame. */
    lv_timer_t *img_timer = lv_timer_create(tablet_home_deferred_images_cb, 100, ctx);
    lv_timer_set_repeat_count(img_timer, 1);

    /* NTP starts in second_classes_timer after post-home network_open. */

    lv_timer_t *timer = lv_timer_create(second_classes_timer, 1000, (void *)ctx);
    lv_timer_ready(timer);
    return LV_RESULT_OK;
}

/**********************
 *  STATIC FUNCTIONS
 **********************/

static void tablet_update_battery_indicator(tablet_home_context_t *ctx)
{
    int pct;
    int fill_w;
    uint32_t fill_color = 0x4a4a4a;
    uint32_t frame_color = TABLET_COLOR_BAT_OK;
    int charging;

    if (!ctx || !ctx->top.label_battery || !ctx->top.battery_fill)
        return;

    pct = tablet_battery_get_percent();
    charging = tablet_battery_is_charging();
    if (pct >= 0) {
        lv_label_set_text_fmt(ctx->top.label_battery, "%d", pct);
        if (charging) {
            /* iPhone style while plugged: full green fill, digits swapped for a bolt. */
            fill_w = TABLET_BAT_FILL_MAX_W;
            fill_color = TABLET_COLOR_BAT_CHARGE;
            frame_color = TABLET_COLOR_BAT_CHARGE;
        } else {
            fill_w = (TABLET_BAT_FILL_MAX_W * pct + 50) / 100;
            if (pct > 0 && fill_w < 1)
                fill_w = 1;
            if (pct <= 20)
                fill_color = TABLET_COLOR_BAT_LOW_FILL;
        }
    } else {
        lv_label_set_text(ctx->top.label_battery, "--");
        fill_w = 0;
    }

    /* Digits <-> bolt swap. */
    if (charging && pct >= 0) {
        lv_obj_add_flag(ctx->top.label_battery, LV_OBJ_FLAG_HIDDEN);
        if (ctx->top.label_charge_bolt) {
            lv_obj_remove_flag(ctx->top.label_charge_bolt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_center(ctx->top.label_charge_bolt);
            lv_obj_move_foreground(ctx->top.label_charge_bolt);
        }
    } else {
        lv_obj_remove_flag(ctx->top.label_battery, LV_OBJ_FLAG_HIDDEN);
        if (ctx->top.label_charge_bolt)
            lv_obj_add_flag(ctx->top.label_charge_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(ctx->top.label_battery);
        lv_obj_move_foreground(ctx->top.label_battery);
    }

    lv_obj_set_width(ctx->top.battery_fill, fill_w);
    lv_obj_set_style_bg_color(ctx->top.battery_fill, lv_color_hex(fill_color), 0);
    /* Outline / tip / digits stay white (Luna OK color); frame goes green while charging. */
    lv_obj_set_style_text_color(ctx->top.label_battery, lv_color_hex(TABLET_COLOR_BAT_OK), 0);
    if (ctx->top.battery_body) {
        lv_obj_set_style_border_color(ctx->top.battery_body,
                                      lv_color_hex(frame_color), 0);
    }
    if (ctx->top.battery_tip) {
        lv_obj_set_style_bg_color(ctx->top.battery_tip, lv_color_hex(frame_color), 0);
    }
}

static void tablet_battery_init(lv_obj_t *parent, tablet_home_context_t *ctx)
{
    /* home_20 includes ASCII digits; Montserrat-12 is disabled on Nova (snap→28 clips). */
    lv_font_t *font = tablet_font_get_freetype_font(20);

    if (!font)
        font = tablet_font_get_montserrat_font(28);

    ctx->top.battery_icon = lv_obj_create(parent);
    lv_obj_set_size(ctx->top.battery_icon, TABLET_BAT_ICON_W, TABLET_BAT_BODY_H);
    lv_obj_set_style_bg_opa(ctx->top.battery_icon, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ctx->top.battery_icon, 0, 0);
    lv_obj_set_style_pad_all(ctx->top.battery_icon, 0, 0);
    lv_obj_remove_flag(ctx->top.battery_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctx->top.battery_icon, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(ctx->top.battery_icon, LV_ALIGN_RIGHT_MID, -TABLET_BAT_RIGHT_PAD, 0);

    ctx->top.battery_body = lv_obj_create(ctx->top.battery_icon);
    lv_obj_set_size(ctx->top.battery_body, TABLET_BAT_BODY_W, TABLET_BAT_BODY_H);
    lv_obj_set_style_bg_opa(ctx->top.battery_body, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ctx->top.battery_body, 1, 0);
    lv_obj_set_style_border_color(ctx->top.battery_body, lv_color_hex(TABLET_COLOR_BAT_OK), 0);
    lv_obj_set_style_radius(ctx->top.battery_body, 3, 0);
    lv_obj_set_style_pad_all(ctx->top.battery_body, TABLET_BAT_PAD, 0);
    lv_obj_remove_flag(ctx->top.battery_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctx->top.battery_body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(ctx->top.battery_body, LV_ALIGN_LEFT_MID, 0, 0);

    ctx->top.battery_fill = lv_obj_create(ctx->top.battery_body);
    lv_obj_set_size(ctx->top.battery_fill, 0, LV_PCT(100));
    lv_obj_set_style_bg_color(ctx->top.battery_fill, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(ctx->top.battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctx->top.battery_fill, 0, 0);
    lv_obj_set_style_radius(ctx->top.battery_fill, 1, 0);
    lv_obj_set_style_pad_all(ctx->top.battery_fill, 0, 0);
    lv_obj_remove_flag(ctx->top.battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ctx->top.battery_fill, LV_ALIGN_LEFT_MID, 0, 0);

    ctx->top.label_battery = lv_label_create(ctx->top.battery_body);
    lv_obj_set_style_text_font(ctx->top.label_battery, font, 0);
    lv_obj_set_style_text_color(ctx->top.label_battery, lv_color_hex(TABLET_COLOR_BAT_OK), 0);
    lv_obj_set_style_text_letter_space(ctx->top.label_battery, -1, 0);
    lv_label_set_text(ctx->top.label_battery, "--");
    lv_obj_add_flag(ctx->top.label_battery, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_center(ctx->top.label_battery);
    lv_obj_move_foreground(ctx->top.label_battery);

    /* Charging bolt (LV_SYMBOL_CHARGE lives in the built-in Montserrat FA range). */
    ctx->top.label_charge_bolt = NULL;
    lv_font_t *bolt_font = tablet_font_get_montserrat_font(28);
    if (bolt_font) {
        ctx->top.label_charge_bolt = lv_label_create(ctx->top.battery_body);
        lv_obj_set_style_text_font(ctx->top.label_charge_bolt, bolt_font, 0);
        lv_obj_set_style_text_color(ctx->top.label_charge_bolt,
                                    lv_color_hex(TABLET_COLOR_BAT_BOLT), 0);
        lv_label_set_text(ctx->top.label_charge_bolt, LV_SYMBOL_CHARGE);
        /* 28px glyph vs ~18px inner height: shrink around the center pivot. */
        lv_obj_set_style_transform_scale(ctx->top.label_charge_bolt,
                                         LV_SCALE_NONE * 55 / 100, 0);
        lv_obj_set_style_transform_pivot_x(ctx->top.label_charge_bolt,
                                           LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(ctx->top.label_charge_bolt,
                                           LV_PCT(50), 0);
        lv_obj_add_flag(ctx->top.label_charge_bolt, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_center(ctx->top.label_charge_bolt);
        lv_obj_add_flag(ctx->top.label_charge_bolt, LV_OBJ_FLAG_HIDDEN);
    }

    ctx->top.battery_tip = lv_obj_create(ctx->top.battery_icon);
    lv_obj_set_size(ctx->top.battery_tip, TABLET_BAT_TIP_W, TABLET_BAT_TIP_H);
    lv_obj_set_style_bg_color(ctx->top.battery_tip, lv_color_hex(TABLET_COLOR_BAT_OK), 0);
    lv_obj_set_style_bg_opa(ctx->top.battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctx->top.battery_tip, 0, 0);
    lv_obj_set_style_radius(ctx->top.battery_tip, 1, 0);
    lv_obj_set_style_pad_all(ctx->top.battery_tip, 0, 0);
    lv_obj_remove_flag(ctx->top.battery_tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ctx->top.battery_tip, LV_ALIGN_RIGHT_MID, 0, 0);

    tablet_update_battery_indicator(ctx);
}

static void tablet_top_init(lv_obj_t *parent, tablet_home_context_t *ctx)
{
    /* Weather lives in its own widget card (see tablet_home()); the status bar
     * only carries WiFi + battery. */

    /* Battery rightmost; WiFi immediately to its left. */
    tablet_battery_init(parent, ctx);

    /* create wlan icon (fixed position) */
#if CONFIG_FW_NETWORK_WIFI
    lv_font_t *font = tablet_font_get_montserrat_font(28);

    lv_obj_t *label_wlan = lv_label_create(parent);
    lv_obj_set_style_text_font(label_wlan, font, 0);
    /* SIZE_CONTENT: LV_PCT(20) made the symbol sit far left of the battery. */
    lv_obj_set_size(label_wlan, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_text(label_wlan, LV_SYMBOL_WIFI);
    /* align_to: GAP is real clear space (RIGHT_MID math alone looked unchanged at 4→6). */
    if (ctx->top.battery_icon) {
        lv_obj_align_to(label_wlan, ctx->top.battery_icon,
                        LV_ALIGN_OUT_LEFT_MID, -TABLET_BAT_GAP, 0);
    } else {
        lv_obj_align(label_wlan, LV_ALIGN_RIGHT_MID, -TABLET_BAT_RIGHT_PAD, 0);
    }
    lv_obj_set_style_text_color(label_wlan, lv_color_hex(LAUNCHER_COLOR_WIFI_OFF), 0);
    lv_obj_remove_flag(label_wlan, LV_OBJ_FLAG_SCROLLABLE);
    ctx->top.label_wlan = label_wlan;
#endif

    return ;
}

/* Weekday strings indexed by tm_wday; localized via lan() (CN/EN). */
static const char *s_weekday_str[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static void tablet_time_init(lv_obj_t *parent, tablet_home_context_t *ctx)
{
    lv_font_t *font;

    if (!parent)
        return;

    /* Montserrat tops out at 48; scale the label up around its center. */
    font = tablet_font_get_montserrat_font(48);
    lv_obj_t *label_time = lv_label_create(parent);

    lv_obj_set_style_text_font(label_time, font, 0);
    lv_obj_set_style_text_letter_space(label_time, GUI_SCALE(1), 0);
    lv_label_set_text(label_time, "00:00");
    lv_obj_set_style_text_color(label_time, lv_color_hex(LAUNCHER_COLOR_INK), 0);
    lv_obj_set_style_transform_scale(label_time,
            LV_SCALE_NONE * LAUNCHER_FONT_TIME / 48, 0);
    lv_obj_set_style_transform_pivot_x(label_time, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(label_time, LV_PCT(50), 0);
    lv_obj_center(label_time);

    ctx->DateTime.label_time = label_time;

    /* Seconds: bitmap font only; positioned by tablet_time_update each tick. */
    lv_obj_t *label_sec = lv_label_create(parent);
    lv_label_set_text(label_sec, "00");
    lv_obj_set_style_text_font(label_sec, tablet_font_get_montserrat_font(28), 0);
    lv_obj_set_style_text_letter_space(label_sec, GUI_SCALE(4), 0);
    lv_obj_set_style_text_color(label_sec, lv_color_hex(LAUNCHER_COLOR_MUTED), 0);

    ctx->DateTime.label_sec = label_sec;
}

/* Right card: weekday+month, bold day, vertical lunar, minimal weather. */
static void tablet_date_card_init(lv_obj_t *card, tablet_home_context_t *ctx)
{
    if (!card)
        return;

    lv_obj_t *label_top = lv_label_create(card);
    lv_label_set_recolor(label_top, true);
    lv_label_set_text(label_top, "#4DA3FF Mon# 1");
    lv_obj_align(label_top, LV_ALIGN_TOP_MID, 0, GUI_SCALE_VER(4));
    lv_obj_set_style_text_color(label_top, lv_color_hex(LAUNCHER_COLOR_MUTED), 0);

    /* Bold day fills the lower card; font applied in the deferred callback. */
    lv_obj_t *label_day = lv_label_create(card);
    lv_label_set_text(label_day, "1");
    lv_obj_align(label_day, LV_ALIGN_TOP_MID, 0, GUI_SCALE_VER(28));
    lv_obj_set_style_text_color(label_day, lv_color_hex(LAUNCHER_COLOR_INK), 0);

    /* Lunar: placed and filled by tablet_time_update each tick. */
    lv_obj_t *label_lunar = lv_label_create(card);
    lv_label_set_text(label_lunar, "");
    lv_obj_set_style_text_color(label_lunar, lv_color_hex(LAUNCHER_COLOR_MUTED), 0);

    /* Weather text, bottom left; stays hidden until a fetch succeeds. */
    lv_obj_t *label_weather = lv_label_create(card);
    lv_label_set_text(label_weather, "");
    lv_obj_align(label_weather, LV_ALIGN_BOTTOM_LEFT,
                 LAUNCHER_CARD_PAD_X, -GUI_SCALE_VER(4));
    lv_obj_set_style_text_color(label_weather, lv_color_hex(LAUNCHER_COLOR_MUTED), 0);
    lv_obj_add_flag(label_weather, LV_OBJ_FLAG_HIDDEN);

    ctx->DateTime.label_date_top = label_top;
    ctx->DateTime.label_date_day = label_day;
    ctx->DateTime.label_lunar = label_lunar;
    ctx->DateTime.label_weather = label_weather;
}

/* ---- Lunar calendar (classic 1900..2100 table; bit16=leap month is 30d,
 * bits15..4=month1..12 lengths, bit3..0=leap month number) ---- */
static const uint32_t s_lunar_info[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, /* 1900-1909 */
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, /* 1910-1919 */
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, /* 1920-1929 */
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, /* 1930-1939 */
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, /* 1940-1949 */
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, /* 1950-1959 */
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, /* 1960-1969 */
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, /* 1970-1979 */
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, /* 1980-1989 */
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0, /* 1990-1999 */
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, /* 2000-2009 */
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, /* 2010-2019 */
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, /* 2020-2029 */
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, /* 2030-2039 */
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, /* 2040-2049 */
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0, /* 2050-2059 */
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4, /* 2060-2069 */
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, /* 2070-2079 */
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, /* 2080-2089 */
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a4d0, 0x0d150, 0x0f252, /* 2090-2099 */
    0x0d520, /* 2100 */
};

static const char *s_lunar_month_str[] = {
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "冬月", "腊月"
};
static const char *s_lunar_day_str[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
};

static int lunar_leap_month(int y)
{
    return s_lunar_info[y - 1900] & 0xF;
}

static int lunar_leap_days(int y)
{
    /* No leap month -> 0 extra days (must not add a phantom month length). */
    return lunar_leap_month(y) ? ((s_lunar_info[y - 1900] & 0x10000) ? 30 : 29) : 0;
}

static int lunar_month_days(int y, int m)
{
    return (s_lunar_info[y - 1900] & (0x10000 >> m)) ? 30 : 29;
}

static int lunar_year_days(int y)
{
    int i, sum = 348;

    for (i = 0x8000; i > 0x8; i >>= 1)
        sum += (s_lunar_info[y - 1900] & i) ? 1 : 0;
    return sum + lunar_leap_days(y);
}

/* Proleptic Gregorian day number (Howard Hinnant's days_from_civil). */
static long days_from_civil(int y, int m, int d)
{
    int era, yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468;
}

/* Solar -> lunar. Returns 0 ok, -1 when outside the 1900..2100 table. */
static int solar_to_lunar(const struct tm *t, int *l_month, int *l_day, int *l_leap)
{
    long offset = days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday)
                - days_from_civil(1900, 1, 31);
    int y, m, leap, temp = 0, is_leap = 0;

    for (y = 1900; y <= 2100 && offset > 0; y++) {
        temp = lunar_year_days(y);
        offset -= temp;
    }
    if (offset < 0) {
        offset += temp;
        y--;
    }
    if (y < 1900 || y > 2100)
        return -1;

    leap = lunar_leap_month(y);
    for (m = 1; m < 13 && offset > 0; m++) {
        if (leap > 0 && m == leap + 1 && !is_leap) {
            m--;
            is_leap = 1;
            temp = lunar_leap_days(y);
        } else {
            temp = lunar_month_days(y, m);
        }
        if (is_leap && m == leap + 1)
            is_leap = 0;
        offset -= temp;
    }
    if (offset == 0 && leap > 0 && m == leap + 1) {
        if (is_leap) {
            is_leap = 0;
        } else {
            is_leap = 1;
            m--;
        }
    }
    if (offset < 0) {
        offset += temp;
        m--;
    }

    *l_month = m;
    *l_day = (int)offset + 1;
    *l_leap = is_leap;
    return 0;
}

/* 农历竖排: '\n' after each 3-byte UTF-8 char. */
static void lunar_text_vertical(char *out, size_t cap, const char *s)
{
    size_t o = 0;

    while (s[0] && s[1] && s[2] && o + 4 < cap) {
        out[o++] = s[0];
        out[o++] = s[1];
        out[o++] = s[2];
        s += 3;
        if (s[0])
            out[o++] = '\n';
    }
    out[o] = '\0';
}

static void tablet_time_update(tablet_home_date_time_t *pDateTime)
{
    /* refresh date and time (wall clock via TZ, e.g. CST-8 in app_01_start.sh) */
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);

    lv_label_set_text_fmt(pDateTime->label_time, "%02d:%02d",
                          timeinfo->tm_hour, timeinfo->tm_min);
    if (pDateTime->label_sec)
        lv_label_set_text_fmt(pDateTime->label_sec, "%02d", timeinfo->tm_sec);

    /* Seconds tuck into the big time's bottom-right corner (recomputed every
     * tick, so wide clocks like "08:00" never collide with them). */
    if (pDateTime->label_sec && pDateTime->label_time) {
        lv_area_t ta;
        lv_area_t ca;
        lv_obj_t *card = lv_obj_get_parent(pDateTime->label_sec);
        const lv_font_t *tf = tablet_font_get_montserrat_font(48);
        int32_t ascent = tf ? (tf->line_height - tf->base_line) : 43;

        lv_obj_update_layout(pDateTime->label_time);
        lv_obj_update_layout(pDateTime->label_sec);
        lv_obj_get_coords(pDateTime->label_time, &ta);
        lv_obj_get_transformed_area(pDateTime->label_time, &ta, 0);
        lv_obj_get_coords(card, &ca);
        /* Anchor x just right of the time's transformed right edge (plain
         * coords ignore the scale); y to the scaled digit baseline, not the
         * line box. */
        lv_obj_align(pDateTime->label_sec, LV_ALIGN_TOP_LEFT,
                     ta.x2 + GUI_SCALE(6) - ca.x1,
                     ta.y1 + ascent * LAUNCHER_FONT_TIME / 48 + GUI_SCALE(2)
                             - lv_obj_get_height(pDateTime->label_sec) - ca.y1);
    }

    if (pDateTime->label_date_top && pDateTime->label_date_day) {
        const char *weekday = lan((char *)s_weekday_str[timeinfo->tm_wday]);

        lv_label_set_text_fmt(pDateTime->label_date_top, "#4DA3FF %s# %d%s",
                              weekday, timeinfo->tm_mon + 1,
                              (get_lan_index() == 1) ? "月" : "");

        lv_label_set_text_fmt(pDateTime->label_date_day, "%d", timeinfo->tm_mday);

        /* Lunar hugs the day's right edge, mid-height; re-applied every tick
         * so "1"<->"31" width changes stay correct. */
        if (pDateTime->label_lunar) {
            int lm, ld, leap;

            lv_obj_update_layout(pDateTime->label_date_day);
            lv_obj_align_to(pDateTime->label_lunar, pDateTime->label_date_day,
                            LV_ALIGN_OUT_RIGHT_MID, GUI_SCALE(8), 0);

            if (solar_to_lunar(timeinfo, &lm, &ld, &leap) == 0
                    && lm >= 1 && lm <= 12 && ld >= 1 && ld <= 30) {
                if (get_lan_index() == 1) {
                    char flat[24];
                    char vert[32];

                    snprintf(flat, sizeof(flat), "%s%s%s",
                             leap ? "闰" : "", s_lunar_month_str[lm - 1],
                             s_lunar_day_str[ld - 1]);
                    lunar_text_vertical(vert, sizeof(vert), flat);
                    lv_label_set_text(pDateTime->label_lunar, vert);
                } else
                    lv_label_set_text_fmt(pDateTime->label_lunar, "%d-%d%s",
                                          lm, ld, leap ? "+" : "");
            } else {
                lv_label_set_text(pDateTime->label_lunar, "");
            }
        }
    }
}

static void tablet_read_net_msg_timer(lv_timer_t *timer)
{
    lv_obj_t *obj = lv_timer_get_user_data(timer);

    notification_t *notification = get_notification();
    if (notification == NULL || notification->queue == NULL) {
        return;
    }

    int handle = notification->queue->create("net2ui");
    if (handle <= 0) {
        return;
    }

    int enable = 0;
    int size = sizeof(enable);
    long long msg_time;

    int ret = notification->queue->read(handle, (char *)&enable, &size, &msg_time);

    if(ret == 0)
    {
        tablet_ensure_call_msg_callback();
        if (s_tablet_call_msg_callback_fn)
            s_tablet_call_msg_callback_fn(enable, (void *)obj);
    }

}


static void tablet_menu_init(lv_obj_t *parent, tablet_home_context_t *ctx)
{
    /* Single menu container, aligned with backup V0.12.1 layout (flex row wrap).
     * More apps than one screen holds: the container scrolls vertically, so the
     * rows beyond the first screen stay reachable. */
    lv_obj_t *menu = tablet_create_obj(parent, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(menu, LV_OPA_0, 0);
    launcher_make_layout_only(menu);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(menu, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu, LV_SCROLLBAR_MODE_AUTO);
    ctx->menu_flex = menu;

    lv_obj_set_layout(menu, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_ROW_WRAP);
    /* cross START: 1st icon row pinned to menu top (avoid SPACE_AROUND pulling rows up) */
    lv_obj_set_flex_align(menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(menu, LAUNCHER_PAD_Y, 0);

    tablet_app_registry_refresh();
    int app_count = 0;
    const tablet_app_desc_t *apps = tablet_app_registry_list(&app_count);
    for (int i = 0; apps && i < app_count; i++) {
        if (!apps[i].enabled)
            continue;
        lv_obj_t *app_obj = tablet_create_menu_for_app(menu, &apps[i]);
        if (!app_obj)
            continue;
        lv_obj_add_event_cb(app_obj, tablet_input_log_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(app_obj, event_callback_menu_app, LV_EVENT_CLICKED, (void *)&apps[i]);
    }
}

static void tablet_dump_launcher_layout(tablet_home_context_t *ctx)
{
#ifdef TABLET_HOME_DUMP_LAYOUT
    lv_obj_t *main;
    lv_obj_t *top;
    lv_obj_t *time;
    uint32_t i;
    uint32_t child_cnt;
    char tag[64];

    if (!ctx || !ctx->main) {
        return;
    }

    main = ctx->main;
    log_info("[layout] screen wh=(%d,%d) pct: top=%d%% cards_top=%d%% cards_h=%d%% menu_top=%d%% menu_h=%d%%",
             (int)LV_HOR_RES, (int)LV_VER_RES,
             LAUNCHER_TOP_H_PCT, LAUNCHER_CARDS_TOP_PCT, LAUNCHER_CARDS_H_PCT,
             LAUNCHER_MENU_TOP_PCT, LAUNCHER_MENU_H_PCT);

    tablet_log_obj_bounds("main", main);

    top = lv_obj_get_child(main, 0);
    time = lv_obj_get_child(main, 1);
    tablet_log_obj_bounds("top_bar", top);
    tablet_log_obj_bounds("cards_row", time);
    if (ctx->DateTime.card_time) {
        tablet_log_obj_bounds("cards.time", ctx->DateTime.card_time);
    }
    if (ctx->DateTime.card_date) {
        tablet_log_obj_bounds("cards.date", ctx->DateTime.card_date);
    }
#if CONFIG_FW_NETWORK_WIFI
    if (ctx->top.label_wlan) {
        tablet_log_obj_bounds("top.wlan", ctx->top.label_wlan);
    }
#endif
    if (ctx->top.battery_icon) {
        tablet_log_obj_bounds("top.battery", ctx->top.battery_icon);
    }

    tablet_log_obj_bounds("time_block", time);
    tablet_log_obj_bounds("time.label_time", ctx->DateTime.label_time);
    tablet_log_obj_bounds("date.label_top", ctx->DateTime.label_date_top);
    tablet_log_obj_bounds("date.label_day", ctx->DateTime.label_date_day);

    tablet_log_obj_bounds("menu_outer", ctx->menu_outer);
    tablet_log_obj_bounds("menu_flex", ctx->menu_flex);

    if (!ctx->menu_flex) {
        return;
    }

    child_cnt = lv_obj_get_child_cnt(ctx->menu_flex);
    log_info("[layout] menu_flex child_cnt=%u icon_cell=%dx%d pad_row=%d",
             (unsigned)child_cnt, MENU_ICON_WIDTH, MENU_ICON_HEIGHT, LAUNCHER_PAD_Y);

    for (i = 0; i < child_cnt; i++) {
        lv_obj_t *cell = lv_obj_get_child(ctx->menu_flex, i);
        lv_obj_t *icon;
        lv_obj_t *label;
        const char *label_txt;

        snprintf(tag, sizeof(tag), "menu[%u].cell", (unsigned)i);
        tablet_log_obj_bounds(tag, cell);

        icon = lv_obj_get_child(cell, 0);
        label = lv_obj_get_child(cell, lv_obj_get_child_cnt(cell) - 1);
        snprintf(tag, sizeof(tag), "menu[%u].icon", (unsigned)i);
        tablet_log_obj_bounds(tag, icon);
        snprintf(tag, sizeof(tag), "menu[%u].label", (unsigned)i);
        tablet_log_obj_bounds(tag, label);

        label_txt = (label != NULL) ? lv_label_get_text(label) : "(null)";
        log_info("[layout] menu[%u] name=\"%s\"", (unsigned)i,
                 label_txt != NULL ? label_txt : "(null)");
    }
#else
    (void)ctx;
#endif /* TABLET_HOME_DUMP_LAYOUT */
}

/**********************
 *  Generated app icons
 **********************/
/* Every app icon is drawn by one code path: gradient squircle + white FA glyph
 * from builtin Montserrat. No PNG assets, one visual language, resolution
 * independent. A new app just adds a row here (or falls through to the hashed
 * fallback below and gets a stable auto hue). */
typedef enum {
    LAUNCHER_ICON_GLYPH = 0,    /* st->glyph: LV_SYMBOL_* in the builtin FA range */
    LAUNCHER_ICON_CHAT,         /* speech bubble + typing dots (AI conversation) */
    LAUNCHER_ICON_CAMERA,       /* body + viewfinder bump + lens ring */
    LAUNCHER_ICON_SCREEN_CAM,   /* portrait screen device + lens (带屏摄像机) */
    LAUNCHER_ICON_GAMEPAD,      /* capsule body + grips + D-pad + dots (games) */
    LAUNCHER_ICON_MIC,          /* capsule head + stem + base (recording) */
    LAUNCHER_ICON_CALENDAR,     /* binder rings + header bar + dot grid */
    LAUNCHER_ICON_MUSIC,        /* beamed double note (music player) */
} launcher_icon_kind_t;

typedef struct {
    const char *app_id;
    launcher_icon_kind_t kind;
    const char *glyph;      /* LV_SYMBOL_*, only for LAUNCHER_ICON_GLYPH */
    uint32_t grad_top;
    uint32_t grad_bottom;
} launcher_icon_style_t;

/* Icon look (kind + gradient) comes from each app's own app.json
 * (icon_kind / icon_glyph / icon_color / icon_color2). Apps without a
 * manifest get a stable hashed palette below; no per-app table here. */

/* Auto palette for apps without a manifest (stable hash of app_id). */
static const uint32_t s_icon_grad_fallback[][2] = {
    { 0x5AC8FA, 0x2F7CF6 }, { 0xFFB340, 0xF07C1F }, { 0x63E6A5, 0x22A24E },
    { 0xC77BFF, 0x8A2BE2 }, { 0xFF7A9E, 0xE03A5F }, { 0x4AD0D0, 0x1E8F8F },
};

/* app.json "icon_glyph" names -> builtin FA symbols in Montserrat. */
static const struct {
    const char *name;
    const char *symbol;
} s_icon_glyphs[] = {
    { "audio", LV_SYMBOL_AUDIO },       { "video", LV_SYMBOL_VIDEO },
    { "image", LV_SYMBOL_IMAGE },       { "list", LV_SYMBOL_LIST },
    { "settings", LV_SYMBOL_SETTINGS }, { "home", LV_SYMBOL_HOME },
    { "play", LV_SYMBOL_PLAY },         { "pause", LV_SYMBOL_PAUSE },
    { "stop", LV_SYMBOL_STOP },         { "next", LV_SYMBOL_NEXT },
    { "prev", LV_SYMBOL_PREV },         { "shuffle", LV_SYMBOL_SHUFFLE },
    { "loop", LV_SYMBOL_LOOP },         { "volume", LV_SYMBOL_VOLUME_MAX },
    { "mute", LV_SYMBOL_MUTE },         { "bell", LV_SYMBOL_BELL },
    { "envelope", LV_SYMBOL_ENVELOPE }, { "call", LV_SYMBOL_CALL },
    { "wifi", LV_SYMBOL_WIFI },         { "bluetooth", LV_SYMBOL_BLUETOOTH },
    { "gps", LV_SYMBOL_GPS },           { "usb", LV_SYMBOL_USB },
    { "sd_card", LV_SYMBOL_SD_CARD },   { "drive", LV_SYMBOL_DRIVE },
    { "directory", LV_SYMBOL_DIRECTORY }, { "file", LV_SYMBOL_FILE },
    { "edit", LV_SYMBOL_EDIT },         { "keyboard", LV_SYMBOL_KEYBOARD },
    { "download", LV_SYMBOL_DOWNLOAD }, { "upload", LV_SYMBOL_UPLOAD },
    { "refresh", LV_SYMBOL_REFRESH },   { "power", LV_SYMBOL_POWER },
    { "charge", LV_SYMBOL_CHARGE },     { "battery", LV_SYMBOL_BATTERY_FULL },
    { "eye", LV_SYMBOL_EYE_OPEN },      { "tint", LV_SYMBOL_TINT },
    { "warning", LV_SYMBOL_WARNING },   { "ok", LV_SYMBOL_OK },
    { "trash", LV_SYMBOL_TRASH },       { "save", LV_SYMBOL_SAVE },
    { "copy", LV_SYMBOL_COPY },         { "cut", LV_SYMBOL_CUT },
    { "bars", LV_SYMBOL_BARS },         { "plus", LV_SYMBOL_PLUS },
};

static const struct {
    const char *name;
    launcher_icon_kind_t kind;
} s_icon_kinds[] = {
    { "glyph", LAUNCHER_ICON_GLYPH },       { "chat", LAUNCHER_ICON_CHAT },
    { "camera", LAUNCHER_ICON_CAMERA },     { "screen_cam", LAUNCHER_ICON_SCREEN_CAM },
    { "gamepad", LAUNCHER_ICON_GAMEPAD },   { "mic", LAUNCHER_ICON_MIC },
    { "calendar", LAUNCHER_ICON_CALENDAR }, { "music", LAUNCHER_ICON_MUSIC },
};

static int launcher_parse_hex_color(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || out == NULL)
        return 0;
    if (s[0] == '#')
        s++;
    if (strlen(s) != 6)
        return 0;
    v = strtoul(s, &end, 16);
    if (end == NULL || *end != '\0')
        return 0;
    *out = (uint32_t)v;
    return 1;
}

/* Icon described in app.json (icon_kind / icon_glyph / icon_color / icon_color2). */
static int launcher_icon_style_from_manifest(const tablet_app_desc_t *app, launcher_icon_style_t *st)
{
    size_t i;
    int known = 0;

    if (app == NULL || (app->icon_kind[0] == '\0' && app->icon_glyph[0] == '\0'))
        return 0;
    st->kind = LAUNCHER_ICON_GLYPH;
    st->glyph = LV_SYMBOL_FILE;
    for (i = 0; i < sizeof(s_icon_kinds) / sizeof(s_icon_kinds[0]); i++) {
        if (strcmp(s_icon_kinds[i].name, app->icon_kind) == 0) {
            st->kind = s_icon_kinds[i].kind;
            known = 1;
        }
    }
    for (i = 0; i < sizeof(s_icon_glyphs) / sizeof(s_icon_glyphs[0]); i++) {
        if (strcmp(s_icon_glyphs[i].name, app->icon_glyph) == 0) {
            st->glyph = s_icon_glyphs[i].symbol;
            known = 1;
        }
    }
    if (!known)
        return 0;
    if (launcher_parse_hex_color(app->icon_color, &st->grad_top)) {
        if (!launcher_parse_hex_color(app->icon_color2, &st->grad_bottom))
            st->grad_bottom = st->grad_top;
    }
    st->app_id = app->app_id;
    return 1;
}

/* Icon look for one app: app.json manifest first, hashed palette fallback. */
static const launcher_icon_style_t *launcher_icon_style_of(const tablet_app_desc_t *app)
{
    static launcher_icon_style_t st_auto;
    static const launcher_icon_style_t st_default = {
        "app", LAUNCHER_ICON_GLYPH, LV_SYMBOL_FILE, 0xB8BECB, 0x6E7686,
    };

    if (app == NULL || app->app_id[0] == '\0')
        return &st_default;
    /* Unknown app: FNV-1a of the id picks a stable hue from the palette. */
    uint32_t h = 2166136261u;
    const char *p;
    size_t n = sizeof(s_icon_grad_fallback) / sizeof(s_icon_grad_fallback[0]);

    for (p = app->app_id; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    st_auto.app_id = app->app_id;
    st_auto.kind = LAUNCHER_ICON_GLYPH;
    st_auto.glyph = LV_SYMBOL_FILE; /* meaningful doc pictogram */
    st_auto.grad_top = s_icon_grad_fallback[h % n][0];
    st_auto.grad_bottom = s_icon_grad_fallback[h % n][1];
    launcher_icon_style_from_manifest(app, &st_auto);
    return &st_auto;
}

/* The builtin FA subset has no chat symbol, so AI-conversation apps get a
 * composed icon: white speech bubble + typing dots on the gradient. */
static void launcher_icon_draw_chat(lv_obj_t *box)
{
    lv_coord_t bw = GUI_SCALE(46);
    lv_coord_t bh = GUI_SCALE_VER(36);
    int i;

    /* Bubble tail: white square rotated 45 deg, tucked under the bubble's
     * bottom-left corner so only one tip pokes out. */
    lv_obj_t *tail = lv_obj_create(box);
    if (tail) {
        lv_obj_remove_style_all(tail);
        lv_obj_set_size(tail, GUI_SCALE(10), GUI_SCALE(10));
        lv_obj_set_style_bg_color(tail, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_angle(tail, 450, 0);
        lv_obj_align(tail, LV_ALIGN_BOTTOM_LEFT, GUI_SCALE(23), -GUI_SCALE(27));
        lv_obj_remove_flag(tail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *bubble = lv_obj_create(box);
    if (bubble == NULL)
        return;
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, bw, bh);
    lv_obj_align(bubble, LV_ALIGN_CENTER, 0, -GUI_SCALE_VER(2));
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, GUI_SCALE(12), 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* "AI is typing" indicator: three dots in the bubble. */
    for (i = -1; i <= 1; i++) {
        lv_obj_t *dot = lv_obj_create(bubble);
        if (dot == NULL)
            continue;
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, GUI_SCALE(6), GUI_SCALE(6));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x6A1FB8), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, i * GUI_SCALE(12), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* Composed white camera on the gradient. Plain: landscape body + viewfinder
 * bump + lens ring. 带屏摄像机: portrait screen device + lens + status dot. */
static void launcher_icon_draw_camera(lv_obj_t *box, bool with_screen, uint32_t accent)
{
    lv_coord_t bw, bh;
    lv_obj_t *body;

    if (box == NULL)
        return;

    if (!with_screen) {
        /* Viewfinder bump peeks above the body's top edge. */
        lv_obj_t *bump = lv_obj_create(box);
        if (bump) {
            lv_obj_remove_style_all(bump);
            lv_obj_set_size(bump, GUI_SCALE(16), GUI_SCALE(8));
            lv_obj_set_style_bg_color(bump, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(bump, LV_OPA_COVER, 0);
            lv_obj_align(bump, LV_ALIGN_TOP_MID, -GUI_SCALE(6), GUI_SCALE(17));
            lv_obj_remove_flag(bump, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
        bw = GUI_SCALE(52);
        bh = GUI_SCALE(38);
    } else {
        bw = GUI_SCALE(34);
        bh = GUI_SCALE(48);
    }

    body = lv_obj_create(box);
    if (body == NULL)
        return;
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, bw, bh);
    lv_obj_set_style_bg_color(body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, GUI_SCALE(with_screen ? 9 : 10), 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, with_screen ? GUI_SCALE(2) : 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Lens ring: transparent center shows the gradient through. */
    lv_obj_t *lens = lv_obj_create(box);
    if (lens) {
        lv_obj_remove_style_all(lens);
        lv_obj_set_size(lens, GUI_SCALE(with_screen ? 18 : 22),
                        GUI_SCALE(with_screen ? 18 : 22));
        lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(lens, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(lens, GUI_SCALE(4), 0);
        lv_obj_set_style_border_opa(lens, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(lens, LV_OPA_0, 0);
        lv_obj_align(lens, LV_ALIGN_CENTER, 0,
                     with_screen ? -GUI_SCALE(8) : 0);
        lv_obj_remove_flag(lens, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    if (with_screen) {
        /* Status dot under the lens reads as the lit screen edge. */
        lv_obj_t *dot = lv_obj_create(box);
        if (dot) {
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, GUI_SCALE(5), GUI_SCALE(5));
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(accent), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_align(dot, LV_ALIGN_CENTER, 0, GUI_SCALE(14));
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

/* Gamepad: white landscape capsule + two grip lobes, accent D-pad cross and
 * four action dots. Classic controller silhouette for the games app. */
static void launcher_icon_draw_gamepad(lv_obj_t *box, uint32_t accent)
{
    static const int8_t dot_off[4][2] = {
        { -5, 0 }, { 5, 0 }, { 0, -5 }, { 0, 5 },
    };
    lv_coord_t cross_w[2] = { GUI_SCALE(14), GUI_SCALE(5) };
    lv_coord_t cross_h[2] = { GUI_SCALE(5), GUI_SCALE(14) };
    lv_obj_t *body;
    int i;

    if (box == NULL)
        return;

    /* Landscape capsule body, floats slightly above center. */
    body = lv_obj_create(box);
    if (body == NULL)
        return;
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, GUI_SCALE(50), GUI_SCALE(26));
    lv_obj_set_style_bg_color(body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, GUI_SCALE(13), 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -GUI_SCALE(3));
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Grip lobes tucked under the body's ends read as controller handles. */
    for (i = -1; i <= 1; i += 2) {
        lv_obj_t *grip = lv_obj_create(box);
        if (grip == NULL)
            continue;
        lv_obj_remove_style_all(grip);
        lv_obj_set_size(grip, GUI_SCALE(18), GUI_SCALE(18));
        lv_obj_set_style_bg_color(grip, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(grip, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(grip, LV_ALIGN_CENTER, i * GUI_SCALE(15), GUI_SCALE(7));
        lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* D-pad cross on the left half: accent reads as a cutout on the white body. */
    for (i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        if (bar == NULL)
            continue;
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, cross_w[i], cross_h[i]);
        lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, -GUI_SCALE(13), -GUI_SCALE(3));
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Four action dots in a diamond on the right half. */
    for (i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(box);
        if (dot == NULL)
            continue;
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, GUI_SCALE(5), GUI_SCALE(5));
        lv_obj_set_style_bg_color(dot, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, GUI_SCALE(13 + dot_off[i][0]),
                     -GUI_SCALE(3) + GUI_SCALE(dot_off[i][1]));
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* Microphone: white capsule head, U-shaped holder ring, stem and base bar. */
static void launcher_icon_draw_mic(lv_obj_t *box)
{
    lv_obj_t *part;

    if (box == NULL)
        return;

    /* Holder ring: only the lower half shows below the head. */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(30), GUI_SCALE(30));
        lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(part, GUI_SCALE(4), 0);
        lv_obj_set_style_border_side(part, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT |
                                     LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_0, 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, -GUI_SCALE(2));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(18), GUI_SCALE(30));
        lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, -GUI_SCALE(8));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(4), GUI_SCALE(8));
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, GUI_SCALE(16));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(20), GUI_SCALE(4));
        lv_obj_set_style_radius(part, GUI_SCALE(2), 0);
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, GUI_SCALE(21));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* Calendar: two binder rings over a white page, accent header bar and a
 * 3x2 date-dot grid. Reads as a calendar, not a generic list. */
static void launcher_icon_draw_calendar(lv_obj_t *box, uint32_t accent)
{
    static const int8_t dot_x[3] = { -9, 0, 9 };
    static const int8_t dot_y[2] = { 6, 14 };
    lv_obj_t *part;
    int i, j;

    if (box == NULL)
        return;

    /* Binder rings poking above the page's top edge. */
    for (i = -1; i <= 1; i += 2) {
        part = lv_obj_create(box);
        if (part == NULL)
            continue;
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(4), GUI_SCALE(12));
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, GUI_SCALE(2), 0);
        lv_obj_align(part, LV_ALIGN_CENTER, i * GUI_SCALE(9), -GUI_SCALE(14));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Page body. */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(38), GUI_SCALE(30));
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, GUI_SCALE(6), 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, GUI_SCALE(4));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Accent header strip flush with the page's top edge. */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(38), GUI_SCALE(10));
        lv_obj_set_style_bg_color(part, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, GUI_SCALE(6), 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, -GUI_SCALE(6));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Date dots in the lower page area. */
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 3; j++) {
            part = lv_obj_create(box);
            if (part == NULL)
                continue;
            lv_obj_remove_style_all(part);
            lv_obj_set_size(part, GUI_SCALE(4), GUI_SCALE(4));
            lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(part, lv_color_hex(accent), 0);
            lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
            lv_obj_align(part, LV_ALIGN_CENTER, GUI_SCALE(dot_x[j]),
                         GUI_SCALE(dot_y[i]));
            lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

/* Music player: beamed double note — two heads, stems and one horizontal beam. */
static void launcher_icon_draw_music(lv_obj_t *box)
{
    static const int8_t x[2] = { -8, 8 };
    lv_obj_t *part;
    int i;

    if (box == NULL)
        return;

    /* Beam across the two stem tops. */
    part = lv_obj_create(box);
    if (part) {
        lv_obj_remove_style_all(part);
        lv_obj_set_size(part, GUI_SCALE(26), GUI_SCALE(6));
        lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, GUI_SCALE(2), 0);
        lv_obj_align(part, LV_ALIGN_CENTER, 0, -GUI_SCALE(12));
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    for (i = 0; i < 2; i++) {
        /* Stem under the beam's ends. */
        part = lv_obj_create(box);
        if (part) {
            lv_obj_remove_style_all(part);
            lv_obj_set_size(part, GUI_SCALE(3), GUI_SCALE(16));
            lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
            lv_obj_align(part, LV_ALIGN_CENTER, GUI_SCALE(x[i]), -GUI_SCALE(2));
            lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }

        /* Note head at the stem's foot. */
        part = lv_obj_create(box);
        if (part) {
            lv_obj_remove_style_all(part);
            lv_obj_set_size(part, GUI_SCALE(10), GUI_SCALE(10));
            lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(part, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
            lv_obj_align(part, LV_ALIGN_CENTER, GUI_SCALE(x[i]), GUI_SCALE(6));
            lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

/** Paint the gradient squircle + the app's icon kind onto an icon box. */
static void launcher_apply_generated_icon(lv_obj_t *box, const tablet_app_desc_t *app)
{
    const launcher_icon_style_t *st;
    lv_font_t *font;

    if (box == NULL)
        return;
    st = launcher_icon_style_of(app);

    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(st->grad_top), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex(st->grad_bottom), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);

    switch (st->kind) {
    case LAUNCHER_ICON_CHAT:
        launcher_icon_draw_chat(box);
        return;
    case LAUNCHER_ICON_CAMERA:
        launcher_icon_draw_camera(box, false, st->grad_bottom);
        return;
    case LAUNCHER_ICON_SCREEN_CAM:
        launcher_icon_draw_camera(box, true, st->grad_bottom);
        return;
    case LAUNCHER_ICON_GAMEPAD:
        launcher_icon_draw_gamepad(box, st->grad_bottom);
        return;
    case LAUNCHER_ICON_MIC:
        launcher_icon_draw_mic(box);
        return;
    case LAUNCHER_ICON_CALENDAR:
        launcher_icon_draw_calendar(box, st->grad_bottom);
        return;
    case LAUNCHER_ICON_MUSIC:
        launcher_icon_draw_music(box);
        return;
    case LAUNCHER_ICON_GLYPH:
    default:
        break;
    }

    lv_obj_t *glyph = lv_label_create(box);
    if (glyph) {
        lv_label_set_text(glyph, st->glyph);
        /* Builtin Montserrat 48 carries the FA range (same font as the old X). */
        font = tablet_font_get_montserrat_font(48);
        if (font)
            lv_obj_set_style_text_font(glyph, font, 0);
        lv_obj_set_style_text_color(glyph, lv_color_hex(0xFFFFFF), 0);
        /* Keep the glyph ~55% of the tile at both panel resolutions. */
        lv_obj_set_style_transform_scale(glyph,
                LV_SCALE_NONE * (MENU_ICON_IMG_SIZE * 55 / 100) / 48, 0);
        lv_obj_set_style_transform_pivot_x(glyph, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(glyph, LV_PCT(50), 0);
        lv_obj_center(glyph);
        lv_obj_remove_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* One tile size and one corner radius for every generated icon box. */
static void launcher_icon_fill(lv_obj_t *box)
{
    if (box == NULL)
        return;
    lv_obj_set_size(box, MENU_ICON_IMG_SIZE, MENU_ICON_IMG_SIZE);
    lv_obj_set_style_radius(box, MENU_ICON_RADIUS, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_20, 0);
}

static lv_obj_t *tablet_create_menu(lv_obj_t *parent, const char *label)
{
    lv_obj_t *obj = tablet_create_obj(parent, MENU_ICON_WIDTH, MENU_ICON_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_0, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    /* A few pixels of finger noise must not scroll this cell and cancel CLICKED. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep LV_OBJ_FLAG_SCROLL_CHAIN: dragging on a cell must scroll the
     * vertically scrollable menu behind it (apps beyond one screen). A clean
     * tap still fires CLICKED — LVGL only scrolls past its drag threshold. */
    lv_obj_set_ext_click_area(obj, GUI_SCALE(8));

    /* Child 0. Icon box: gradient squircle drawn by launcher_apply_generated_icon(). */
    lv_obj_t *icon_box = lv_obj_create(obj);
    lv_obj_remove_style_all(icon_box);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 0);
    launcher_icon_fill(icon_box);
    /* Neutral fill until the per-app gradient lands; keeps the skeleton frame solid. */
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x272B33), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(icon_box, GUI_SCALE(6), 0);
    lv_obj_set_style_shadow_ofs_y(icon_box, GUI_SCALE_VER(2), 0);
    lv_obj_set_style_shadow_color(icon_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(icon_box, LV_OPA_40, 0);
    lv_obj_add_flag(icon_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Label sits under the tile. Parent-relative align is order-independent:
     * align_to before set_width() would keep the default-size left edge and
     * push the centered text right of the icon. */
    lv_obj_t *label_obj = lv_label_create(obj);
    lv_obj_align(label_obj, LV_ALIGN_TOP_MID, 0, MENU_ICON_IMG_SIZE + MENU_ICON_LABEL_GAP);
    lv_font_t *font = tablet_font_get_freetype_font(LAUNCHER_FONT_MENU);
    lv_obj_set_style_text_font(label_obj, font, 0);
    lv_label_set_long_mode(label_obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label_obj, MENU_ICON_WIDTH);
    lv_obj_set_style_text_align(label_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_obj, label);
    lv_obj_set_style_text_color(label_obj, lv_color_hex(LAUNCHER_COLOR_INK), 0);
    lv_obj_add_flag(label_obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(label_obj, LV_OBJ_FLAG_CLICKABLE);

    return obj;
}

static lv_obj_t *tablet_create_menu_for_app(lv_obj_t *parent, const tablet_app_desc_t *app)
{
    char shown[64];
    const char *label = (app && app->display_name[0]) ? app->display_name : "app";

    lan_app_name(label, shown, sizeof(shown));
    if (shown[0] == '\0')
        snprintf(shown, sizeof(shown), "%s", label);
    lv_obj_t *obj = tablet_create_menu(parent, shown);
    if (!obj)
        return NULL;
    /* Icon is generated (gradient + glyph from app.json icon_* fields), not
     * loaded from the app.json "icon" image. */
    launcher_apply_generated_icon(lv_obj_get_child(obj, 0), app);
    return obj;
}

/** Build absolute path for dlopen when entry_so is relative to install_path. */
static void tablet_resolve_entry_so(const tablet_app_desc_t *app, char *out, size_t out_len)
{
    if (!app || !out || out_len == 0)
        return;
    out[0] = '\0';
    if (app->entry_so[0] == '\0')
        return;
    if (app->entry_so[0] == '/') {
        snprintf(out, out_len, "%s", app->entry_so);
        return;
    }
    if (app->install_path[0] == '\0') {
        snprintf(out, out_len, "%s", app->entry_so);
        return;
    }
    size_t n = strlen(app->install_path);
    if (app->install_path[n - 1] == '/')
        snprintf(out, out_len, "%s%s", app->install_path, app->entry_so);
    else
        snprintf(out, out_len, "%s/%s", app->install_path, app->entry_so);
}

/** Fallback: install_path/lib/<basename(entry_so)> when relative path dlopen fails. */
static void tablet_entry_so_lib_fallback(const tablet_app_desc_t *app, char *out, size_t out_len)
{
    if (!app || !out || out_len == 0)
        return;
    out[0] = '\0';
    if (app->install_path[0] == '\0' || app->entry_so[0] == '\0')
        return;
    const char *fn = strrchr(app->entry_so, '/');
    fn = fn ? fn + 1 : app->entry_so;
    if (fn[0] == '\0')
        return;
    size_t n = strlen(app->install_path);
    if (app->install_path[n - 1] == '/')
        snprintf(out, out_len, "%slib/%s", app->install_path, fn);
    else
        snprintf(out, out_len, "%s/lib/%s", app->install_path, fn);
}

/* Latest tap wins. The worker does not call LVGL; the GUI timer runs entry. */
static volatile int s_launch_seq;
static volatile int s_launch_ready_seq;
static const tablet_app_desc_t *s_launch_app;
static void *s_launch_so;
static lv_timer_t *s_launch_timer;

/* Online CPU count. sysconf follows this thread's affinity. */
static int launcher_read_online_cpus(cpu_set_t *set)
{
    FILE *fp;
    char buf[128];
    char *p;
    int n = 0;

    CPU_ZERO(set);
    fp = fopen("/sys/devices/system/cpu/online", "r");
    if (!fp)
        return 0;
    if (!fgets(buf, (int)sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    p = buf;
    while (*p) {
        int lo;
        int hi;
        int c;

        while (*p && (*p < '0' || *p > '9'))
            p++;
        if (!*p)
            break;
        lo = 0;
        while (*p >= '0' && *p <= '9') {
            lo = lo * 10 + (*p - '0');
            p++;
        }
        hi = lo;
        if (*p == '-') {
            p++;
            hi = 0;
            while (*p >= '0' && *p <= '9') {
                hi = hi * 10 + (*p - '0');
                p++;
            }
        }
        if (hi < lo)
            hi = lo;
        for (c = lo; c <= hi && c < CPU_SETSIZE; c++) {
            CPU_SET(c, set);
            n++;
        }
    }
    return n;
}

static void *launcher_dlopen_entry(const tablet_app_desc_t *app)
{
    char resolved[512] = {0};
    char fallback[512] = {0};
    void *handle = NULL;

    if (!app || app->entry_so[0] == '\0')
        return NULL;
    tablet_resolve_entry_so(app, resolved, sizeof(resolved));
    tablet_entry_so_lib_fallback(app, fallback, sizeof(fallback));
    if (resolved[0] != '\0') {
        handle = dlopen(resolved, RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
            log_warn("dlopen failed: so=%s err=%s", resolved, dlerror());
    }
    if (!handle && fallback[0] != '\0' &&
        (resolved[0] == '\0' || strcmp(fallback, resolved) != 0)) {
        (void)dlerror();
        handle = dlopen(fallback, RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
            log_warn("dlopen failed (lib fallback): so=%s err=%s", fallback, dlerror());
    }
    return handle;
}

static void launcher_finish_app_launch(void *user_data)
{
    const tablet_app_desc_t *app = (const tablet_app_desc_t *)user_data;
    lv_obj_t *main = s_ctx ? s_ctx->main : NULL;
    const char *fail_reason = "EMPTY_ENTRY_FIELDS";
    int (*entry_fn)(lv_obj_t *) = NULL;

    (void)tablet_font_ensure_full();
    if (!app || !main)
        return;

    if (s_launch_so && app->entry_symbol[0] != '\0') {
        entry_fn = (int (*)(lv_obj_t *))dlsym(s_launch_so, app->entry_symbol);
        if (!entry_fn) {
            fail_reason = "DLSYM_HANDLE_FAIL";
            log_warn("dlsym entry failed: symbol=%s err=%s", app->entry_symbol, dlerror());
        }
    }
    if (!entry_fn && app->entry_symbol[0] != '\0') {
        entry_fn = (int (*)(lv_obj_t *))dlsym(RTLD_DEFAULT, app->entry_symbol);
        if (!entry_fn) {
            fail_reason = "DLSYM_DEFAULT_FAIL";
            log_warn("dlsym default failed: symbol=%s err=%s", app->entry_symbol, dlerror());
        }
    }
    if (entry_fn) {
        if (entry_fn(main) == 0)
            lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(lv_display_get_default());
        return;
    }

    if (app->entry_so[0] != '\0' && app->entry_symbol[0] == '\0')
        fail_reason = "EMPTY_ENTRY_FIELDS";
    else if (!s_launch_so && app->entry_so[0] != '\0')
        fail_reason = "DLOPEN_FAIL";

    log_error("app launch failed: app_id=%s install_path=%s entry_so=%s entry_symbol=%s reason=%s",
              app->app_id,
              app->install_path[0] ? app->install_path : "(empty)",
              app->entry_so[0] ? app->entry_so : "(empty)",
              app->entry_symbol[0] ? app->entry_symbol : "(empty)",
              fail_reason);
    show_msgbox_note("app entry not found");
}

static void launcher_launch_timer_cb(lv_timer_t *timer)
{
    int seq = s_launch_seq;
    const tablet_app_desc_t *app;

    if (s_launch_ready_seq != seq)
        return;
    __sync_synchronize();
    app = s_launch_app;
    lv_timer_delete(timer);
    s_launch_timer = NULL;
    if (s_launch_seq == seq)
        s_launch_seq = seq + 1;
    launcher_finish_app_launch((void *)app);
}

static void *launcher_prepare_app_thread(void *arg)
{
    int seq = (int)(intptr_t)arg;
    const tablet_app_desc_t *app;
    void *so;

    if (seq != s_launch_seq)
        return NULL;
    app = s_launch_app;
    (void)app_main_open_multimedia_handle();
    (void)app_main_open_network_handle();
    so = launcher_dlopen_entry(app);
    if (seq != s_launch_seq)
        return NULL;
    s_launch_so = so;
    __sync_synchronize();
    if (seq != s_launch_seq)
        return NULL;
    s_launch_ready_seq = seq;
    return NULL;
}

static void event_callback_menu_app(lv_event_t *e)
{
    const tablet_app_desc_t *app = (const tablet_app_desc_t *)lv_event_get_user_data(e);
    if (!app)
        return;
    lv_obj_t *main = s_ctx ? s_ctx->main : NULL;
    if (!main)
        return;

    /* 进程型应用（exec）：fork+execl 执行 install_path 下的脚本/二进制后直接
     * 返回，不 dlopen 进本进程。不经 shell（无 sh -c 注入面）；双 fork+setsid
     * 等价原 sh 的 & 脱离——应用自己接管 fb/IPU/AO 并结束本进程（如 emu：
     * 杀桌面→游戏→退出拉回桌面）。
     * 脚本 stdout/stderr 落 /tmp 便于排查启动失败。
     * 连点去抖：一次触摸可能派生多次 clicked；exec 型应用启动即接管桌面，
     * 重复 exec 会让两个启动脚本互踩（后者秒退并错误地拉回桌面）。 */
    if (app->exec[0] != '\0') {
        static time_t s_last_exec_click;
        time_t now = time(NULL);
        if (s_last_exec_click != 0 && now - s_last_exec_click < 3) {
            log_warn("exec app re-click within 3s ignored: app_id=%s", app->app_id);
            return;
        }
        s_last_exec_click = now;
        char exec_path[512] = {0};
        if (app->exec[0] == '/') {
            snprintf(exec_path, sizeof(exec_path), "%s", app->exec);
        } else if (app->install_path[0] != '\0') {
            size_t n = strlen(app->install_path);
            if (app->install_path[n - 1] == '/')
                snprintf(exec_path, sizeof(exec_path), "%s%s", app->install_path, app->exec);
            else
                snprintf(exec_path, sizeof(exec_path), "%s/%s", app->install_path, app->exec);
        } else {
            snprintf(exec_path, sizeof(exec_path), "%s", app->exec);
        }
        if (access(exec_path, X_OK) != 0) {
            log_warn("exec app not runnable: path=%s errno=%d(%s)",
                     exec_path, errno, strerror(errno));
            return;
        }
        char log_path[256];
        snprintf(log_path, sizeof(log_path), "/tmp/app_exec_%s.log", app->app_id);
        pid_t pid = fork();
        if (pid < 0) {
            log_warn("exec app fork failed: errno=%d(%s)", errno, strerror(errno));
            return;
        }
        if (pid == 0) {
            /* 中间子进程：脱离会话再 fork 一次，孙进程被 init 收养（免收尸） */
            if (setsid() < 0)
                _exit(126);
            pid_t grand = fork();
            if (grand < 0)
                _exit(126);
            if (grand > 0)
                _exit(0);
            /* 孙进程：复刻原 `>log 2>&1` 重定向，stdin 接 /dev/null */
            int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                (void)dup2(log_fd, STDOUT_FILENO);
                (void)dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0) {
                (void)dup2(null_fd, STDIN_FILENO);
                close(null_fd);
            }
            execl(exec_path, exec_path, (char *)NULL);
            /* exec 失败：fork 后 stdio 缓冲不可靠，直接 write 原始 fd */
            {
                char emsg[128];
                int elen = snprintf(emsg, sizeof(emsg),
                                    "launcher: exec failed: %s errno=%d\n",
                                    exec_path, errno);
                if (elen > 0) {
                    ssize_t nwr = write(STDERR_FILENO, emsg, (size_t)elen);
                    (void)nwr;
                }
                _exit(127);
            }
        }
        /* 收中间子进程（立即退出，等待时间可忽略）；孙进程已脱离本进程 */
        {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
        }
        log_info("exec app launch: path=%s log=%s child=%d",
                 exec_path, log_path, (int)pid);
        return;
    }

    /* Return now so this click finishes. A later tap replaces the app. */
    s_launch_app = app;
    s_launch_ready_seq = 0;
    s_launch_seq++;
    if (s_launch_timer == NULL)
        s_launch_timer = lv_timer_create(launcher_launch_timer_cb, 20, NULL);
    log_info("app launch click: app_id=%s install_path=%s entry_so=%s entry_symbol=%s",
             app->app_id,
             app->install_path[0] ? app->install_path : "(empty)",
             app->entry_so[0] ? app->entry_so : "(empty)",
             app->entry_symbol[0] ? app->entry_symbol : "(empty)");

    /* dlopen off the GUI thread. Entry still runs here. */
    {
        pthread_t tid;
        pthread_attr_t attr;
        cpu_set_t online;
        cpu_set_t saved;
        int n;
        int widened = 0;
        int created;

        n = launcher_read_online_cpus(&online);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (n > 1 &&
            pthread_getaffinity_np(pthread_self(), sizeof(saved), &saved) == 0 &&
            pthread_setaffinity_np(pthread_self(), sizeof(online), &online) == 0)
            widened = 1;
        created = pthread_create(&tid, &attr, launcher_prepare_app_thread,
                                 (void *)(intptr_t)s_launch_seq);
        if (widened)
            (void)pthread_setaffinity_np(pthread_self(), sizeof(saved), &saved);
        pthread_attr_destroy(&attr);
        if (created == 0)
            return;
        log_warn("launcher: app prepare thread create failed");
    }

    (void)app_main_open_multimedia_handle();
    (void)app_main_open_network_handle();
    s_launch_so = launcher_dlopen_entry(app);
    if (s_launch_timer) {
        lv_timer_delete(s_launch_timer);
        s_launch_timer = NULL;
    }
    launcher_finish_app_launch((void *)app);
}

/**
* @brief   timer for timing task
* @param   [IN]timer: timer object
* @return  void
**/
static void second_classes_timer(lv_timer_t *timer)
{
    tablet_home_context_t *ctx = (tablet_home_context_t *)lv_timer_get_user_data(timer);

    tablet_time_update(&(ctx->DateTime));
    tablet_update_battery_indicator(ctx);

#if CONFIG_NETWORK_SUPPORTED
#if CONFIG_FW_NETWORK_WIFI
    lv_obj_t *label_wlan = ctx->top.label_wlan;
#endif

    static bool state = false;
    static uint32_t net_poll_tick = 0;
    static uint32_t weather_count = 0;
    static int s_ntp_thread_started = 0;
    bool connected = state;

    /* Start NTP only after post-home network_open (avoids early libnetwork relocate fail). */
    if (!s_ntp_thread_started && app_main_get_network() != NULL) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, ntp_background_sync_thread, NULL) == 0) {
            (void)pthread_detach(tid);
            s_ntp_thread_started = 1;
        }
    }

#if defined(CONFIG_APP_WEATHER)
    /* Apply weather text fetched by the worker thread (LVGL thread only). */
    tablet_weather_apply_pending_text();
#endif

    /* Poll iface/station every 5s (timer is 1s). */
    if ((net_poll_tick++ % 5u) == 0u)
        connected = network_is_connected();

    if (connected)
    {
        if (connected != state)
        {
#if CONFIG_FW_NETWORK_WIFI
            lv_obj_set_style_text_color(label_wlan, lv_color_hex(LAUNCHER_COLOR_INK), 0);
#endif
#if defined(CONFIG_APP_WEATHER)
            /* Net just became ready: fetch once right away. */
            weather_count = 0;
            tablet_weather_request_async();
#endif
        }

#if defined(CONFIG_APP_WEATHER)
        weather_count++;
        if (weather_count >= TABLET_WEATHER_POLL_SEC)
        {
            weather_count = 0;
            tablet_weather_request_async();
        }
#endif
    }
    else
    {
        if (connected != state)
        {
#if CONFIG_FW_NETWORK_WIFI
            lv_obj_set_style_text_color(label_wlan, lv_color_hex(LAUNCHER_COLOR_WIFI_OFF), 0);
#endif
        }
    }

    state = connected;
#endif
}

/* 静态菜单分发回调已移除：只保留 event_callback_menu_app(JSON-first) */

/**
 * @brief Deferred: QOI wallpaper + full fonts.
 */
static void tablet_home_deferred_images_cb(lv_timer_t *timer)
{
    tablet_home_context_t *ctx = (tablet_home_context_t *)lv_timer_get_user_data(timer);
    if (!ctx || !ctx->main)
        return;

    tablet_home_try_set_background(ctx->main);
    (void)tablet_font_ensure_full();
    {
        lv_font_t *f;

        if (ctx->DateTime.label_date_top && (f = tablet_font_get_freetype_font(LAUNCHER_FONT_DATE)))
            lv_obj_set_style_text_font(ctx->DateTime.label_date_top, f, 0);
        if (ctx->DateTime.label_date_day) {
            /* Bold day-of-month; falls back to normal weight if bold creation fails. */
            f = tablet_font_get_freetype_bold_font(LAUNCHER_FONT_DAY);
            if (!f)
                f = tablet_font_get_freetype_font(LAUNCHER_FONT_DAY);
            if (f)
                lv_obj_set_style_text_font(ctx->DateTime.label_date_day, f, 0);
        }
        if (ctx->DateTime.label_lunar && (f = tablet_font_get_freetype_font(LAUNCHER_FONT_LUNAR)))
            lv_obj_set_style_text_font(ctx->DateTime.label_lunar, f, 0);
        if (ctx->DateTime.label_weather && (f = tablet_font_get_freetype_font(LAUNCHER_FONT_WEATHER)))
            lv_obj_set_style_text_font(ctx->DateTime.label_weather, f, 0);
    }
    if (ctx->menu_flex) {
        lv_font_t *menu_font = tablet_font_get_freetype_font(LAUNCHER_FONT_MENU);
        uint32_t n = lv_obj_get_child_cnt(ctx->menu_flex);
        uint32_t i;
        for (i = 0; menu_font && i < n; i++) {
            lv_obj_t *cell = lv_obj_get_child(ctx->menu_flex, i);
            uint32_t cn = cell ? lv_obj_get_child_cnt(cell) : 0;
            lv_obj_t *label = (cn > 0) ? lv_obj_get_child(cell, cn - 1) : NULL;
            if (label)
                lv_obj_set_style_text_font(label, menu_font, 0);
        }
    }
}
