// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna watch-style digital clock: date, time, weather; NTP sync when online.
 */
#define LOG_TAG "WEARABLE_CLOCK"

#include "wearable_clock.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_globle_config.h"
#include "app_main.h"
#include "gui_common.h"
#include "gui_font.h"
#include "log.h"
#include "system_thread.h"

#if CONFIG_NETWORK_SUPPORTED
#include "network.h"
#include "weather_service.h"
#endif

#define WEARABLE_NTP_SERVER_IP          "203.107.6.88"
#define WEARABLE_NTP_SERVER_DEFAULT     "ntp.aliyun.com"
#define WEARABLE_NTP_POST_CONNECT_SEC   3
#define WEARABLE_NTP_RETRY_SEC          60
#define WEARABLE_NTP_RETRY_MAX_SEC      900   /* cap initial-sync backoff */
#define WEARABLE_NTP_RESYNC_SEC         3600
#define WEARABLE_WEATHER_POLL_SEC       900   /* 15 minutes */
#define WEARABLE_NET_CACHE_SEC          5     /* network state HAL cache TTL */
#define WEARABLE_WIFI_RETRY_SEC         2     /* status poll interval while waiting */
#define WEARABLE_WIFI_TIMEOUT_SEC       60    /* cover app.sh SDIO/aic8800 retries */
#define WEARABLE_WIFI_REASSOC_SEC       12    /* min gap between CONNECT ioctls */
#define WEARABLE_COLOR_ACCENT           0x05fbe2
#define WEARABLE_COLOR_MUTED            0x666666
#define WEARABLE_COLOR_CARD             0x161616
#define WEARABLE_COLOR_BAT_OK           0xeeeeee
#define WEARABLE_COLOR_BAT_LOW          0xff4d4d
/* Phone-style battery: SoC digits inside body (no '%' sign). */
#define WEARABLE_BAT_BODY_W             28
#define WEARABLE_BAT_BODY_H             14
#define WEARABLE_BAT_TIP_W              2
#define WEARABLE_BAT_TIP_H              7
#define WEARABLE_BAT_PAD                1
#define WEARABLE_BAT_FILL_MAX_W         (WEARABLE_BAT_BODY_W - 2 - 2 * WEARABLE_BAT_PAD)
/*
 * Battery SoC from SARADC AIN1 (ADC1):
 *   Vbat --1/2--> ADC pin.
 *   ak_saradc IIO in_voltageN_raw reports millivolts at the pin (not ADC codes);
 *   same units as vendor ps_ir thresholds (e.g. 900/400).
 *   Vbat_mV = adc_pin_mV * 2
 */
#define WEARABLE_BAT_ADC1_RAW_PATH      "/sys/bus/iio/devices/iio:device0/in_voltage1_raw"
#define WEARABLE_BAT_HW_DIVIDER         2
#define WEARABLE_BAT_EMPTY_MV           3000
#define WEARABLE_BAT_FULL_MV            4200
#define WEARABLE_BAT_EMA_SHIFT          2     /* pct = (3*prev + new) / 4 */
/* Full-screen simple wallpaper (240x240), not a clock-dial icon. */
#define WEARABLE_CLOCK_BG               "clock/wallpaper.png"
#define WEARABLE_CLOCK_BG_PX            240
/* WiFi status icon: 28x28 PNG on 240 panel (bold rings, easy to see). */
#define WEARABLE_WIFI_ICON              "status/wifi.png"
#define WEARABLE_WIFI_ICON_ON           "status/wifi_on.png"
#define WEARABLE_WIFI_ICON_PX           28
/* Shared top status strip height so weather / WiFi / battery share one midline. */
#define WEARABLE_STATUS_ROW_H           WEARABLE_WIFI_ICON_PX
/* China standard time (POSIX TZ); NTP sets UTC, localtime() needs TZ. */
#define WEARABLE_TZ_POSIX               "CST-8"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *image_bg;
    lv_obj_t *status_row;
    lv_obj_t *image_wifi;
    lv_obj_t *label_battery;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_body;
    lv_obj_t *battery_fill;
    lv_obj_t *battery_tip;
    lv_obj_t *label_date;
    lv_obj_t *label_time;
    lv_obj_t *label_sec;
    lv_obj_t *weather_row;
    lv_obj_t *image_weather;
    lv_obj_t *label_weather;
    lv_timer_t *timer;
    char image_path[128];
    char weather_text[128];
    uint32_t weather_tick;
    int net_was_connected;
    int wifi_icon_connected;
} wearable_clock_ctx_t;

static wearable_clock_ctx_t s_clock;

static void wearable_clock_update_time_labels(void);

static void wearable_clock_apply_timezone(void)
{
    if (setenv("TZ", WEARABLE_TZ_POSIX, 1) != 0) {
        log_warn("set TZ=%s failed", WEARABLE_TZ_POSIX);
        return;
    }
    tzset();
}

static lv_font_t *wearable_clock_font_digits(uint16_t weight, const lv_font_t *fallback)
{
    lv_font_t *font = gui_font_get_freetype_font(weight);

    return font ? font : (lv_font_t *)fallback;
}

#if CONFIG_NETWORK_SUPPORTED
/* Wall-clock-safe sleep: NTP settimeofday must not abort this wait. */
static void wearable_clock_sleep_monotonic(unsigned int sec)
{
    struct timespec req;

    if (sec == 0u)
        return;

    req.tv_sec = (time_t)sec;
    req.tv_nsec = 0;
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &req) != 0) {
        if (errno != EINTR)
            break;
    }
}

static bool wearable_clock_network_probe_connected(void)
{
    network_t *network;
    char ip[64];
    network_wifi_station_state_e state;

    network = (network_t *)app_main_get_network();
    if (!network || !network->wifi || !network->wifi->station)
        return false;

    ip[0] = '\0';
    if (network->wifi->station->get_ip_address(ip, sizeof(ip)) == NETWORK_OK) {
        if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
            return true;
    }

    if (network->wifi->station->get_state(&state) == NETWORK_OK &&
        state == NETWORK_WIFI_STATION_STATE_CONNECTED)
        return true;

    return false;
}

/* aic8800: need /dev/aic_cdev (fasync) + vnet0 before associate. */
static int wearable_clock_wifi_hw_ready(void)
{
    return access("/dev/aic_cdev", F_OK) == 0 &&
           access("/sys/class/net/vnet0", F_OK) == 0;
}

static void wearable_clock_network_try_connect(void)
{
    static int station_inited;
    static int hw_wait_logged;
    static time_t last_assoc_mono;
    network_t *network = (network_t *)app_main_get_network();
    network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
    struct timespec now_ts;
    time_t now_mono;

    if (!network || !network->wifi || !network->wifi->station)
        return;

    if (!wearable_clock_wifi_hw_ready()) {
        if (!hw_wait_logged) {
            log_warn("wifi boot: waiting for /dev/aic_cdev and vnet0");
            hw_wait_logged = 1;
        }
        return;
    }
    if (hw_wait_logged) {
        log_info("wifi boot: aic8800 ready");
        hw_wait_logged = 0;
    }

    if (!station_inited) {
        if (!network->wifi->station->init ||
            network->wifi->station->init() != NETWORK_OK) {
            log_warn("wifi station init not ready");
            return;
        }
        station_inited = 1;
    }

    /* Do not re-issue CONNECT while associating — it aborts in-flight DHCP. */
    if (network->wifi->station->get_state &&
        network->wifi->station->get_state(&state) == NETWORK_OK &&
        state == NETWORK_WIFI_STATION_STATE_CONNECTING) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    now_mono = now_ts.tv_sec;
    if (last_assoc_mono != 0 &&
        (now_mono - last_assoc_mono) < (time_t)WEARABLE_WIFI_REASSOC_SEC) {
        return;
    }

    if (network->wifi->station->connect_from_config) {
        log_info("wifi boot: associate from fw_wlan_config.ini");
        (void)network->wifi->station->connect_from_config();
        last_assoc_mono = now_mono;
    }
}

static void *wearable_clock_network_thread(void *arg)
{
    struct timespec t0, now;
    unsigned int elapsed_sec;

    (void)arg;
    (void)thread_set_current_name("clock_wifi");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    log_info("wifi boot connect: retry every %ds, timeout %ds",
             WEARABLE_WIFI_RETRY_SEC, WEARABLE_WIFI_TIMEOUT_SEC);

    for (;;) {
        wearable_clock_network_try_connect();
        if (wearable_clock_network_probe_connected()) {
            log_info("wifi boot connect: success");
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_sec = (unsigned int)(now.tv_sec - t0.tv_sec);
        if (elapsed_sec >= WEARABLE_WIFI_TIMEOUT_SEC) {
            log_warn("wifi boot connect: timeout after %us, stop retry",
                     WEARABLE_WIFI_TIMEOUT_SEC);
            break;
        }

        {
            unsigned int remain = WEARABLE_WIFI_TIMEOUT_SEC - elapsed_sec;
            unsigned int sleep_sec = remain < WEARABLE_WIFI_RETRY_SEC
                                         ? remain
                                         : WEARABLE_WIFI_RETRY_SEC;
            wearable_clock_sleep_monotonic(sleep_sec);
        }

        if (wearable_clock_network_probe_connected()) {
            log_info("wifi boot connect: success");
            break;
        }
    }

    return NULL;
}

static void wearable_clock_start_network_once(void)
{
    static int started;

    if (started) {
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, wearable_clock_network_thread, NULL) == 0) {
        (void)pthread_detach(tid);
        started = 1;
    } else {
        log_warn("wifi init thread create failed");
    }
}

static bool wearable_clock_network_connected(void)
{
    static volatile int s_cached = -1;
    static time_t s_last_check = 0;
    time_t now;
    bool result;

    now = time(NULL);
    if (s_cached >= 0 && (now - s_last_check) < WEARABLE_NET_CACHE_SEC)
        return (bool)s_cached;

    result = wearable_clock_network_probe_connected();
    s_cached = result ? 1 : 0;
    s_last_check = now;
    return result;
}

static int s_ntp_synced;
static pthread_mutex_t s_ntp_lock = PTHREAD_MUTEX_INITIALIZER;

static int wearable_clock_ntp_set_server(network_t *network, const char *server)
{
    char server_buf[64];

    if (!network || !network->ntp || !network->ntp->set_server || !server) {
        return -1;
    }

    snprintf(server_buf, sizeof(server_buf), "%s", server);
    return network->ntp->set_server(server_buf);
}

static void wearable_clock_try_ntp_sync(void)
{
    network_t *network = (network_t *)app_main_get_network();
    static const char *servers[] = {
        WEARABLE_NTP_SERVER_IP,
        WEARABLE_NTP_SERVER_DEFAULT,
        "cn.pool.ntp.org",
        NULL,
    };
    static unsigned int s_fail_log_skip;
    size_t i;
    int ret = -1;
    const char *used = NULL;
    int saw_resolve_fail = 0;

    if (s_ntp_synced) {
        return;
    }

    if (!wearable_clock_network_connected()) {
        return;
    }

    if (pthread_mutex_trylock(&s_ntp_lock) != 0) {
        return;
    }

    if (network && network->ntp && network->ntp->init) {
        (void)network->ntp->init();
    }

    for (i = 0; servers[i] != NULL; i++) {
        used = servers[i];
        ret = wearable_clock_ntp_set_server(network, used);
        if (ret == NETWORK_OK) {
            break;
        }
        /*
         * Hostname path uses getaddrinfo; one DNS failure usually means the
         * rest of the name list will burn ~30s each for the same result.
         */
        if (ret == NETWORK_ERROR_OPERATION_FAILED && i > 0) {
            saw_resolve_fail = 1;
            break;
        }
    }

    pthread_mutex_unlock(&s_ntp_lock);

    if (ret == NETWORK_OK) {
        s_ntp_synced = 1;
        s_fail_log_skip = 0;
        wearable_clock_apply_timezone();
        log_info("NTP sync ok: %s (TZ=%s)", used ? used : "unknown", WEARABLE_TZ_POSIX);
        if (s_clock.timer) {
            wearable_clock_update_time_labels();
        }
    } else if (used) {
        /* Rate-limit: first failure + every 5th thereafter. */
        if (s_fail_log_skip == 0) {
            log_warn("NTP sync failed: %s ret=%d%s", used, ret,
                     saw_resolve_fail ? " (DNS)" : "");
        }
        s_fail_log_skip++;
        if (s_fail_log_skip >= 5)
            s_fail_log_skip = 0;
    }
}

static void *wearable_clock_ntp_thread(void *arg)
{
    unsigned int fail_backoff_sec = WEARABLE_NTP_RETRY_SEC;

    (void)arg;

    /* Raw pthread: name it so top -H shows the clock module instead of the
     * inherited "LunaGui" name. */
    (void)thread_set_current_name("clock_ntp");

    /*
     * Must not use sleep()/usleep() here: they follow CLOCK_REALTIME on this
     * uClibc. ntp_set() calls settimeofday() and jumps the wall clock from the
     * unset boot time (~1970) to real UTC; any in-flight or subsequent REALTIME
     * sleep then returns immediately → this thread busy-loops at ~100% CPU
     * (top shows {clock_ntp}), starving VI capture → CHN[0] select timeout.
     * Use wearable_clock_sleep_monotonic() instead.
     */
    for (;;) {
        if (!wearable_clock_network_connected()) {
            wearable_clock_sleep_monotonic(5);
            continue;
        }

        if (!s_ntp_synced) {
            wearable_clock_sleep_monotonic(WEARABLE_NTP_POST_CONNECT_SEC);
            wearable_clock_try_ntp_sync();
            if (!s_ntp_synced) {
                wearable_clock_sleep_monotonic(fail_backoff_sec);
                if (fail_backoff_sec < WEARABLE_NTP_RETRY_MAX_SEC) {
                    unsigned int next = fail_backoff_sec * 2u;

                    fail_backoff_sec = next > WEARABLE_NTP_RETRY_MAX_SEC
                                           ? WEARABLE_NTP_RETRY_MAX_SEC
                                           : next;
                }
            } else {
                fail_backoff_sec = WEARABLE_NTP_RETRY_SEC;
            }
            continue;
        }

        wearable_clock_sleep_monotonic(WEARABLE_NTP_RESYNC_SEC);
        /*
         * Periodic re-sync. On failure keep the previous successful time and
         * wait another RESYNC interval — do not drop into the 60s retry loop
         * (that used to spam ntp/DNS errors forever after the first hourly try).
         */
        s_ntp_synced = 0;
        wearable_clock_try_ntp_sync();
        if (!s_ntp_synced) {
            s_ntp_synced = 1;
            log_warn("NTP re-sync failed, keep previous time; retry in %ds",
                     WEARABLE_NTP_RESYNC_SEC);
        }
    }
    return NULL;
}

static void wearable_clock_start_ntp_once(void)
{
    static int started;

    if (started) {
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, wearable_clock_ntp_thread, NULL) == 0) {
        (void)pthread_detach(tid);
        started = 1;
    } else {
        log_warn("ntp sync thread create failed");
    }
}

static void wearable_clock_update_wifi_indicator(int connected)
{
    const char *src;

    if (!s_clock.image_wifi) {
        return;
    }

    /* Same load path as weather icons. */
    src = connected ? WEARABLE_WIFI_ICON_ON : WEARABLE_WIFI_ICON;
    if (s_clock.wifi_icon_connected != connected ||
        lv_obj_has_flag(s_clock.image_wifi, LV_OBJ_FLAG_HIDDEN)) {
        if (gui_image_set_src(s_clock.image_wifi, src) == LV_RESULT_OK) {
            lv_obj_remove_flag(s_clock.image_wifi, LV_OBJ_FLAG_HIDDEN);
            s_clock.wifi_icon_connected = connected;
        } else if (gui_image_set_src(s_clock.image_wifi, WEARABLE_WIFI_ICON) == LV_RESULT_OK) {
            lv_obj_remove_flag(s_clock.image_wifi, LV_OBJ_FLAG_HIDDEN);
            log_warn("wifi_on icon missing, use %s", WEARABLE_WIFI_ICON);
        } else {
            log_warn("wifi icon missing (%s)", src);
        }
    }
    lv_obj_invalidate(s_clock.image_wifi);
}
#endif

/* Read battery SoC: prefer power_supply capacity, else ADC1 + OCV table. */
static int wearable_clock_read_sysfs_capacity(void)
{
    static const char *paths[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/bms/capacity",
        NULL,
    };
    size_t i;

    for (i = 0; paths[i] != NULL; i++) {
        FILE *fp = fopen(paths[i], "r");
        int pct = -1;

        if (!fp)
            continue;
        if (fscanf(fp, "%d", &pct) == 1) {
            fclose(fp);
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
            return pct;
        }
        fclose(fp);
    }
    return -1;
}

static int wearable_clock_read_adc1_pin_mv(void)
{
    FILE *fp;
    int mv = -1;

    fp = fopen(WEARABLE_BAT_ADC1_RAW_PATH, "r");
    if (!fp)
        return -1;
    if (fscanf(fp, "%d", &mv) != 1)
        mv = -1;
    fclose(fp);
    /* Pin after 1/2 divider: expect roughly 1500..2100 mV for a Li-ion pack. */
    if (mv < 0 || mv > 5000)
        return -1;
    return mv;
}

static int wearable_clock_vbat_mv_from_adc_pin_mv(int pin_mv)
{
    return pin_mv * WEARABLE_BAT_HW_DIVIDER;
}

/*
 * Li-ion OCV curve (resting). Nominal cell 3.7V ≈ 50%.
 * Points are (Vbat_mV, percent), descending voltage.
 */
static int wearable_clock_percent_from_vbat_mv(int mv)
{
    static const struct {
        int mv;
        int pct;
    } ocv[] = {
        { WEARABLE_BAT_FULL_MV, 100 },
        { 4100, 90 },
        { 4000, 80 },
        { 3920, 70 },
        { 3850, 60 },
        { 3780, 55 },
        { 3700, 50 }, /* nominal */
        { 3620, 40 },
        { 3500, 25 },
        { 3400, 15 },
        { 3300, 8 },
        { WEARABLE_BAT_EMPTY_MV, 0 },
    };
    size_t n = sizeof(ocv) / sizeof(ocv[0]);
    size_t i;

    if (mv >= ocv[0].mv)
        return 100;
    if (mv <= ocv[n - 1].mv)
        return 0;

    for (i = 0; i + 1 < n; i++) {
        if (mv <= ocv[i].mv && mv >= ocv[i + 1].mv) {
            int mv_hi = ocv[i].mv;
            int mv_lo = ocv[i + 1].mv;
            int pct_hi = ocv[i].pct;
            int pct_lo = ocv[i + 1].pct;
            int span = mv_hi - mv_lo;

            if (span <= 0)
                return pct_lo;
            return pct_lo + (pct_hi - pct_lo) * (mv - mv_lo) / span;
        }
    }
    return 0;
}

static int wearable_clock_read_battery_percent(void)
{
    static int s_pct_filt = -1;
    static int s_logged_adc = 0;
    int pct;
    int pin_mv;
    int mv;

    pct = wearable_clock_read_sysfs_capacity();
    if (pct >= 0) {
        s_pct_filt = pct;
        return pct;
    }

    pin_mv = wearable_clock_read_adc1_pin_mv();
    if (pin_mv < 0) {
        s_pct_filt = -1;
        return -1;
    }

    mv = wearable_clock_vbat_mv_from_adc_pin_mv(pin_mv);
    pct = wearable_clock_percent_from_vbat_mv(mv);

    if (!s_logged_adc) {
        log_info("battery ADC1 pin=%dmV Vbat=%dmV SoC=%d%%", pin_mv, mv, pct);
        s_logged_adc = 1;
    }

    if (s_pct_filt < 0) {
        s_pct_filt = pct;
    } else {
        /* Light EMA to damp 1 Hz UI flicker from ADC noise. */
        s_pct_filt = ((s_pct_filt * ((1 << WEARABLE_BAT_EMA_SHIFT) - 1)) + pct) >>
                     WEARABLE_BAT_EMA_SHIFT;
    }
    if (s_pct_filt < 0)
        s_pct_filt = 0;
    if (s_pct_filt > 100)
        s_pct_filt = 100;
    return s_pct_filt;
}

static void wearable_clock_update_battery_indicator(void)
{
    int pct;
    int fill_w;
    uint32_t frame_color = WEARABLE_COLOR_BAT_OK;
    uint32_t fill_color = 0x3a3a3a;

    if (!s_clock.label_battery || !s_clock.battery_fill)
        return;

    pct = wearable_clock_read_battery_percent();
    if (pct >= 0) {
        lv_label_set_text_fmt(s_clock.label_battery, "%d", pct);
        fill_w = (WEARABLE_BAT_FILL_MAX_W * pct + 50) / 100;
        if (pct > 0 && fill_w < 1)
            fill_w = 1;
        if (pct <= 20) {
            frame_color = WEARABLE_COLOR_BAT_LOW;
            fill_color = 0x7a2020;
        } else {
            fill_color = 0x4a4a4a;
        }
    } else {
        lv_label_set_text(s_clock.label_battery, "--");
        fill_w = 0;
    }
    lv_obj_center(s_clock.label_battery);

    lv_obj_set_width(s_clock.battery_fill, fill_w);
    lv_obj_set_style_bg_color(s_clock.battery_fill, lv_color_hex(fill_color), 0);
    lv_obj_set_style_text_color(s_clock.label_battery, lv_color_hex(frame_color), 0);
    if (s_clock.battery_body) {
        lv_obj_set_style_border_color(s_clock.battery_body,
                                      lv_color_hex(frame_color), 0);
    }
    if (s_clock.battery_tip) {
        lv_obj_set_style_bg_color(s_clock.battery_tip, lv_color_hex(frame_color), 0);
    }
}

#if CONFIG_NETWORK_SUPPORTED
static int wearable_clock_update_weather_ui(const char *image_path, const char *label_text)
{
    if (image_path) {
        snprintf(s_clock.image_path, sizeof(s_clock.image_path), "%s", image_path);
    }
    if (label_text) {
        snprintf(s_clock.weather_text, sizeof(s_clock.weather_text), "%s", label_text);
    }

    if (s_clock.image_weather && s_clock.image_path[0] != '\0') {
        if (gui_image_set_src(s_clock.image_weather, s_clock.image_path) == LV_RESULT_OK) {
            lv_obj_remove_flag(s_clock.image_weather, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_invalidate(s_clock.image_weather);
    }
    if (s_clock.label_weather && s_clock.weather_text[0] != '\0') {
        lv_label_set_text(s_clock.label_weather, s_clock.weather_text);
        lv_obj_invalidate(s_clock.label_weather);
    }
    return 0;
}

static void wearable_clock_request_weather(void)
{
    if (!wearable_clock_network_connected()) {
        return;
    }
    (void)app_weather_update_request(wearable_clock_update_weather_ui);
}
#endif

static void wearable_clock_update_time_labels(void)
{
    time_t rawtime;
    struct tm *timeinfo;
    static const char *weekdays[] = {
        /* Explicit \uXXXX avoids editor encoding glitches on「四」. */
        "\u661f\u671f\u65e5", /* 星期日 */
        "\u661f\u671f\u4e00", /* 星期一 */
        "\u661f\u671f\u4e8c", /* 星期二 */
        "\u661f\u671f\u4e09", /* 星期三 */
        "\u661f\u671f\u56db", /* 星期四 */
        "\u661f\u671f\u4e94", /* 星期五 */
        "\u661f\u671f\u516d", /* 星期六 */
    };

    if (!s_clock.label_time || !s_clock.label_date) {
        return;
    }

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (!timeinfo) {
        return;
    }

    lv_label_set_text_fmt(s_clock.label_time, "%02d:%02d",
                          timeinfo->tm_hour, timeinfo->tm_min);
    if (s_clock.label_sec) {
        lv_label_set_text_fmt(s_clock.label_sec, "%02d", timeinfo->tm_sec);
    }
    /* Trailing thin space:「四」glyph is taller/wider than「三」, avoid clip/garbled. */
    lv_label_set_text_fmt(s_clock.label_date, "%s\u2009  %02d/%02d",
                          weekdays[timeinfo->tm_wday],
                          timeinfo->tm_mon + 1, timeinfo->tm_mday);
    lv_obj_align(s_clock.label_date, LV_ALIGN_TOP_MID, 0, 66);
}

static void wearable_clock_on_timer(lv_timer_t *timer)
{
    (void)timer;

    wearable_clock_update_time_labels();
    wearable_clock_update_battery_indicator();

#if CONFIG_NETWORK_SUPPORTED
    int connected = wearable_clock_network_connected() ? 1 : 0;

    wearable_clock_update_wifi_indicator(connected);

    if (connected && !s_clock.net_was_connected) {
        wearable_clock_request_weather();
        s_clock.weather_tick = 0;
    }

    if (connected) {
        s_clock.weather_tick++;
        if ((s_clock.weather_tick % WEARABLE_WEATHER_POLL_SEC) == 0) {
            wearable_clock_request_weather();
        }
    } else if (s_clock.net_was_connected) {
        s_clock.weather_tick = 0;
        if (s_clock.label_weather) {
            lv_label_set_text(s_clock.label_weather, "--");
        }
        if (s_clock.image_weather) {
            lv_obj_add_flag(s_clock.image_weather, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_clock.net_was_connected = connected;
#endif
}

static void wearable_clock_on_delete(lv_event_t *e)
{
    (void)e;
    if (s_clock.timer) {
        lv_timer_delete(s_clock.timer);
        s_clock.timer = NULL;
    }
    memset(&s_clock, 0, sizeof(s_clock));
}

lv_obj_t *wearable_clock_create(lv_obj_t *parent)
{
    lv_font_t *font_time;
    lv_font_t *font_sec;
    lv_font_t *font_date;
#if CONFIG_NETWORK_SUPPORTED
    lv_font_t *font_weather;
#endif

    if (!parent) {
        log_error("null parent");
        return NULL;
    }

    memset(&s_clock, 0, sizeof(s_clock));
    wearable_clock_apply_timezone();

    font_time = wearable_clock_font_digits(54, &lv_font_montserrat_48);
    font_sec = wearable_clock_font_digits(22, &lv_font_montserrat_24);
    /* 16px:「四」bitmap at 18 often clips on 240 row and looks garbled. */
    font_date = gui_font_get_cjk_font(16);
    if (!font_date) {
        font_date = wearable_clock_font_digits(16, &lv_font_montserrat_16);
    }

    s_clock.root = lv_obj_create(parent);
    lv_obj_set_size(s_clock.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_clock.root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock.root, 0, 0);
    lv_obj_set_style_pad_all(s_clock.root, 0, 0);
    lv_obj_remove_flag(s_clock.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_clock.root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(s_clock.root, wearable_clock_on_delete, LV_EVENT_DELETE, NULL);

    /* Full-screen simple wallpaper (not clock-dial icon). */
    s_clock.image_bg = lv_image_create(s_clock.root);
    lv_obj_set_size(s_clock.image_bg, WEARABLE_CLOCK_BG_PX, WEARABLE_CLOCK_BG_PX);
    lv_obj_align(s_clock.image_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_clock.image_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(s_clock.image_bg);
    if (gui_image_set_src(s_clock.image_bg, WEARABLE_CLOCK_BG) == LV_RESULT_OK) {
        log_info("clock wallpaper: %s", WEARABLE_CLOCK_BG);
    } else {
        lv_obj_add_flag(s_clock.image_bg, LV_OBJ_FLAG_HIDDEN);
        /* Fallback: solid dark fill on root if wallpaper missing. */
        lv_obj_set_style_bg_color(s_clock.root, lv_color_hex(0x0c0e12), 0);
        lv_obj_set_style_bg_opa(s_clock.root, LV_OPA_COVER, 0);
        log_warn("clock wallpaper missing (%s), use solid bg", WEARABLE_CLOCK_BG);
    }

#if CONFIG_NETWORK_SUPPORTED
    /* Top-left: weather icon + short text */
    font_weather = gui_font_get_cjk_font(16);
    if (!font_weather) {
        font_weather = wearable_clock_font_digits(14, &lv_font_montserrat_14);
    }

    s_clock.weather_row = lv_obj_create(s_clock.root);
    lv_obj_set_size(s_clock.weather_row, LV_SIZE_CONTENT, WEARABLE_STATUS_ROW_H);
    lv_obj_set_style_bg_opa(s_clock.weather_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock.weather_row, 0, 0);
    lv_obj_set_style_pad_all(s_clock.weather_row, 0, 0);
    lv_obj_set_style_pad_column(s_clock.weather_row, 10, 0);
    lv_obj_set_flex_flow(s_clock.weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock.weather_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_clock.weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock.weather_row, LV_ALIGN_TOP_LEFT, 12, 8);

    s_clock.image_weather = lv_image_create(s_clock.weather_row);
    lv_obj_set_size(s_clock.image_weather, 22, 22);
    lv_image_set_scale(s_clock.image_weather, 64);
    lv_obj_add_flag(s_clock.image_weather, LV_OBJ_FLAG_HIDDEN);

    s_clock.label_weather = lv_label_create(s_clock.weather_row);
    lv_obj_set_style_text_font(s_clock.label_weather, font_weather, 0);
    lv_obj_set_style_text_color(s_clock.label_weather, lv_color_hex(0xdddddd), 0);
    lv_label_set_long_mode(s_clock.label_weather, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_clock.label_weather, 72);
    lv_label_set_text(s_clock.label_weather, "--");
#endif

    /*
     * Top-right: battery in a fixed-height strip (same Y/H as weather).
     * WiFi is a sibling on root (absolute align); see below.
     */
    s_clock.status_row = lv_obj_create(s_clock.root);
    lv_obj_set_size(s_clock.status_row, LV_SIZE_CONTENT, WEARABLE_STATUS_ROW_H);
    lv_obj_set_style_bg_opa(s_clock.status_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock.status_row, 0, 0);
    lv_obj_set_style_pad_all(s_clock.status_row, 0, 0);
    lv_obj_set_flex_flow(s_clock.status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock.status_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_clock.status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock.status_row, LV_ALIGN_TOP_RIGHT, -6, 8);

#if CONFIG_NETWORK_SUPPORTED
    /*
     * WiFi on root with absolute TOP_RIGHT (same path that previously painted).
     * Do NOT align_to(battery): during create the flex parent may still be 0-sized,
     * which pushes the icon off-screen. Y=8 matches WEARABLE_STATUS_ROW_H midline.
     * X=-42: 28px icon + ~4px gap left of the ~30px battery at -6.
     */
    s_clock.image_wifi = lv_image_create(s_clock.root);
    lv_obj_set_size(s_clock.image_wifi, WEARABLE_WIFI_ICON_PX, WEARABLE_WIFI_ICON_PX);
    lv_obj_align(s_clock.image_wifi, LV_ALIGN_TOP_RIGHT, -42, 8);
    s_clock.wifi_icon_connected = -1;
    if (gui_image_set_src(s_clock.image_wifi, WEARABLE_WIFI_ICON) == LV_RESULT_OK) {
        log_info("wifi icon ok: %s/%s @ TOP_RIGHT(-42,8)",
                 RES_IMAGE_PATH, WEARABLE_WIFI_ICON);
    } else {
        lv_obj_add_flag(s_clock.image_wifi, LV_OBJ_FLAG_HIDDEN);
        log_warn("wifi icon missing: %s/%s (flash status/)",
                 RES_IMAGE_PATH, WEARABLE_WIFI_ICON);
    }
#endif

    /* Battery: outline + fill + percent text centered inside body + tip. */
    s_clock.battery_icon = lv_obj_create(s_clock.status_row);
    lv_obj_set_size(s_clock.battery_icon,
                    WEARABLE_BAT_BODY_W + WEARABLE_BAT_TIP_W,
                    WEARABLE_BAT_BODY_H);
    lv_obj_set_style_bg_opa(s_clock.battery_icon, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock.battery_icon, 0, 0);
    lv_obj_set_style_pad_all(s_clock.battery_icon, 0, 0);
    lv_obj_remove_flag(s_clock.battery_icon, LV_OBJ_FLAG_SCROLLABLE);

    s_clock.battery_body = lv_obj_create(s_clock.battery_icon);
    lv_obj_set_size(s_clock.battery_body, WEARABLE_BAT_BODY_W, WEARABLE_BAT_BODY_H);
    lv_obj_set_style_bg_opa(s_clock.battery_body, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock.battery_body, 1, 0);
    lv_obj_set_style_border_color(s_clock.battery_body, lv_color_hex(WEARABLE_COLOR_BAT_OK), 0);
    lv_obj_set_style_radius(s_clock.battery_body, 3, 0);
    lv_obj_set_style_pad_all(s_clock.battery_body, WEARABLE_BAT_PAD, 0);
    lv_obj_remove_flag(s_clock.battery_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock.battery_body, LV_ALIGN_LEFT_MID, 0, 0);

    s_clock.battery_fill = lv_obj_create(s_clock.battery_body);
    lv_obj_set_size(s_clock.battery_fill, 0, LV_PCT(100));
    lv_obj_set_style_bg_color(s_clock.battery_fill, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(s_clock.battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_clock.battery_fill, 0, 0);
    lv_obj_set_style_radius(s_clock.battery_fill, 1, 0);
    lv_obj_set_style_pad_all(s_clock.battery_fill, 0, 0);
    lv_obj_remove_flag(s_clock.battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock.battery_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_clock.label_battery = lv_label_create(s_clock.battery_body);
    lv_obj_set_style_text_font(s_clock.label_battery, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_clock.label_battery, lv_color_hex(WEARABLE_COLOR_BAT_OK), 0);
    lv_obj_set_style_text_letter_space(s_clock.label_battery, -1, 0);
    lv_label_set_text(s_clock.label_battery, "--");
    lv_obj_center(s_clock.label_battery);
    lv_obj_move_foreground(s_clock.label_battery);

    s_clock.battery_tip = lv_obj_create(s_clock.battery_icon);
    lv_obj_set_size(s_clock.battery_tip, WEARABLE_BAT_TIP_W, WEARABLE_BAT_TIP_H);
    lv_obj_set_style_bg_color(s_clock.battery_tip, lv_color_hex(WEARABLE_COLOR_BAT_OK), 0);
    lv_obj_set_style_bg_opa(s_clock.battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_clock.battery_tip, 0, 0);
    lv_obj_set_style_radius(s_clock.battery_tip, 1, 0);
    lv_obj_set_style_pad_all(s_clock.battery_tip, 0, 0);
    lv_obj_remove_flag(s_clock.battery_tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock.battery_tip, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Single CJK line; pad so taller「四」is not clipped (looks like garbage). */
    s_clock.label_date = lv_label_create(s_clock.root);
    lv_obj_set_style_text_font(s_clock.label_date, font_date, 0);
    lv_obj_set_style_text_color(s_clock.label_date, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_align(s_clock.label_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(s_clock.label_date, 8, 0);
    lv_obj_set_style_pad_ver(s_clock.label_date, 4, 0);
    lv_obj_add_flag(s_clock.label_date, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_label_set_text(s_clock.label_date, "\u661f\u671f\u4e00  01/01");
    lv_obj_align(s_clock.label_date, LV_ALIGN_TOP_MID, 0, 66);

    /* Time pulled lower on the 240x240 face */
    s_clock.label_time = lv_label_create(s_clock.root);
    lv_obj_set_style_text_font(s_clock.label_time, font_time, 0);
    lv_obj_set_style_text_color(s_clock.label_time, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_letter_space(s_clock.label_time, 1, 0);
    lv_label_set_text(s_clock.label_time, "00:00");
    lv_obj_align(s_clock.label_time, LV_ALIGN_CENTER, -8, 18);

    s_clock.label_sec = lv_label_create(s_clock.root);
    lv_obj_set_style_text_font(s_clock.label_sec, font_sec, 0);
    lv_obj_set_style_text_color(s_clock.label_sec, lv_color_hex(WEARABLE_COLOR_ACCENT), 0);
    lv_label_set_text(s_clock.label_sec, "00");
    lv_obj_align_to(s_clock.label_sec, s_clock.label_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -2);

#if CONFIG_NETWORK_SUPPORTED
    wearable_clock_start_network_once();
    wearable_clock_start_ntp_once();
#endif

    s_clock.timer = lv_timer_create(wearable_clock_on_timer, 1000, NULL);
    lv_timer_ready(s_clock.timer);
    wearable_clock_update_time_labels();
    wearable_clock_on_timer(NULL);

    if (s_clock.image_bg)
        lv_obj_move_background(s_clock.image_bg);
    if (s_clock.label_date)
        lv_obj_move_foreground(s_clock.label_date);
    if (s_clock.label_time)
        lv_obj_move_foreground(s_clock.label_time);
    if (s_clock.label_sec)
        lv_obj_move_foreground(s_clock.label_sec);
    if (s_clock.status_row)
        lv_obj_move_foreground(s_clock.status_row);
#if CONFIG_NETWORK_SUPPORTED
    if (s_clock.image_wifi)
        lv_obj_move_foreground(s_clock.image_wifi);
    if (s_clock.weather_row)
        lv_obj_move_foreground(s_clock.weather_row);
#endif

    return s_clock.root;
}

void wearable_clock_set_suspended(int suspended)
{
    if (!s_clock.timer)
        return;

    if (suspended)
        lv_timer_pause(s_clock.timer);
    else
        lv_timer_resume(s_clock.timer);
}
