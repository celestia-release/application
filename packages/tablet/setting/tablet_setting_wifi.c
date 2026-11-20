// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo setting WiFi screen.
 */
#define LOG_TAG "TABLET_SETTING_WIFI"

#include "config.h"        /* CONFIG_FW_NETWORK_WIFI (project) */
#include "system_config.h" /* config_build_file_path */

#ifdef CONFIG_FW_NETWORK_WIFI
#include "tablet_setting_wifi.h"
#include "tablet_setting.h"
#include "tablet_font.h"
#include "app_main.h"
#include "multimedia.h"
#include "log.h"
#include <stdio.h>
#define log_info_app log_info
#define log_warn_app log_warn

#include "network.h"
#include "gthread.h"
#include "zxing_scan.h"
#include "system_time.h"
#include "iniparser.h"
#include "cJSON.h"
#include "wifi_cred_crypto.h"

enum nb_link_state
{
	LINK_STATE_QUIT = 0,
	LINK_STATE_INIT = 1,
	LINK_STATE_RESET = 2,
	LINK_STATE_QR_OR_WAV_INFO_GETTING = 3,
	LINK_STATE_RECV_QR_OR_WAV_INFO = 4,
	LINK_STATE_WIFI_CONNECTING = 5,
	LINK_STATE_SERVER_CONNECTING = 6,
	LINK_STATE_SERVER_CONNECTED = 7,
};
#define WIFI_SSID_LEN  33
#define WIFI_PWD_LEN  80

typedef struct
{
    char ssid[WIFI_SSID_LEN];   /* WiFi SSID */
    int  ssidLen;
    char pwd[WIFI_PWD_LEN];    /* WiFi password */
    int  pwdLen;
    char extension[256];       /* Extension field */
    int extensionLen;
}wifi_info_t;

static wifi_info_t g_connect_info = {0};

static enum nb_link_state eLinkState = LINK_STATE_RESET;

static network_t *s_wifi_network = NULL; /* from app_main_get_network() */

static lv_obj_t* win_left = NULL;
static lv_obj_t* wifi_win_main = NULL;
static lv_obj_t* wifi_win_ssid = NULL;
static lv_obj_t* wifi_lable_status = NULL;
static lv_obj_t* wifi_lable_connect = NULL;
static lv_obj_t* ssid_ta = NULL, *pwd_ta = NULL;
/* Keyboard active flag: when user is typing, ignore long-press/click actions on WiFi list. */
static volatile int s_wifi_keyboard_active = 0;

static network_wifi_scan_result_t g_wifi_list[32];
static lv_obj_t *g_wifi_list_ctrl[32] = {0};
int g_wifi_list_cnt = 0;
/** Shown when scan returns 0 APs; hidden when list has items */
static lv_obj_t *s_no_networks_label = NULL;
static lv_obj_t *s_wifi_driver_hint = NULL;
static lv_obj_t *s_wifi_list_title = NULL;

/* Async WiFi scan timer to avoid blocking GUI thread (system_sleep_ms) */
static lv_timer_t *s_wifi_scan_timer = NULL;
static int s_wifi_scan_try_cnt = 0;
static int s_wifi_scan_try_max = 8;
static void wifi_scan_async_timer_cb(lv_timer_t *timer);
static int s_auto_connect_tried = 0;
/* Tracks settings-side ensure_init; boot may already init when settings.json wifi.enable=1 */
static int s_wifi_station_inited = 0;
static int iTimeOut = 20;  /* Timeout in seconds */
static int iTimeSec = 0;
static lv_timer_t* work_timer = NULL;
static lv_obj_t* win_connecting = NULL;
static lv_obj_t* win_msg_txt = NULL, *win_msg_close_btn = NULL;
static int start_connect(wifi_info_t *pInfo, char *msg_buf, int len);
static void connect_async_task(lv_timer_t * timer);

#define APP_SETTINGS_CONFIG_PATH "/etc/config/settings.json"

/* settings.json "wifi":{"enable","ssid","psk","auto_connect"} — same object/keys
 * (enable/ssid/psk) that framework/services/network/network.c's connect_from_config
 * already reads; "auto_connect" is added alongside for the settings-UI "remember
 * this network" preference, which is a distinct concept from the board-level
 * wifi.enable capability gate (was hal_wifi_config.ini [station] enable). */
static cJSON *app_settings_load_root(void)
{
    FILE *fp = fopen(APP_SETTINGS_CONFIG_PATH, "rb");
    long size;
    size_t n;
    char *buf;
    cJSON *root;

    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size <= 0 || size > (1024 * 1024)) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static int app_settings_save_root(cJSON *root)
{
    char *printed;
    FILE *fp;

    if (!root)
        return -1;
    printed = cJSON_Print(root);
    if (!printed)
        return -1;
    fp = fopen(APP_SETTINGS_CONFIG_PATH, "wb");
    if (!fp) {
        free(printed);
        return -1;
    }
    fputs(printed, fp);
    fclose(fp);
    free(printed);
    return 0;
}

static int wifi_load_saved_credentials(wifi_info_t *info)
{
    cJSON *root, *wifi, *auto_connect, *ssid, *psk;
    int found = 0;

    if (!info)
        return -1;
    memset(info, 0, sizeof(*info));

    root = app_settings_load_root();
    if (!root)
        return -1;

    wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        auto_connect = cJSON_GetObjectItem(wifi, "auto_connect");
        ssid = cJSON_GetObjectItem(wifi, "ssid");
        psk = cJSON_GetObjectItem(wifi, "psk");
        if (cJSON_IsNumber(auto_connect) && auto_connect->valueint &&
            cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0] != '\0') {
            strncpy(info->ssid, ssid->valuestring, sizeof(info->ssid) - 1);
            if (cJSON_IsString(psk) && psk->valuestring &&
                wifi_psk_decrypt(psk->valuestring, info->pwd, sizeof(info->pwd)) != 0)
                info->pwd[0] = '\0';
            info->ssidLen = (int)strlen(info->ssid);
            info->pwdLen = (int)strlen(info->pwd);
            found = 1;
        }
    }
    cJSON_Delete(root);
    return found ? 0 : -1;
}

int tablet_setting_wifi_save_sta(const char *ssid, const char *psk)
{
    cJSON *root, *wifi;
    char enc_psk[196] = {0};
    int ret;

    if (!ssid || ssid[0] == '\0')
        return -1;

    root = app_settings_load_root();
    if (!root)
        root = cJSON_CreateObject();
    if (!root) {
        log_warn_app("wifi save: cannot create %s root", APP_SETTINGS_CONFIG_PATH);
        return -1;
    }

    wifi = cJSON_GetObjectItem(root, "wifi");
    if (!cJSON_IsObject(wifi)) {
        cJSON_DeleteItemFromObject(root, "wifi");
        wifi = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "wifi", wifi);
    }

    if (psk && psk[0] != '\0') {
        if (wifi_psk_encrypt(psk, enc_psk, sizeof(enc_psk)) != 0)
            enc_psk[0] = '\0';
    }

    cJSON_DeleteItemFromObject(wifi, "ssid");
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    cJSON_DeleteItemFromObject(wifi, "psk");
    cJSON_AddStringToObject(wifi, "psk", enc_psk);
    cJSON_DeleteItemFromObject(wifi, "auto_connect");
    cJSON_AddNumberToObject(wifi, "auto_connect", 1);

    ret = app_settings_save_root(root);
    if (ret != 0)
        log_warn_app("wifi save: cannot write %s", APP_SETTINGS_CONFIG_PATH);
    else
        log_info_app("wifi save: wrote %s ssid=%s auto_connect=1",
                     APP_SETTINGS_CONFIG_PATH, ssid);
    cJSON_Delete(root);
    return ret;
}

static int wifi_save_credentials(const wifi_info_t *info)
{
    if (!info)
        return -1;
    return tablet_setting_wifi_save_sta(info->ssid, info->pwd);
}

static int wifi_station_ready(void)
{
    return s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station;
}

/* Board WiFi capability gate: settings.json "wifi":{"enable"} (primary),
 * hal_wifi_config.ini [station] enable (legacy fallback for boards that
 * still ship that file and have not migrated settings.json yet). Same
 * conservative default as before this migration: disabled (0) unless a
 * source explicitly says otherwise. */
static int wifi_station_enabled_in_config(void)
{
    cJSON *root = app_settings_load_root();
    if (root) {
        cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
        if (cJSON_IsObject(wifi)) {
            cJSON *enable = cJSON_GetObjectItem(wifi, "enable");
            if (cJSON_IsNumber(enable)) {
                int enabled = enable->valueint ? 1 : 0;
                cJSON_Delete(root);
                return enabled;
            }
        }
        cJSON_Delete(root);
    }

    {
        char cfg_path[256] = {0};
        if (config_build_file_path("hal_wifi_config.ini", cfg_path, sizeof(cfg_path)) != 0)
            return 0;
        dictionary *handle = iniparser_load(cfg_path);
        if (!handle)
            return 0;
        int enabled = iniparser_dict_getint(handle, "station", "enable", 0);
        iniparser_freedict(handle);
        return enabled ? 1 : 0;
    }
}

/** Init station for settings scan/connect; gated by settings.json wifi.enable (same as tablet_run). */
static int wifi_station_ensure_init(void)
{
    if (!wifi_station_enabled_in_config()) {
        log_warn_app("[DBG_WIFI_SCAN] ensure_init: wifi.enable=0");
        return -1;
    }
    if (!wifi_station_ready()) {
        log_warn_app("[DBG_WIFI_SCAN] ensure_init: station ops null");
        return -1;
    }
    if (s_wifi_station_inited)
        return 0;

    int ret = s_wifi_network->wifi->station->init();
    log_warn_app("[DBG_WIFI_SCAN] station->init() ret=%d (enable=1)", ret);
    if (ret == 0) {
        s_wifi_station_inited = 1;
        return 0;
    }
    return ret;
}

static int wifi_station_scan_start_logged(void)
{
    if (!s_wifi_station_inited) {
        log_warn_app("[DBG_WIFI_SCAN] scan_start skipped: station not inited");
        return -1;
    }
    int ret = s_wifi_network->wifi->station->scan_start();
    log_warn_app("[DBG_WIFI_SCAN] scan_start ret=%d", ret);
    return ret;
}

static int wifi_station_scan_get_results_logged(void)
{
    if (!s_wifi_station_inited) {
        log_warn_app("[DBG_WIFI_SCAN] scan_get_results skipped: station not inited");
        return -1;
    }
    int max = (int)(sizeof(g_wifi_list) / sizeof(g_wifi_list[0]));
    g_wifi_list_cnt = max;
    int ret = s_wifi_network->wifi->station->scan_get_results(g_wifi_list, &g_wifi_list_cnt);
    if (ret != 0)
        g_wifi_list_cnt = 0;
    log_warn_app("[DBG_WIFI_SCAN] scan_get_results ret=%d cnt=%d", ret, g_wifi_list_cnt);
    if (g_wifi_list_cnt > max)
        g_wifi_list_cnt = max;
    return ret;
}

static void create_wifi_list_ui(lv_obj_t *cont, network_wifi_scan_result_t wifi_list[], int wifi_item_count);

/** Remove entries with empty or whitespace-only SSID from g_wifi_list; update g_wifi_list_cnt. */
static void wifi_list_remove_empty_entries(void)
{
    int n = 0;
    for (int i = 0; i < g_wifi_list_cnt; i++) {
        const char *s = g_wifi_list[i].ssid;
        if (!s) continue;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') continue;
        if (n != i)
            memcpy(&g_wifi_list[n], &g_wifi_list[i], sizeof(network_wifi_scan_result_t));
        n++;
    }
    g_wifi_list_cnt = n;
}

static int get_wifi_info_by_ssid(char *ssid, network_wifi_scan_result_t *out)
{
    for (int i = 0; i < g_wifi_list_cnt; i++)
    {
        if (strcmp(ssid, g_wifi_list[i].ssid) == 0)
        {
            memcpy(out, &g_wifi_list[i], sizeof(network_wifi_scan_result_t));
            return 0;
        }
    }
    return -1;
}

static int show_wifi_status(void)
{
    char str_status_buf[256] = {0};
    if (s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station) {
        network_wifi_station_state_e state;
        if (s_wifi_network->wifi->station->get_state(&state) == 0 && state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
            char ssid[33] = {0}, ip[64] = {0};
            s_wifi_network->wifi->station->get_ssid(ssid, sizeof(ssid));
            s_wifi_network->wifi->station->get_ip_address(ip, sizeof(ip));
            if (ssid[0] == '\0' && g_connect_info.ssid[0] != '\0')
                strncpy(ssid, g_connect_info.ssid, sizeof(ssid) - 1);
            if (ssid[0] == '\0')
                strncpy(ssid, "-", sizeof(ssid) - 1);
            sprintf(str_status_buf, "SSID: %s\n%s\nIP: %s", ssid, lan("connected"), ip);
            network_wifi_scan_result_t scan_info = {0};
            if (get_wifi_info_by_ssid(ssid, &scan_info) == 0) {
                sprintf(str_status_buf + strlen(str_status_buf), "\n%s: %d", lan("signal strength"), scan_info.rssi);
            }
        } else if (eLinkState == LINK_STATE_WIFI_CONNECTING ||
                   eLinkState == LINK_STATE_SERVER_CONNECTING) {
            sprintf(str_status_buf, "SSID: %s\n%s...",
                    g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                    lan("connecting") ? lan("connecting") : "Connecting");
        } else if (eLinkState == LINK_STATE_SERVER_CONNECTED) {
            sprintf(str_status_buf, "SSID: %s\n%s",
                    g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                    lan("connected"));
        } else {
            sprintf(str_status_buf, "%s: %s", g_connect_info.ssid[0] ? g_connect_info.ssid : "-", lan("connect failed"));
        }
    } else {
        sprintf(str_status_buf, "%s: %s", g_connect_info.ssid[0] ? g_connect_info.ssid : "-", lan("connect failed"));
    }

    if (wifi_lable_status)
        lv_label_set_text(wifi_lable_status, str_status_buf);

    log_info_app("out: ");
    return 0;
}

static void wifi_try_auto_connect_once(void)
{
    if (s_auto_connect_tried)
        return;
    s_auto_connect_tried = 1;
    if (!wifi_station_ready())
        return;

    network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
    if (s_wifi_network->wifi->station->get_state(&state) == 0 &&
        state == NETWORK_WIFI_STATION_STATE_CONNECTED)
        return;

    wifi_info_t saved = {0};
    if (wifi_load_saved_credentials(&saved) != 0)
        return;

    char msg[128] = {0};
    if (start_connect(&saved, msg, sizeof(msg)) == 0) {
        memcpy(&g_connect_info, &saved, sizeof(g_connect_info));
        iTimeSec = system_get_sys_run_time_s();
        if (!work_timer)
            work_timer = lv_timer_create(connect_async_task, 1000, NULL);
        log_info_app("wifi auto connect start: ssid=%s", saved.ssid);
    }
}

static int start_connect(wifi_info_t *pInfo, char *msg_buf, int len)
{
    log_info_app("TickTick");

    if (wifi_station_ensure_init() != 0) {
        log_warn_app("[DBG_WIFI_SCAN] connect abort: station not inited");
        strncpy(msg_buf, lan("wifi driver unavailable"), len);
        return -1;
    }

    if (s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station)
        s_wifi_network->wifi->station->disconnect();
    eLinkState = LINK_STATE_WIFI_CONNECTING;

    if (strnlen(pInfo->ssid, WIFI_PWD_LEN - 1) <= 0) {
        log_info_app("ssid is null");
        strncpy(msg_buf, lan("ssid is null"), len);
        return -1;
    }
    if (strnlen(pInfo->pwd, WIFI_PWD_LEN - 1) > 0 && strnlen(pInfo->pwd, WIFI_PWD_LEN - 1) < 8) {
        log_info_app("WiFiPwdError-Reinput");
        strncpy(msg_buf, lan("password is error"), len);
        return -2;
    }
    if (s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station) {
        network_wifi_station_config_t config = {0};
        strncpy(config.ssid, pInfo->ssid, sizeof(config.ssid) - 1);
        strncpy(config.passphrase, pInfo->pwd, sizeof(config.passphrase) - 1);
        config.security_type = 0;
        s_wifi_network->wifi->station->connect(&config);
    }
    return 0;
}

/* Same rule as QR pair: get_ssid may be stubbed; CONNECTED + IP after connect is OK. */
static int wifi_list_sta_is_success(network_wifi_station_state_e state,
                                    const char *cur_ssid, const char *ip)
{
    if (state != NETWORK_WIFI_STATION_STATE_CONNECTED || g_connect_info.ssid[0] == '\0')
        return 0;
    if (cur_ssid && cur_ssid[0] != '\0')
        return strcmp(cur_ssid, g_connect_info.ssid) == 0;
    if (ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
        return 1;
    return 0;
}

static int conect_process(char *msg_buf, int len)
{
    int iTimeOut = 30;
    int iTimeSec = system_get_sys_run_time_s();
    long long last_log_s = -1;
    while (eLinkState == LINK_STATE_WIFI_CONNECTING && ((system_get_sys_run_time_s() - iTimeSec) < iTimeOut)) {
        network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
        char cur_ssid[WIFI_SSID_LEN] = {0};
        char ip[64] = {0};
        long long elapsed;

        if (s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station) {
            s_wifi_network->wifi->station->get_state(&state);
            if (state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
                s_wifi_network->wifi->station->get_ssid(cur_ssid, sizeof(cur_ssid));
                s_wifi_network->wifi->station->get_ip_address(ip, sizeof(ip));
            }
        }
        if (wifi_list_sta_is_success(state, cur_ssid, ip)) {
            eLinkState = LINK_STATE_SERVER_CONNECTED;
            log_info_app("ConnectWiFiOK ssid='%s' ip='%s'", g_connect_info.ssid,
                         ip[0] ? ip : "-");
            strncpy(msg_buf, lan("connect wifi ok"), len);
            return 0;
        }
        elapsed = system_get_sys_run_time_s() - iTimeSec;
        if (elapsed != last_log_s) {
            last_log_s = elapsed;
            log_info_app(" pair network wait, state=%d cur='%s' ip='%s' want='%s' [t: %lld s]",
                         (int)state, cur_ssid[0] ? cur_ssid : "-",
                         ip[0] ? ip : "-",
                         g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                         elapsed);
        }
        system_sleep_ms(100);
    }
    log_info_app("NetWorkConnectFail-CheckWifiRightAndCloseRoute");
    strncpy(msg_buf, lan("wifi connect failed, please check wifi and close route !"), len);
    return -3;
}

/* List item click callback */
static void item_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);

    if (code == LV_EVENT_CLICKED) {
        if (s_wifi_keyboard_active) {
            /* Ignore list click while typing; prevents "long-press WiFi" behavior. */
            return;
        }
        network_wifi_scan_result_t *p = (network_wifi_scan_result_t *)lv_obj_get_user_data(obj);
        log_debug("clicked Wi-Fi: %s", p->ssid);

        if (wifi_win_ssid)
            lv_textarea_set_text(wifi_win_ssid, p->ssid);
        /* TODO: get full item info and e.g. lv_scr_load(connect_screen) */
    }
}

/* Refresh button click callback */
static void btn_event_refresh_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);

    if (code == LV_EVENT_CLICKED)
    {
        log_warn_app("[DBG_WIFI_SCAN] refresh clicked kb_active=%d", s_wifi_keyboard_active);
        if (s_wifi_keyboard_active) {
            return;
        }
        if (!wifi_station_ready() || wifi_station_ensure_init() != 0) {
            log_warn_app("[DBG_WIFI_SCAN] refresh abort: station not ready/inited");
            create_wifi_list_ui(win_left, g_wifi_list, 0);
            return;
        }
        for (int i = 0; i < g_wifi_list_cnt; i++)
        {
            if(g_wifi_list_ctrl[i])
            {
                lv_obj_delete(g_wifi_list_ctrl[i]);
                g_wifi_list_ctrl[i] = NULL;
            }
        }
        g_wifi_list_cnt = 0;
        if (wifi_station_scan_start_logged() == 0) {
            system_sleep_ms(500);
            wifi_station_scan_get_results_logged();
        }
        wifi_list_remove_empty_entries();
        log_warn_app("[DBG_WIFI_SCAN] refresh done list_cnt=%d", g_wifi_list_cnt);
        create_wifi_list_ui(win_left, g_wifi_list, g_wifi_list_cnt);
    }
}

static void wifi_list_ctrl_clear(void)
{
    int i;

    for (i = 0; i < (int)(sizeof(g_wifi_list_ctrl) / sizeof(g_wifi_list_ctrl[0])); i++) {
        if (g_wifi_list_ctrl[i] && lv_obj_is_valid(g_wifi_list_ctrl[i]))
            lv_obj_delete(g_wifi_list_ctrl[i]);
        g_wifi_list_ctrl[i] = NULL;
    }
}

static void create_wifi_list_ui(lv_obj_t *cont, network_wifi_scan_result_t wifi_list[], int wifi_item_count)
{
    wifi_list_ctrl_clear();

    if (!wifi_station_ready()) {
        if (s_no_networks_label)
            lv_obj_add_flag(s_no_networks_label, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_driver_hint == NULL) {
            s_wifi_driver_hint = lv_label_create(cont);
            lv_obj_set_width(s_wifi_driver_hint, LV_PCT(100));
            lv_label_set_long_mode(s_wifi_driver_hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(s_wifi_driver_hint, lv_color_hex(SETTING_TEXT_SEC), 0);
            if (tablet_font_get_freetype_font(16))
                lv_obj_set_style_text_font(s_wifi_driver_hint, tablet_font_get_freetype_font(16), 0);
            else
                lv_obj_set_style_text_font(s_wifi_driver_hint, tablet_font_get_montserrat_font(14), 0);
            lv_obj_set_style_bg_opa(s_wifi_driver_hint, LV_OPA_0, 0);
            lv_obj_set_style_border_width(s_wifi_driver_hint, 0, 0);
        }
        {
            const char *msg = lan("WiFi driver not loaded");
            if (!msg) msg = lan("wifi driver unavailable");
            if (!msg)
                msg = "Wi-Fi is unavailable.\n(Network driver not loaded or WiFi not initialized.)";
            lv_label_set_text(s_wifi_driver_hint, msg);
        }
        lv_obj_clear_flag(s_wifi_driver_hint, LV_OBJ_FLAG_HIDDEN);
        g_wifi_list_cnt = 0;
        return;
    }

    if (s_wifi_driver_hint)
        lv_obj_add_flag(s_wifi_driver_hint, LV_OBJ_FLAG_HIDDEN);

    if (wifi_item_count == 0) {
        g_wifi_list_cnt = 0;
        if (s_no_networks_label == NULL) {
            s_no_networks_label = lv_label_create(cont);
            lv_obj_set_width(s_no_networks_label, LV_PCT(100));
            lv_label_set_long_mode(s_no_networks_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(s_no_networks_label, lv_color_hex(SETTING_TEXT_SEC), 0);
            if (tablet_font_get_freetype_font(16))
                lv_obj_set_style_text_font(s_no_networks_label, tablet_font_get_freetype_font(16), 0);
            else
                lv_obj_set_style_text_font(s_no_networks_label, tablet_font_get_montserrat_font(14), 0);
            lv_obj_set_style_bg_opa(s_no_networks_label, LV_OPA_0, 0);
            lv_obj_set_style_border_width(s_no_networks_label, 0, 0);
        }
        {
            const char *msg = lan("No networks found");
            if (!msg) msg = "No networks found";
            lv_label_set_text(s_no_networks_label, msg);
        }
        lv_obj_clear_flag(s_no_networks_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Has APs: hide no-networks hint and show list */
    if (s_no_networks_label) {
        lv_obj_add_flag(s_no_networks_label, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < wifi_item_count; i++) {
        network_wifi_scan_result_t *item = &wifi_list[i];

        /* One row per list entry */
        lv_obj_t * item_cont = lv_obj_create(cont);
        g_wifi_list_ctrl[i] = item_cont;
        lv_obj_set_size(item_cont, LV_PCT(96), 52);
        //lv_obj_set_flex_flow(item_cont, LV_FLEX_FLOW_ROW);
        //lv_obj_set_flex_align(item_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(item_cont, lv_color_hex(SETTING_CARD_ITEM), 0);
        lv_obj_set_style_bg_opa(item_cont, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(item_cont, 1, 0);
        lv_obj_set_style_border_color(item_cont, lv_color_hex(SETTING_BORDER), 0);
        lv_obj_set_style_radius(item_cont, GUI_SCALE(12), 0);
        /* Row itself must not pan: a drag inside it belongs to the list, and an
         * internally scrolled row also swallows the CLICKED used to pick a network. */
        lv_obj_remove_flag(item_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(item_cont, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(item_cont, LV_OBJ_FLAG_SCROLL_CHAIN);

        /* SSID label — readable, still below the section title (22) */
        lv_obj_t * ssid_label = lv_label_create(item_cont);
        lv_label_set_text(ssid_label, item->ssid);
        lv_obj_set_style_text_color(ssid_label, lv_color_hex(SETTING_TEXT), 0);
        if (tablet_font_get_freetype_font(18))
            lv_obj_set_style_text_font(ssid_label, tablet_font_get_freetype_font(18), 0);
        else
            lv_obj_set_style_text_font(ssid_label, tablet_font_get_montserrat_font(16), 0);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid_label, LV_PCT(100));
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 0, 0);

   /*     lv_obj_t * bar = lv_bar_create(item_cont);
        lv_obj_set_width(bar, 70);
        lv_obj_set_height(bar, LV_PCT(100));
        int rssi = item->signal;
        if (rssi < -100) rssi = -100;
        if (rssi > -40) rssi = -40;
        int value = (rssi + 100) * 100 / 60;
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
        ... */

        lv_obj_set_user_data(item_cont, &wifi_list[i]);
        lv_obj_add_event_cb(item_cont, item_event_cb, LV_EVENT_CLICKED, (void*)&wifi_list[i]);
    }
}

/* Async task: check connection state in background */
static void connect_async_task(lv_timer_t * timer)
{
    log_info_app ("in: ");

    char msg_buf[256] = {0};
    network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
    if (s_wifi_network && s_wifi_network->wifi && s_wifi_network->wifi->station)
        s_wifi_network->wifi->station->get_state(&state);
    if (state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
        eLinkState = LINK_STATE_SERVER_CONNECTED;
        log_info_app("ConnectWiFiOK");
        strcpy(msg_buf, lan("connect wifi ok"));
        (void)wifi_save_credentials(&g_connect_info);
    } else if (system_get_sys_run_time_s() - iTimeSec > iTimeOut)
    {
        log_info_app ("NetWorkConnectFail-CheckWifiRightAndCloseRoute");
        strcpy(msg_buf, lan("wifi connect failed, please check wifi and close route !"));
    }
    else
    {
        log_info_app ("connecting..................\n");
        return;
    }

    if(win_connecting)
    {
        //??lv_obj_delete(win_connecting);
        lv_msgbox_close(win_connecting);
        win_connecting = NULL;
    }
    if(work_timer)
    {
        lv_timer_delete(work_timer);
        work_timer = NULL;
    }

    show_wifi_status();
    show_msgbox_note(msg_buf);

    log_info_app ("out: ");
}

static void wifi_show_msgbox_note(const char *str)
{
    /* Create message box */
    lv_obj_t * mbox = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox, LV_PCT(40), LV_PCT(30));

    lv_obj_t * background = lv_obj_get_parent(mbox);
    lv_obj_set_style_bg_opa(background, LV_OPA_0, 0);

    lv_obj_center(mbox);

    lv_font_t *font = tablet_font_get_freetype_font(20);

    lv_obj_t * title = lv_msgbox_add_title(mbox, lan("note"));
    lv_obj_set_style_text_font(title, font, 0);

    lv_obj_t *txt = lv_msgbox_add_text(mbox, str);
    win_msg_txt = txt;
    lv_obj_set_style_text_font(txt, font, 0);

    lv_obj_t * btn = lv_msgbox_add_close_button(mbox);
    win_msg_close_btn = btn;
    lv_obj_set_size(btn, LV_PCT(20), LV_PCT(100));
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * show_msgbox_note_no_btn(const char *str)
{
    lv_obj_t * mbox = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox, LV_PCT(40), LV_PCT(30));

    lv_obj_t * background = lv_obj_get_parent(mbox);
    lv_obj_set_style_bg_opa(background, LV_OPA_0, 0);

    lv_obj_center(mbox);

    lv_font_t *font = tablet_font_get_freetype_font(20);

    lv_obj_t * title = lv_msgbox_add_title(mbox, lan("note"));
    lv_obj_set_style_text_font(title, font, 0);

    lv_obj_t *txt = lv_msgbox_add_text(mbox, str);
    lv_obj_set_style_text_font(txt, font, 0);
    return mbox;
}

/* Connect button click callback */
static void btn_event_connect_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);

    if (code == LV_EVENT_CLICKED) {
        strcpy(g_connect_info.ssid, lv_textarea_get_text(ssid_ta));
        strcpy(g_connect_info.pwd, lv_textarea_get_text(pwd_ta));
        char msg[256] = {0};
        int ret = start_connect(&g_connect_info,msg,sizeof(msg));
        if(ret != 0)
        {
            show_msgbox_note(msg);
            return;
        }

        /* Message box without close button */
        sprintf(msg, "%s:%s", lan("connecting"), g_connect_info.ssid);
        win_connecting = show_msgbox_note_no_btn(msg);
        //wifi_show_msgbox_note(msg);

        iTimeSec = system_get_sys_run_time_s();

        /* Start async check timer (non-blocking) */
        work_timer = lv_timer_create(connect_async_task, 1000, btn);
    }
}

/* Soft keyboard (global) */
static lv_obj_t * kb = NULL;

/* Optional keyboard event callback */
static void kb_event_cb(lv_event_t * e)
{
    (void)e;
    /* Handle key events (enter, backspace, etc.) if needed */
}

/* Show keyboard when text area gets focus */
static void ta_focused_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        s_wifi_keyboard_active = 1;
        if (!kb) {
            kb = lv_keyboard_create(lv_display_get_screen_active(lv_display_get_default()));
            lv_obj_set_size(kb, 300, 200);
            lv_obj_center(kb);
            lv_keyboard_set_textarea(kb, ta);
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
            lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_defocused_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);

    if (code == LV_EVENT_DEFOCUSED) {
        if (kb) {
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
        s_wifi_keyboard_active = 0;
    }
}

static void wifi_scan_async_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!wifi_station_ready() || !s_wifi_station_inited)
        return;

    /* Poll scan results without blocking GUI thread. */
    if (!s_wifi_network || !s_wifi_network->wifi || !s_wifi_network->wifi->station)
        return;

    wifi_station_scan_get_results_logged();
    wifi_list_remove_empty_entries();
    log_warn_app("[DBG_WIFI_SCAN] async poll try=%d/%d cnt=%d",
                 s_wifi_scan_try_cnt + 1, s_wifi_scan_try_max, g_wifi_list_cnt);

    /* Rebuild list UI if window exists. */
    if (win_left) {
        create_wifi_list_ui(win_left, g_wifi_list, g_wifi_list_cnt);
        lv_obj_invalidate(win_left);
    }

    s_wifi_scan_try_cnt++;
    if (g_wifi_list_cnt > 0 || s_wifi_scan_try_cnt >= s_wifi_scan_try_max) {
        if (s_wifi_scan_timer) {
            lv_timer_delete(s_wifi_scan_timer);
            s_wifi_scan_timer = NULL;
        }
    }
}

static void create_wifi_connect_ui(lv_obj_t *parent)
{
    /* Right column: refresh list with icon + text */
    lv_obj_t *btn_refresh = lv_btn_create(parent);
    lv_obj_set_size(btn_refresh, LV_PCT(100), 48);
    lv_obj_add_event_cb(btn_refresh, btn_event_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_bg_opa(btn_refresh, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_refresh, GUI_SCALE(10), 0);
    lv_obj_set_style_border_width(btn_refresh, 1, 0);
    lv_obj_set_style_border_color(btn_refresh, lv_color_hex(SETTING_BORDER), 0);
    lv_obj_set_flex_flow(btn_refresh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_refresh, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_refresh, 10, 0);

    lv_obj_t *lbl_rf_sym = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_rf_sym, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(lbl_rf_sym, lv_color_hex(SETTING_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_rf_sym, tablet_font_get_montserrat_font(14), 0);

    lv_obj_t *lbl_rf_txt = lv_label_create(btn_refresh);
    {
        const char *t = lan("Refresh");
        if (!t) t = lan("refresh");
        if (!t) t = "Refresh";
        lv_label_set_text(lbl_rf_txt, t);
    }
    lv_obj_set_style_text_color(lbl_rf_txt, lv_color_hex(SETTING_TEXT), 0);
    if (tablet_font_get_freetype_font(18))
        lv_obj_set_style_text_font(lbl_rf_txt, tablet_font_get_freetype_font(18), 0);

    /* SSID input */
    ssid_ta = lv_textarea_create(parent);
    wifi_win_ssid = ssid_ta;
    lv_obj_set_width(ssid_ta, 260);
    lv_textarea_set_placeholder_text(ssid_ta, lan("Select SSID"));
    lv_textarea_set_one_line(ssid_ta, true);
    lv_obj_set_style_bg_color(ssid_ta, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_bg_opa(ssid_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ssid_ta, lv_color_hex(SETTING_TEXT), 0);
    lv_obj_set_style_border_width(ssid_ta, 1, 0);
    lv_obj_set_style_border_color(ssid_ta, lv_color_hex(SETTING_BORDER), 0);
    lv_obj_set_style_radius(ssid_ta, GUI_SCALE(10), 0);
    /* Password input */
    pwd_ta = lv_textarea_create(parent);
    lv_obj_set_width(pwd_ta, 260);
    lv_textarea_set_placeholder_text(pwd_ta, lan("Enter password"));
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_style_bg_color(pwd_ta, lv_color_hex(SETTING_CARD_ITEM), 0);
    lv_obj_set_style_bg_opa(pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(pwd_ta, lv_color_hex(SETTING_TEXT), 0);
    lv_obj_set_style_border_width(pwd_ta, 1, 0);
    lv_obj_set_style_border_color(pwd_ta, lv_color_hex(SETTING_BORDER), 0);
    lv_obj_set_style_radius(pwd_ta, GUI_SCALE(10), 0);
    lv_obj_add_event_cb(pwd_ta, ta_focused_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(pwd_ta, ta_defocused_event_cb, LV_EVENT_DEFOCUSED, NULL);

    if (!kb) {
        kb = lv_keyboard_create(lv_display_get_screen_active(lv_display_get_default()));
        static lv_style_t style_kb;
        lv_style_init(&style_kb);
        lv_style_set_text_font(&style_kb, tablet_font_get_montserrat_font(24));
        lv_obj_add_style(kb, &style_kb, LV_PART_ITEMS);
        lv_obj_set_size(kb, LV_HOR_RES - 40, 200);
        //lv_obj_center(kb);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_RIGHT, -2, -(2 + TABLET_HOME_BAR_H));
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_keyboard_set_textarea(kb, pwd_ta);

    /* Connect button */
    lv_obj_t * btn_conn = lv_btn_create(parent);
    lv_obj_set_size(btn_conn, 130, 50);
    lv_obj_add_event_cb(btn_conn, btn_event_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn_conn, lv_color_hex(SETTING_ACCENT), 0);
    lv_obj_set_style_bg_grad_color(btn_conn, lv_color_hex(SETTING_ACCENT_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(btn_conn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(btn_conn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_conn, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_width(btn_conn, GUI_SCALE(8), 0);
    lv_obj_set_style_shadow_ofs_y(btn_conn, GUI_SCALE_VER(2), 0);
    lv_obj_set_style_shadow_color(btn_conn, lv_color_hex(SETTING_ACCENT_DEEP), 0);
    lv_obj_set_style_shadow_opa(btn_conn, LV_OPA_50, 0);
    wifi_lable_connect = lv_label_create(btn_conn);
    lv_obj_center(wifi_lable_connect);
    lv_obj_set_style_text_color(wifi_lable_connect, lv_color_white(), 0);
    lv_label_set_text(wifi_lable_connect, lan("Connect"));

    /* Status area (takes remaining height) */
    lv_obj_t * info_area = lv_obj_create(parent);
    tablet_setting_flat_container(info_area);
    lv_obj_set_size(info_area, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info_area, 1);
    lv_obj_set_style_pad_all(info_area, 0, 0);
    {
        lv_font_t *f20 = tablet_font_get_freetype_font(20);
        if (f20)
            lv_obj_set_style_text_font(info_area, f20, 0);
    }
    lv_obj_set_style_bg_color(info_area, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(info_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_area, 0, 0);

    wifi_lable_status = lv_label_create(info_area);
    lv_obj_set_size(wifi_lable_status, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_lable_status, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_bg_opa(wifi_lable_status, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_lable_status, 20, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_lable_status, GUI_SCALE(10), 0);
    lv_obj_set_style_border_width(wifi_lable_status, 1, 0);
    lv_obj_set_style_border_color(wifi_lable_status, lv_color_hex(SETTING_BORDER), 0);
    lv_obj_set_style_text_color(wifi_lable_status, lv_color_hex(SETTING_TEXT), 0);
    {
        lv_font_t *f20 = tablet_font_get_freetype_font(20);
        if (f20)
            lv_obj_set_style_text_font(wifi_lable_status, f20, 0);
    }
    lv_label_set_text(wifi_lable_status, lan("WiFi status"));
}

/* Build main WiFi settings UI */
static void create_ui(lv_obj_t *parent)
{
    tablet_setting_flat_container(parent);

    lv_obj_t * cont = lv_obj_create(parent);
    wifi_win_main = cont;
    tablet_setting_flat_container(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(cont, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_column(cont, 10, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    win_left = lv_obj_create(cont);
    tablet_setting_flat_container(win_left);
    lv_obj_set_size(win_left, 0, LV_PCT(100));
    lv_obj_set_flex_grow(win_left, 1);
    lv_obj_set_flex_flow(win_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win_left, 10, 0);
    lv_obj_set_style_bg_color(win_left, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_radius(win_left, GUI_SCALE(16), 0);
    lv_obj_set_style_bg_opa(win_left, LV_OPA_COVER, 0);
    /* Launcher card recipe: white hairline + soft shadow (after flat_container). */
    lv_obj_set_style_border_width(win_left, 1, 0);
    lv_obj_set_style_border_color(win_left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(win_left, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(win_left, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(win_left, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(win_left, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(win_left, LV_OPA_40, 0);
    lv_obj_remove_flag(win_left, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_set_scrollbar_mode(win_left, LV_SCROLLBAR_MODE_AUTO);

    s_wifi_list_title = lv_label_create(win_left);
    {
        const char *tit = lan("WiFi network list");
        if (!tit) tit = lan("WLAN");
        if (!tit) tit = "Wi-Fi networks";
        lv_label_set_text(s_wifi_list_title, tit);
    }
    lv_obj_set_width(s_wifi_list_title, LV_PCT(100));
    /* Stand out: primary text + larger font + accent bar feel */
    lv_obj_set_style_text_color(s_wifi_list_title, lv_color_hex(SETTING_TEXT), 0);
    if (tablet_font_get_freetype_font(22))
        lv_obj_set_style_text_font(s_wifi_list_title, tablet_font_get_freetype_font(22), 0);
    else if (tablet_font_get_freetype_font(20))
        lv_obj_set_style_text_font(s_wifi_list_title, tablet_font_get_freetype_font(20), 0);
    else
        lv_obj_set_style_text_font(s_wifi_list_title, tablet_font_get_montserrat_font(18), 0);
    lv_obj_set_style_pad_bottom(s_wifi_list_title, 10, 0);
    lv_obj_set_style_border_side(s_wifi_list_title, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_wifi_list_title, 2, 0);
    lv_obj_set_style_border_color(s_wifi_list_title, lv_color_hex(SETTING_ACCENT), 0);

    create_wifi_list_ui(win_left, g_wifi_list, g_wifi_list_cnt);

    /* Right panel: fixed width on the right only (not half-screen), avoids full-width top edge vs left list */
    lv_obj_t * win_right = lv_obj_create(cont);
    tablet_setting_flat_container(win_right);
    lv_obj_set_size(win_right, LV_HOR_RES / 2 - 20, LV_PCT(100));
    lv_obj_set_flex_grow(win_right, 0);
    lv_obj_set_flex_flow(win_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(win_right, 10, 0);
    lv_obj_set_flex_align(win_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* AI tab right column style: CARD_BG card with launcher hairline + shadow */
    lv_obj_set_style_pad_all(win_right, 10, 0);
    lv_obj_set_style_bg_color(win_right, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_bg_opa(win_right, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(win_right, 0, 0);
    lv_obj_set_style_radius(win_right, GUI_SCALE(16), 0);
    lv_obj_set_style_border_width(win_right, 1, 0);
    lv_obj_set_style_border_color(win_right, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(win_right, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(win_right, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(win_right, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(win_right, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(win_right, LV_OPA_40, 0);
    lv_obj_remove_flag(win_right, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    create_wifi_connect_ui(win_right);
}

lv_result_t tablet_setting_wifi_create(lv_obj_t *parent)
{
    int do_scan = 0;
    int init_ret = -1;

    log_warn_app("in: parent=%p", (void *)parent);

    s_wifi_network = (network_t *)app_main_get_network();
    /* Never block GUI thread here: init/scan/sleep can make left menu unresponsive. */
    g_wifi_list_cnt = 0;
    if (wifi_station_ready()) {
        init_ret = wifi_station_ensure_init();
        log_warn_app("[DBG_WIFI_SCAN] create: ensure_init ret=%d inited=%d",
                     init_ret, s_wifi_station_inited);
    } else {
        log_warn_app("[DBG_WIFI_SCAN] create: station ops null");
    }

    /* Build UI first, then kick scan — avoids racing scan callbacks before widgets exist. */
    create_ui(parent);

    if (g_connect_info.ssid[0] != '\0' && ssid_ta && lv_obj_is_valid(ssid_ta))
        lv_textarea_set_text(ssid_ta, g_connect_info.ssid);
    if (g_connect_info.pwd[0] != '\0' && pwd_ta && lv_obj_is_valid(pwd_ta))
        lv_textarea_set_text(pwd_ta, g_connect_info.pwd);
    show_wifi_status();
    wifi_try_auto_connect_once();

    if (init_ret == 0)
        do_scan = (wifi_station_scan_start_logged() == 0);
    log_warn_app("g_wifi_list_cnt=%d do_scan=%d", g_wifi_list_cnt, do_scan);

    /* Start async scan polling after UI is ready. */
    if (do_scan && s_wifi_scan_timer == NULL) {
        s_wifi_scan_try_cnt = 0;
        s_wifi_scan_try_max = 12;
        s_wifi_scan_timer = lv_timer_create(wifi_scan_async_timer_cb, 400, NULL);
    }

    log_warn_app("out: ");
    return LV_RESULT_OK;
}


lv_result_t tablet_setting_wifi_close()
{
    log_warn_app( "in: ");

    if (work_timer) {
        lv_timer_delete(work_timer);
        work_timer = NULL;
    }
    if (win_connecting) {
        lv_msgbox_close(win_connecting);
        win_connecting = NULL;
    }
    win_msg_txt = NULL;
    win_msg_close_btn = NULL;
    eLinkState = LINK_STATE_RESET;

    if (s_wifi_scan_timer) {
        lv_timer_delete(s_wifi_scan_timer);
        s_wifi_scan_timer = NULL;
    }
    s_wifi_scan_try_cnt = 0;
    s_auto_connect_tried = 0;

    if (wifi_win_main && lv_obj_is_valid(wifi_win_main)) {
        lv_obj_delete(wifi_win_main);
    }
    wifi_win_main = NULL;
    win_left = NULL;
    wifi_win_ssid = NULL;
    wifi_lable_status = NULL;
    wifi_lable_connect = NULL;
    ssid_ta = NULL;
    pwd_ta = NULL;
    s_no_networks_label = NULL;
    s_wifi_driver_hint = NULL;
    s_wifi_list_title = NULL;
    /* Parent delete already freed list rows — only drop dangling pointers. */
    {
        int i;
        for (i = 0; i < (int)(sizeof(g_wifi_list_ctrl) / sizeof(g_wifi_list_ctrl[0])); i++)
            g_wifi_list_ctrl[i] = NULL;
    }
    if (kb && lv_obj_is_valid(kb)) {
        lv_obj_delete(kb);
    }
    kb = NULL;
    s_wifi_keyboard_active = 0;
    log_warn_app( "out: ");
    return LV_RESULT_OK;
}


#else
/* Stubs when CONFIG_FW_NETWORK_WIFI is not defined so linker has both symbols */
#include "tablet_setting_wifi.h"
lv_result_t tablet_setting_wifi_create(lv_obj_t *parent)
{
    (void)parent;
    return LV_RESULT_OK;
}
lv_result_t tablet_setting_wifi_close(void)
{
    return LV_RESULT_OK;
}
int tablet_setting_wifi_save_sta(const char *ssid, const char *psk)
{
    (void)ssid;
    (void)psk;
    return -1;
}
#endif
