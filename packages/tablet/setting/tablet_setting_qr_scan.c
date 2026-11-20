// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo setting QR scan screen.
 */
#define LOG_TAG "TABLET_SETTING_QR_SCAN"

#include "config.h"

#ifdef CONFIG_FW_NETWORK_WIFI
#include "tablet_setting_wifi.h"
#include "tablet_font.h"
#include "tablet_setting.h"
#include "tablet_common.h"

#include "app_main.h"
#include "multimedia.h"
#include "log.h"
#define log_info_app log_info
#define log_warn_app log_warn
#define log_error_app log_error

#include "network.h"
#include "gthread.h"
#include "zxing_scan.h"
#include "tablet_video_ctrl.h"
#include "system_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1: save YUV to /mnt/yuv every frame — can block UI exit on slow/missing mount; keep 0 on product. */
#ifndef QR_PAIR_SAVE_YUV_DEBUG
#define QR_PAIR_SAVE_YUV_DEBUG 0
#endif
/* Decode only 1 of N good frames — full-res ZXing on every frame can block join for many seconds. */
#ifndef QR_PAIR_ZXING_EVERY_N
#define QR_PAIR_ZXING_EVERY_N 4
#endif

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

#define AK_CHECK_POINT()  ((void)0)
static GThread *pair_th = NULL;
/* pair_th runtime state; UI close path polls briefly instead of blocking forever in join */
static volatile int s_pair_th_running = 0;

static int nb_scan_connect_run(void *arg);

static void *pair_th_wrapper(void *arg)
{
    (void)arg;
    s_pair_th_running = 1;
    nb_scan_connect_run(NULL);
    s_pair_th_running = 0;
    return NULL;
}

/* Used across GUI thread + pair_th thread: must be volatile so thread sees updates */
static volatile enum nb_link_state eLinkState = LINK_STATE_RESET;
static volatile int nRunScan = 1;
static network_t *s_qr_network = NULL;

static multimedia_t *camera_multimedia = NULL;
static lv_obj_t *lable_wifi_status;
static lv_obj_t *wifi_info_detail_label;  /* WiFi info panel content (right of camera) */
static lv_obj_t *wifi_win_main = NULL;
static lv_timer_t *wifi_info_timer = NULL;
static lv_timer_t *s_start_pair_timer = NULL;
static lv_obj_t *s_preview_status_label = NULL;
static lv_obj_t *s_preview_area = NULL;
static lv_timer_t *s_start_preview_timer = NULL;
static lv_timer_t *s_preview_ready_timer = NULL;
static uint32_t s_preview_seen_seq = 0;

static char play_path[64] = {"/usr/tips"};
static char s_qr_header_status[96];
static volatile int s_header_status_dirty;
/* Scan thread's stream handle so close() can force-unreg to break a blocking stream_get_frame */
static volatile int s_qr_scan_streamer = -1;
static int s_qr_preview_stream_idx = -1;
static int s_qr_scan_stream_idx = -1;

static void qr_resolve_stream_indexes(void)
{
    s_qr_preview_stream_idx = -1;
    s_qr_scan_stream_idx = -1;
    multimedia_t *mm = camera_multimedia ? camera_multimedia : (multimedia_t *)app_main_get_multimedia();
    if (!mm)
        return;
    tablet_camera_streams_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.idx_preview = -1;
    cs.idx_qr = -1;
    if (tablet_camera_streams_resolve(mm, &cs) == 0) {
        s_qr_preview_stream_idx = cs.idx_preview;
        s_qr_scan_stream_idx = cs.idx_qr;
    }
}

/* Pairing thread only sets text here; wifi_info_timer_cb applies on GUI thread */
static void qr_set_header_text(const char *text)
{
    if (!text)
        return;
    strncpy(s_qr_header_status, text, sizeof(s_qr_header_status) - 1);
    s_qr_header_status[sizeof(s_qr_header_status) - 1] = '\0';
    s_header_status_dirty = 1;
}

static int nb_play_wav_only(const char *name)
{
    multimedia_t *m = (multimedia_t *)app_main_get_multimedia();
    if (!m || !m->audio || !m->audio->play_notice || !name)
        return -1;
    char str[128] = {0};
    sprintf(str, "%s/%s.wav", play_path, name);
    m->audio->play_notice(0, str);
    return 0;
}

/* Play tip WAV only — never overwrite header with tip filename (was causing
 * WaitForScanning / TickTick / Connecting / ConnectWiFiOK flicker). */
static int nb_play_file(char *name)
{
    return nb_play_wav_only(name);
}

/* Stable header + detail helpers (pair thread may call; timer applies text). */
static void qr_ui_show_scanning(void)
{
    qr_set_header_text(lan("WaitForScanning") ? lan("WaitForScanning") : "Scanning...");
}

static void qr_ui_show_connecting(const char *ssid)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s%s",
             lan("connecting") ? lan("connecting") : "Connecting",
             (ssid && ssid[0]) ? ": " : "",
             (ssid && ssid[0]) ? ssid : "");
    qr_set_header_text(buf);
}

static void qr_ui_show_connected(const char *ssid, const char *ip)
{
    char buf[128];
    const char *s = (ssid && ssid[0]) ? ssid : "-";
    const char *i = (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0) ? ip : "-";
    snprintf(buf, sizeof(buf), "%s: %s  IP:%s",
             lan("connected") ? lan("connected") : "Connected", s, i);
    qr_set_header_text(buf);
}

static void qr_set_entry_header(void)
{
    network_t *net = s_qr_network ? s_qr_network : (network_t *)app_main_get_network();

    if (net && net->wifi && net->wifi->station) {
        network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;

        if (net->wifi->station->get_state(&state) == 0 &&
            state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
            char ssid[33] = {0};
            char ip[64] = {0};
            char buf[96];

            net->wifi->station->get_ssid(ssid, sizeof(ssid));
            net->wifi->station->get_ip_address(ip, sizeof(ip));
            if (ssid[0] == '\0')
                strncpy(ssid, "WiFi", sizeof(ssid) - 1);
            snprintf(buf, sizeof(buf), "%s: %s%s%s",
                     lan("connected") ? lan("connected") : "Connected",
                     ssid,
                     ip[0] ? "  IP:" : "",
                     ip[0] ? ip : "");
            qr_set_header_text(buf);
            return;
        }
    }

    qr_set_header_text(lan("WaitForScanning") ? lan("WaitForScanning") : "WaitForScanning");
    (void)nb_play_wav_only("WaitForScanning");
}

typedef enum {
	TABLET_SCAN_STATE_INIT = 0,
	TABLET_QR_OK = 1,
	TABLET_QR_TIMEOUT = 2,
	TABLET_WAV_OK = 3,
	TABLET_WAV_TIMEOUT = 4,
} scan_state_t;

static volatile scan_state_t enScanState = TABLET_SCAN_STATE_INIT;

/* MeCard WIFI fields may be in any order (WIFI:S:..;T:..;P:.. or WIFI:T:..;P:..;S:..).
 * Only treat KEY as a field start when at buffer start, after ';', or right after "WIFI:". */
static char *wifi_mecard_find_field(char *buf, char key)
{
    char pat[3] = { key, ':', '\0' };
    char *p = buf;

    if (!buf || !buf[0])
        return NULL;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == buf || p[-1] == ';' ||
            (p == buf + 5 && (strncmp(buf, "WIFI:", 5) == 0 || strncmp(buf, "wifi:", 5) == 0))) {
            return p + 2;
        }
        p++;
    }
    return NULL;
}

static int wifi_mecard_copy_field(const char *src, char *dst, int dst_len)
{
    int n = 0;

    if (!src || !dst || dst_len <= 1)
        return 0;
    for (int i = 0; src[i] != '\0' && src[i] != ';'; i++) {
        if (n >= dst_len - 1)
            break;
        dst[n++] = src[i];
    }
    dst[n] = '\0';
    return n;
}

static int nb_parser_wifi_info(char *buf,wifi_info_t *pInfo)
{
    char *ssid;
    char *pwd;

    if (!buf || !pInfo) {
        return -1;
    }
    memset(pInfo, 0, sizeof(*pInfo));

    ssid = wifi_mecard_find_field(buf, 'S');
    if (!ssid)
        ssid = wifi_mecard_find_field(buf, 's');
    if (!ssid)
        return -1;
    pInfo->ssidLen = wifi_mecard_copy_field(ssid, pInfo->ssid, WIFI_SSID_LEN);
    if (pInfo->ssidLen <= 0)
        return -1;

    pwd = wifi_mecard_find_field(buf, 'P');
    if (!pwd)
        pwd = wifi_mecard_find_field(buf, 'p');
    if (pwd)
        pInfo->pwdLen = wifi_mecard_copy_field(pwd, pInfo->pwd, WIFI_PWD_LEN);

    return 0;
}

/* Last SSID that QR pairing marked success — used to keep scanning without
 * disconnect/reconnect when the same code is still in view. */
static char s_last_ok_ssid[WIFI_SSID_LEN];
/* Debounce same-SSID re-pair while wpa is restarting / QR still in frame. */
static long long s_last_ok_ms;
/* Snapshot of IP before QR-triggered disconnect+connect. Stale leftover IP must
 * never count as success (UI showed new SSID + old IP + 已连接). */
static char s_pair_old_ip[64];
static int s_pair_seen_down;
#ifndef QR_SAME_SSID_DEBOUNCE_MS
#define QR_SAME_SSID_DEBOUNCE_MS 5000
#endif

static int qr_ip_valid(const char *ip)
{
    return (ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0);
}

/* Prefer live IP even if station state briefly flickers during wpa restart. */
static int qr_sta_has_ip(char *ip, size_t ip_len)
{
    if (ip && ip_len)
        ip[0] = '\0';
    if (!s_qr_network || !s_qr_network->wifi || !s_qr_network->wifi->station)
        return 0;
    if (!ip || ip_len == 0)
        return 0;
    s_qr_network->wifi->station->get_ip_address(ip, ip_len);
    return qr_ip_valid(ip);
}

/* 1 = already online (skip pair UI/reconnect), 0 = started connect, -1 = error */
static int scan_finished_handle(wifi_info_t *pInfo)
{
    char ip[64] = {0};
    long long now_ms;

    if (pInfo)
        memcpy(&g_connect_info, pInfo, sizeof(g_connect_info));

    log_debug("ssid: %s len=%d, pwd len=%d", pInfo->ssid, pInfo->ssidLen, pInfo->pwdLen);

    if (strnlen(pInfo->pwd, WIFI_PWD_LEN - 1) > 0 && strnlen(pInfo->pwd, WIFI_PWD_LEN - 1) < 8) {
        nb_play_file("WiFiPwdError-Reinput");
        return -1;
    }

    now_ms = system_get_sys_run_time64() / 1000;
    /* Same SSID already online, or just paired within debounce window → silent skip.
     * Avoids killall wpa loop while the same QR stays in view. */
    if (s_last_ok_ssid[0] != '\0' && pInfo->ssid[0] != '\0' &&
        strcmp(s_last_ok_ssid, pInfo->ssid) == 0) {
        int has_ip = qr_sta_has_ip(ip, sizeof(ip));
        int in_debounce = (s_last_ok_ms > 0 &&
                           (now_ms - s_last_ok_ms) >= 0 &&
                           (now_ms - s_last_ok_ms) < QR_SAME_SSID_DEBOUNCE_MS);
        if (has_ip || in_debounce) {
            eLinkState = LINK_STATE_SERVER_CONNECTED;
            if (!qr_ip_valid(ip))
                qr_sta_has_ip(ip, sizeof(ip));
            qr_ui_show_connected(pInfo->ssid, ip);
            log_info_app("qr_scan: already on '%s' ip=%s debounce=%d, skip reconnect",
                         pInfo->ssid, qr_ip_valid(ip) ? ip : "-", in_debounce);
            return 1;
        }
    }

    nb_play_file("TickTick");
    /* Capture old IP before disconnect so pair success cannot accept leftovers. */
    s_pair_old_ip[0] = '\0';
    s_pair_seen_down = 0;
    qr_sta_has_ip(s_pair_old_ip, sizeof(s_pair_old_ip));
    eLinkState = LINK_STATE_WIFI_CONNECTING;
    qr_ui_show_connecting(pInfo->ssid);
    if (enScanState == TABLET_QR_OK)
        nb_play_file("ScanOK-ConnectingToWiFi");

    if (s_qr_network && s_qr_network->wifi && s_qr_network->wifi->station) {
        network_wifi_station_config_t config = {0};
        s_qr_network->wifi->station->disconnect();
        strncpy(config.ssid, pInfo->ssid, sizeof(config.ssid) - 1);
        strncpy(config.passphrase, pInfo->pwd, sizeof(config.passphrase) - 1);
        config.security_type = 0;
        s_qr_network->wifi->station->connect(&config);
    }
    return 0;
}

/* Success after QR disconnect+connect:
 * - Prefer live SSID match when get_ssid works.
 * - Platform get_ssid is often empty: require link-down observed, then a valid
 *   IP that is NOT the pre-pair leftover (same IP = still on old AP). */
static int qr_pair_sta_is_success(network_wifi_station_state_e state,
                                  const char *cur_ssid, const char *ip)
{
    if (state != NETWORK_WIFI_STATION_STATE_CONNECTED || g_connect_info.ssid[0] == '\0')
        return 0;
    if (!qr_ip_valid(ip))
        return 0;
    if (cur_ssid && cur_ssid[0] != '\0')
        return strcmp(cur_ssid, g_connect_info.ssid) == 0;
    /* get_ssid stub: never accept leftover IP from before disconnect. */
    if (!s_pair_seen_down)
        return 0;
    if (s_pair_old_ip[0] != '\0' && strcmp(ip, s_pair_old_ip) == 0)
        return 0;
    return 1;
}

static int pair_net_work_process(void)
{
    int iTimeOut = 60;  /* Timeout in seconds */
    int iTimeSec = system_get_sys_run_time_s();
    long long last_log_s = -1;
    AK_CHECK_POINT();
    while (nRunScan == 1 && eLinkState == LINK_STATE_WIFI_CONNECTING && ((system_get_sys_run_time_s() - iTimeSec) < iTimeOut)) {
        network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;
        char cur_ssid[WIFI_SSID_LEN] = {0};
        char ip[64] = {0};
        long long elapsed;

        if (s_qr_network && s_qr_network->wifi && s_qr_network->wifi->station) {
            s_qr_network->wifi->station->get_state(&state);
            s_qr_network->wifi->station->get_ip_address(ip, sizeof(ip));
            if (state != NETWORK_WIFI_STATION_STATE_CONNECTED) {
                s_pair_seen_down = 1;
            } else {
                s_qr_network->wifi->station->get_ssid(cur_ssid, sizeof(cur_ssid));
                /* Also treat "IP cleared / changed away from old" as link rebuild. */
                if (!qr_ip_valid(ip) ||
                    (s_pair_old_ip[0] != '\0' && strcmp(ip, s_pair_old_ip) != 0))
                    s_pair_seen_down = 1;
            }
        }
        if (qr_pair_sta_is_success(state, cur_ssid, ip)) {
            if (tablet_setting_wifi_save_sta(g_connect_info.ssid, g_connect_info.pwd) != 0)
                log_warn_app("qr_scan: connect ok but settings.json wifi save failed");
            strncpy(s_last_ok_ssid, g_connect_info.ssid, sizeof(s_last_ok_ssid) - 1);
            s_last_ok_ssid[sizeof(s_last_ok_ssid) - 1] = '\0';
            s_last_ok_ms = system_get_sys_run_time64() / 1000;
            eLinkState = LINK_STATE_SERVER_CONNECTED;
            qr_ui_show_connected(g_connect_info.ssid, ip);
            nb_play_file("ConnectWiFiOK");
            log_info_app(" pair network done [t: %lld s] ssid='%s' ip='%s' cur_ssid='%s' old_ip='%s'",
                         system_get_sys_run_time_s() - iTimeSec, g_connect_info.ssid,
                         ip[0] ? ip : "-", cur_ssid[0] ? cur_ssid : "-",
                         s_pair_old_ip[0] ? s_pair_old_ip : "-");
            return 0;
        }
        elapsed = system_get_sys_run_time_s() - iTimeSec;
        if (elapsed != last_log_s) {
            last_log_s = elapsed;
            log_info_app(" pair network wait state=%d cur_ssid='%s' ip='%s' want='%s' down=%d old_ip='%s' [t: %lld s]",
                         (int)state, cur_ssid[0] ? cur_ssid : "-",
                         ip[0] ? ip : "-",
                         g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                         s_pair_seen_down,
                         s_pair_old_ip[0] ? s_pair_old_ip : "-",
                         elapsed);
        }
        system_sleep_ms(100);
    }

	if (system_get_sys_run_time_s() - iTimeSec >= iTimeOut)  /* Connect timeout */
    {
		//aa_ls_play_file (ak_get_res_path(), "NetWorkConnectFail-CheckWifiRightAndCloseRoute");
        nb_play_file ("NetWorkConnectFail-CheckWifiRightAndCloseRoute");
        //return -3;
    }

    return -3;
}

static int face_svp_save_pics(const char *path, int index, char *buf, int len)
{
    FILE *fd = NULL;
    char acFilePath[255] = {0};
    sprintf(acFilePath, "%s/yuv_frame_%d.yuv", path, index);
    fd = fopen(acFilePath, "w+b");
    if (fd) {
        fwrite(buf, 1, len, fd);
        fclose(fd);
    } else {

        return -1;
    }
    return 0;
}


/* Pair thread must observe this often: zxing_scan_qrcode can take long; otherwise UI thread
 * blocks forever in g_thread_join(pair_th) when user leaves QR tab. */
static int qr_pairing_should_stop(void)
{
    return (nRunScan == 0 || eLinkState == LINK_STATE_QUIT);
}

static int qr_scan(wifi_info_t *info)
{

	enScanState = TABLET_SCAN_STATE_INIT;

	//qrscanned info
	char cQrInfo[512]; /// fixme enough??
    long long us;
    uint8_t *nv12_copy = NULL;
    uint8_t *nv21_tmp = NULL;
    size_t nv12_cap = 0;
    uint32_t seq = 0;

	int ret = -1;
	int ic = 0;
    unsigned loop_cnt = 0;
    unsigned zxing_calls = 0;
    unsigned zxing_ok = 0;
    unsigned zxing_nv21_tried = 0;
    unsigned copy_fail = 0;
    unsigned last_seen_ok = 0;

    /* 扫码源：优先使用 UI preview 画面（tablet_video_ctrl 里已经拿到 NV12 并显示在屏幕上）。 */
    nv12_cap = (uint32_t)LCD_HOR_RES_MAX * (uint32_t)LCD_VER_RES_MAX * 3u / 2u;
    nv12_copy = (uint8_t *)malloc(nv12_cap);
    if (!nv12_copy) {
        return -1;
    }
    nv21_tmp = (uint8_t *)malloc(nv12_cap);
    if (!nv21_tmp) {
        free(nv12_copy);
        return -1;
    }

    log_info_app("qr_scan: start (source=preview nv12) cap=%zu every_n=%d zxing_fn=%p",
                 nv12_cap, (int)QR_PAIR_ZXING_EVERY_N, (void *)zxing_scan_qrcode);

    // scanned QR info in loop
    while(nRunScan == 1 && eLinkState == LINK_STATE_QR_OR_WAV_INFO_GETTING)
    {
        //log_warn_app( "eLinkState=%d", eLinkState);

    	if(enScanState == TABLET_WAV_OK)
    	{
    		ret = 0;
			break;
		}
		if(enScanState == TABLET_WAV_TIMEOUT)
		{
			ret = -1;
			break;
		}

        int iScanedLen = 0;
        /* Close requested: break early and let this thread unreg safely */
        if (nRunScan == 0 || eLinkState == LINK_STATE_QUIT) {
            ret = -1;
            break;
        }
        loop_cnt++;
        int fw = 0, fh = 0;
        int copy_ret = tablet_video_ctrl_copy_latest_nv12(nv12_copy, nv12_cap, &fw, &fh, &seq);
        if (copy_ret <= 0) {
            copy_fail++;
            if ((copy_fail % 50u) == 0u) {
                log_warn_app("qr_scan: no new preview frame (copy_ret=%d) loop=%u fail=%u",
                             copy_ret, loop_cnt, copy_fail);
            }
            system_sleep_ms(30);
            continue;
        }

        if (qr_pairing_should_stop()) {
            ret = -1;
            break;
        }

        /* ZXing on every frame is too heavy; also limits how long one iteration can run after quit. */
        {
            static unsigned s_zxing_stride;
            s_zxing_stride++;
            if (QR_PAIR_ZXING_EVERY_N > 1 &&
                (s_zxing_stride % (unsigned)QR_PAIR_ZXING_EVERY_N) != 0u) {
                system_sleep_ms(5);
                continue;
            }
        }

		if(ic++%20 == 0)
        {
			log_debug("qr_scan: preview frame w=%d h=%d seq=%u loop=%u zxing_calls=%u ok=%u",
                      fw, fh, (unsigned)seq, loop_cnt, zxing_calls, zxing_ok);
        }

        if (qr_pairing_should_stop()) {
            ret = -1;
            break;
        }

        /*
         * ZXing 实现对输入格式有差异：有的版本吃 NV21（VU），有的能吃 NV12（UV）。
         * 这里两种都试一次（代价很小），确保"看到预览就能解码"。
         */
        long long t0 = system_get_sys_run_time64();
        zxing_calls++;
        iScanedLen = zxing_scan_qrcode(fw, fh, nv12_copy, cQrInfo, sizeof(cQrInfo));
        us = system_get_sys_run_time64() - t0;
        if (iScanedLen <= 0) {
            size_t ysz = (size_t)fw * (size_t)fh;
            size_t csz = ysz / 2u;
            if (ysz + csz <= nv12_cap) {
                memcpy(nv21_tmp, nv12_copy, ysz);
                /* NV12 UV -> NV21 VU */
                for (size_t i = 0; i + 1 < csz; i += 2) {
                    nv21_tmp[ysz + i] = nv12_copy[ysz + i + 1];
                    nv21_tmp[ysz + i + 1] = nv12_copy[ysz + i];
                }
                zxing_nv21_tried++;
                t0 = system_get_sys_run_time64();
                iScanedLen = zxing_scan_qrcode(fw, fh, nv21_tmp, cQrInfo, sizeof(cQrInfo));
                us = system_get_sys_run_time64() - t0;
            }
        }

        if ((loop_cnt % 60u) == 0u) {
            log_info_app("qr_scan: stats loop=%u copy_fail=%u zxing_calls=%u nv21_try=%u ok=%u last_us=%lld last_ret=%d",
                         loop_cnt, copy_fail, zxing_calls, zxing_nv21_tried, zxing_ok, us, iScanedLen);
        }

        if (qr_pairing_should_stop()) {
            ret = -1;
            break;
        }

//        log_info_app( "zbar scan used time = %llums scanedlen=%d ", us / 1000, iScanedLen);
        /// get scanlen of not
        if (iScanedLen > 0)
        {
            zxing_ok++;
            /* Print once when first detected, then only when content changes. */
            if (!last_seen_ok) {
                log_info_app("qr_scan: DETECTED (len=%d, us=%lld) cQrInfo=%s", iScanedLen, us, cQrInfo);
                last_seen_ok = 1;
            } else {
                log_debug("qr_scan: detected again (len=%d us=%lld)", iScanedLen, us);
            }
            /* Parse QR code payload */
            ret = nb_parser_wifi_info(cQrInfo,info);
            if (ret != 0) {
                log_warn_app("qr_scan: parse wifi info FAILED ret=%d cQrInfo=%s", ret, cQrInfo);
                /* keep scanning */
                system_sleep_ms(80);
                continue;
            }
            log_info_app("qr_scan: parsed ssid='%s' pwd_len=%d", info->ssid, info->pwdLen);
			enScanState = TABLET_QR_OK;
			break;
    	}

        system_sleep_ms(100);
    }
    if (nv21_tmp)
        free(nv21_tmp);
    if (nv12_copy)
        free(nv12_copy);
	if(ret != 0)
	{
		enScanState = TABLET_QR_TIMEOUT;
		system_sleep_ms(100);
	}

	return ret;
}

static int nb_scan_connect_run(void *arg)
{
    log_warn_app( "in: ");

	int ret = -1;
	wifi_info_t stWifiInfo;

    /* Pairing: YUV stream at higher frame rate */
	while(nRunScan == 1 && eLinkState != LINK_STATE_QUIT)
	{
        log_warn_app( "eLinkState=%d", eLinkState);

		if(eLinkState == LINK_STATE_RESET || eLinkState == LINK_STATE_SERVER_CONNECTED)
		{
			char cur_ip[64] = {0};
			int already_online = (s_last_ok_ssid[0] != '\0' && qr_sta_has_ip(cur_ip, sizeof(cur_ip)));

			/* Keep connected banner while background-scanning; else show Scanning. */
			if (already_online)
				qr_ui_show_connected(s_last_ok_ssid, cur_ip);
			else
				qr_ui_show_scanning();

			eLinkState = LINK_STATE_QR_OR_WAV_INFO_GETTING;
			memset(&stWifiInfo, 0x00, sizeof(stWifiInfo));

			ret = qr_scan(&stWifiInfo);
			if (ret == 0)
			{
				eLinkState = LINK_STATE_RECV_QR_OR_WAV_INFO;
				ret = scan_finished_handle(&stWifiInfo);
				if (ret == 1) {
					/* Same SSID already online — no reconnect, no UI flash. */
					eLinkState = LINK_STATE_SERVER_CONNECTED;
					system_sleep_ms(1500);
				} else if (ret == 0) {
					ret = pair_net_work_process();
					if (ret != 0) {
						if (s_qr_network && s_qr_network->wifi && s_qr_network->wifi->station)
							s_qr_network->wifi->station->disconnect();
						s_last_ok_ssid[0] = '\0';
						s_last_ok_ms = 0;
						qr_ui_show_scanning();
						eLinkState = LINK_STATE_RESET;
					} else {
						/* Stay connected UI; continue scanning for a different QR. */
						eLinkState = LINK_STATE_SERVER_CONNECTED;
						system_sleep_ms(1500);
					}
				} else {
					qr_ui_show_scanning();
					eLinkState = LINK_STATE_RESET;
				}
			}
			else
			{
				/* Timeout / quit mid-scan: restore connected banner if still online. */
				if (already_online) {
					qr_ui_show_connected(s_last_ok_ssid, cur_ip);
					eLinkState = LINK_STATE_SERVER_CONNECTED;
				} else {
					eLinkState = LINK_STATE_RESET;
				}
			}
            /* Default YUV frame rate */
		}

		system_sleep_ms(100);
	}

    log_warn_app("out: ConnectTaskQuit");

	return 0;
}

/* WiFi info panel: same layout as WLAN settings status (backup/current tablet_setting_wifi) */
static void wifi_info_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_header_status_dirty && lable_wifi_status) {
        s_header_status_dirty = 0;
        lv_label_set_text(lable_wifi_status, s_qr_header_status);
    }
    if (!wifi_info_detail_label || !s_qr_network)
        return;
    char buf[384];
    buf[0] = '\0';
    if (s_qr_network->wifi && s_qr_network->wifi->station) {
        char ssid[33] = {0}, ip[64] = {0};
        network_wifi_station_state_e state = NETWORK_WIFI_STATION_STATE_DISCONNECTED;

        s_qr_network->wifi->station->get_state(&state);
        s_qr_network->wifi->station->get_ip_address(ip, sizeof(ip));
        s_qr_network->wifi->station->get_ssid(ssid, sizeof(ssid));
        if (ssid[0] == '\0' && g_connect_info.ssid[0] != '\0')
            strncpy(ssid, g_connect_info.ssid, sizeof(ssid) - 1);
        if (ssid[0] == '\0' && s_last_ok_ssid[0] != '\0')
            strncpy(ssid, s_last_ok_ssid, sizeof(ssid) - 1);

        if (eLinkState == LINK_STATE_WIFI_CONNECTING ||
            eLinkState == LINK_STATE_SERVER_CONNECTING) {
            /* Connecting wins over stale CONNECTED+old-IP during switch. */
            snprintf(buf, sizeof(buf), "SSID: %s\nPWD: %s\n%s...",
                     g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                     (g_connect_info.pwd[0] ? "******" : "-"),
                     lan("connecting") ? lan("connecting") : "Connecting");
        } else if (qr_ip_valid(ip) || eLinkState == LINK_STATE_SERVER_CONNECTED ||
                   state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
            /* Connected / already-online: stable SSID + IP (no scanning flash). */
            if (ssid[0] == '\0')
                strncpy(ssid, "-", sizeof(ssid) - 1);
            snprintf(buf, sizeof(buf), "SSID: %s\n%s\nIP: %s",
                     ssid, lan("connected") ? lan("connected") : "Connected",
                     qr_ip_valid(ip) ? ip : "-");
        } else if (eLinkState == LINK_STATE_QR_OR_WAV_INFO_GETTING ||
                   eLinkState == LINK_STATE_RESET) {
            snprintf(buf, sizeof(buf), "%s\n\n%s",
                     lan("Please scan WiFi QR code") ? lan("Please scan WiFi QR code")
                                                     : "Please scan WiFi QR code",
                     lan("Scanning") ? lan("Scanning") : "Scanning...");
        } else {
            snprintf(buf, sizeof(buf), "SSID: %s\nPWD: %s\n%s",
                     g_connect_info.ssid[0] ? g_connect_info.ssid : "-",
                     (g_connect_info.pwd[0] ? "******" : "-"),
                     lan("connect failed"));
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", lan("Please scan WiFi QR code") ? lan("Please scan WiFi QR code") : "Please scan WiFi QR code");
    }
    lv_label_set_text(wifi_info_detail_label, buf);
}

/* Build QR scan / pairing UI: header + [ camera (left) | WiFi info (right) ] */
static void create_ui(lv_obj_t *parent)
{
    tablet_setting_flat_container(parent);

    lv_obj_t *cont = lv_obj_create(parent);
    wifi_win_main = cont;
    tablet_setting_flat_container(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* Top title bar (60px) */
    lv_obj_t *header = lv_obj_create(cont);
    tablet_setting_flat_container(header);
    lv_obj_set_size(header, LV_PCT(100), GUI_SCALE_VER(56));
    lv_obj_set_style_bg_color(header, lv_color_hex(SETTING_SIDEBAR_BG), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    lv_obj_t *header_label = lv_label_create(header);
    lable_wifi_status = header_label;
    lv_label_set_text(header_label, lan("WiFi status"));
    lv_obj_set_style_text_color(header_label, lv_color_white(), 0);
    if (tablet_font_get_freetype_font(20))
        lv_obj_set_style_text_font(header_label, tablet_font_get_freetype_font(20), 0);
    else
        lv_obj_set_style_text_font(header_label, tablet_font_get_montserrat_font(20), 0);
    lv_obj_center(header_label);

    /* Right big area: row = [ camera (left) | WiFi info panel (right) ] */
    lv_obj_t *row = lv_obj_create(cont);
    tablet_setting_flat_container(row);
    lv_obj_set_size(row, LV_PCT(100), 0);
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* Top-align: right card only wraps content height, not full column strip aligned with camera top */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* Left: camera preview */
    lv_obj_t *video_area = lv_obj_create(row);
    tablet_setting_flat_container(video_area);
    lv_obj_set_size(video_area, 0, LV_PCT(100));
    lv_obj_set_flex_grow(video_area, 1);
    lv_obj_set_style_min_width(video_area, 200, 0);
    lv_obj_set_style_pad_all(video_area, 0, 0);
    lv_obj_remove_flag(video_area, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(video_area, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_bg_opa(video_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(video_area, GUI_SCALE(16), 0);
    /*
     * 优化进入速度：
     * - 这里先把 UI 画出来（不做 camera 初始化/stream_reg_client）
     * - 再通过一次性 timer 异步启动 preview，避免"点进来卡黑屏"等待硬件初始化。
     */
    s_preview_area = video_area;
    s_preview_status_label = lv_label_create(video_area);
    lv_obj_set_style_text_color(s_preview_status_label, lv_color_hex(SETTING_TEXT_DIM), 0);
    if (tablet_font_get_freetype_font(18))
        lv_obj_set_style_text_font(s_preview_status_label, tablet_font_get_freetype_font(18), 0);
    lv_label_set_text(s_preview_status_label, lan("Camera initializing..."));
    lv_obj_align(s_preview_status_label, LV_ALIGN_TOP_MID, 0, 8);

    /* Right: WiFi info panel */
    lv_obj_t *wifi_panel = lv_obj_create(row);
    tablet_setting_flat_container(wifi_panel);
    lv_obj_set_width(wifi_panel, 240);
    lv_obj_set_height(wifi_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(wifi_panel, 0);
    lv_obj_set_style_max_height(wifi_panel, LV_PCT(100), 0);
    /* Same card recipe as AI right column: CARD_BG + launcher hairline + shadow */
    lv_obj_set_style_bg_color(wifi_panel, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_bg_opa(wifi_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(wifi_panel, 0, 0);
    lv_obj_set_style_radius(wifi_panel, GUI_SCALE(16), 0);
    lv_obj_set_style_border_width(wifi_panel, 1, 0);
    lv_obj_set_style_border_color(wifi_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(wifi_panel, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(wifi_panel, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(wifi_panel, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(wifi_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(wifi_panel, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(wifi_panel, 10, 0);
    lv_obj_set_style_text_color(wifi_panel, lv_color_hex(SETTING_TEXT), 0);
    if (tablet_font_get_freetype_font(18))
        lv_obj_set_style_text_font(wifi_panel, tablet_font_get_freetype_font(18), 0);

    wifi_info_detail_label = lv_label_create(wifi_panel);
    lv_obj_set_size(wifi_info_detail_label, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(wifi_info_detail_label, lv_color_hex(SETTING_TEXT), 0);
    if (tablet_font_get_freetype_font(18))
        lv_obj_set_style_text_font(wifi_info_detail_label, tablet_font_get_freetype_font(18), 0);
    lv_label_set_text(wifi_info_detail_label, lan("Please scan WiFi QR code") ? lan("Please scan WiFi QR code") : "Please scan WiFi QR code");
    lv_label_set_long_mode(wifi_info_detail_label, LV_LABEL_LONG_WRAP);

    wifi_info_timer = lv_timer_create(wifi_info_timer_cb, 800, NULL);
}

static void preview_ready_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_preview_status_label)
        return;
    /* Poll whether preview has produced any frame yet; once yes, hide the placeholder. */
    static uint8_t *buf;
    static size_t cap;
    if (!buf) {
        cap = (uint32_t)LCD_HOR_RES_MAX * (uint32_t)LCD_VER_RES_MAX * 3u / 2u;
        buf = (uint8_t *)malloc(cap);
        if (!buf)
            return;
    }
    int w = 0, h = 0;
    int r = tablet_video_ctrl_copy_latest_nv12(buf, cap, &w, &h, &s_preview_seen_seq);
    if (r > 0) {
        log_info_app("[DBG_QR_PREVIEW] first frame w=%d h=%d seq=%u lcd=%dx%d",
                     w, h, (unsigned)s_preview_seen_seq,
                     (int)LCD_HOR_RES_MAX, (int)LCD_VER_RES_MAX);
        lv_obj_add_flag(s_preview_status_label, LV_OBJ_FLAG_HIDDEN);
        if (s_preview_ready_timer) {
            lv_timer_t *tmp = s_preview_ready_timer;
            s_preview_ready_timer = NULL;
            lv_timer_delete(tmp);
        }
    }
}

static void start_preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_start_preview_timer) {
        lv_timer_t *tmp = s_start_preview_timer;
        s_start_preview_timer = NULL;
        lv_timer_delete(tmp);
    }

    if (!s_preview_area) {
        return;
    }

    /* UI 预览：内部会根据 video_cfg.json 自动选择 ui_preview/video_preview（参数不使用）。 */
    lv_result_t pr = tablet_video_ctrl_create_for_stream(s_preview_area, 1);
    log_info_app("[DBG_QR_PREVIEW] tablet_video_ctrl_create_for_stream ret=%d area=%p",
                 (int)pr, (void *)s_preview_area);
    if (s_preview_status_label) {
        if (pr != LV_RESULT_OK) {
            lv_obj_set_style_text_color(s_preview_status_label, lv_color_hex(SETTING_ERR), 0);
            lv_label_set_text(s_preview_status_label, lan("Camera preview init failed"));
        } else {
            lv_obj_set_style_text_color(s_preview_status_label, lv_color_hex(SETTING_TEXT_DIM), 0);
            lv_label_set_text(s_preview_status_label, lan("Camera preview starting..."));
            /* Start polling for first frame, then hide placeholder. */
            s_preview_seen_seq = 0;
            if (s_preview_ready_timer) {
                lv_timer_t *tmp2 = s_preview_ready_timer;
                s_preview_ready_timer = NULL;
                lv_timer_delete(tmp2);
            }
            s_preview_ready_timer = lv_timer_create(preview_ready_timer_cb, 150, NULL);
        }
    }
}

static void start_pair_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_start_pair_timer) {
        lv_timer_t *tmp = s_start_pair_timer;
        s_start_pair_timer = NULL;
        lv_timer_delete(tmp);
    }

    /* Avoid blocking on old pair thread from UI thread. */
    if (s_pair_th_running) {
        log_warn_app("qr create: previous pair_th still running, skip new thread");
        return;
    }
    pair_th = NULL;
    s_qr_scan_streamer = -1;
    nRunScan = 1;
    eLinkState = LINK_STATE_RESET;
    pair_th = g_thread_new("pair_th", pair_th_wrapper, NULL);
}

lv_result_t tablet_setting_qr_scan_create(lv_obj_t *parent)
{
#if 1
    create_ui(parent);

    camera_multimedia = (multimedia_t *)app_main_get_multimedia();
    qr_resolve_stream_indexes();
    log_info_app("qr streams: idx_preview=%d idx_qr=%d", s_qr_preview_stream_idx, s_qr_scan_stream_idx);
    s_qr_network = (network_t *)app_main_get_network();
    if (s_qr_network && s_qr_network->wifi && s_qr_network->wifi->station)
        s_qr_network->wifi->station->init();
    qr_set_entry_header();

    /* Defer preview start so UI can appear immediately. */
    if (s_start_preview_timer) {
        lv_timer_t *tmp = s_start_preview_timer;
        s_start_preview_timer = NULL;
        lv_timer_delete(tmp);
    }
    s_start_preview_timer = lv_timer_create(start_preview_timer_cb, 1, NULL);
    if (s_start_preview_timer)
        lv_timer_set_repeat_count(s_start_preview_timer, 1);

    /*
     * 先让相机预览跑起来（tablet_video_ctrl_create_for_stream 已注册 client + timer），
     * 再启动扫码线程去 stream_get_frame + ZXing，避免 UI 首屏卡顿/黑屏。
     */
    if (s_start_pair_timer) {
        lv_timer_t *tmp = s_start_pair_timer;
        s_start_pair_timer = NULL;
        lv_timer_delete(tmp);
    }
    s_start_pair_timer = lv_timer_create(start_pair_timer_cb, 200, NULL);
    if (s_start_pair_timer)
        lv_timer_set_repeat_count(s_start_pair_timer, 1);
#else
    create_ui1(parent);
#endif

    return LV_RESULT_OK;
}


lv_result_t tablet_setting_qr_scan_close()
{
    log_warn_app("in: ");

    /* Stop scan thread + unreg scan client BEFORE closing preview. Destroying the preview
     * broadcast client first while scan still holds a second reader on stream-1 can crash
     * inside circular_buffer / thread teardown (cam_bc_v1 exit then segfault). */
    eLinkState = LINK_STATE_QUIT;
    nRunScan = 0;

    if (wifi_info_timer) {
        lv_timer_t *t = wifi_info_timer;
        wifi_info_timer = NULL;
        lv_timer_delete(t);
    }
    if (s_start_pair_timer) {
        lv_timer_t *t = s_start_pair_timer;
        s_start_pair_timer = NULL;
        lv_timer_delete(t);
    }
    if (s_start_preview_timer) {
        lv_timer_t *t = s_start_preview_timer;
        s_start_preview_timer = NULL;
        lv_timer_delete(t);
    }
    if (s_preview_ready_timer) {
        lv_timer_t *t = s_preview_ready_timer;
        s_preview_ready_timer = NULL;
        lv_timer_delete(t);
    }
    wifi_info_detail_label = NULL;
    s_preview_area = NULL;
    s_preview_status_label = NULL;

    /* Unreg scan reader here so stream_get_frame can return (broadcast wait is bounded but join
     * was still stuck in ZXing / disk I/O). ZXing uses malloc copy of YUV, not frame.data. */
    {
        multimedia_t *mm = camera_multimedia ? camera_multimedia : (multimedia_t *)app_main_get_multimedia();
        int h = s_qr_scan_streamer;
        if (h >= 0 && mm && mm->camera) {
            log_warn_app("pre-unreg QR scan client before join");
            s_qr_scan_streamer = -1;
            mm->camera->stream_unreg_client(h);
        }
    }

    /* Never block UI forever waiting scanner thread. Poll shortly then continue close. */
    log_warn_app("wait pair_th exit (non-blocking)");
    {
        int wait_ms = 0;
        while (s_pair_th_running && wait_ms < 400) {
            system_sleep_ms(20);
            wait_ms += 20;
        }
        if (s_pair_th_running)
            log_warn_app("pair_th still running, continue close without join");
    }

    log_warn_app("tablet_video_ctrl_close enter");
    system_sleep_ms(30);
    tablet_video_ctrl_close();
    log_warn_app("tablet_video_ctrl_close done");

    /* Remove QR tab body so leaving the tab (or re-entering) can call create_ui again.
     * Safe after stream_unreg: preview callback/timer are torn down in tablet_video_ctrl_close.
     * Whole settings window delete on back still only frees objects once. */
    if (wifi_win_main != NULL) {
        if (lv_obj_is_valid(wifi_win_main))
            lv_obj_delete(wifi_win_main);
        wifi_win_main = NULL;
    }
    lable_wifi_status = NULL;

    log_warn_app( "out: ");

    return LV_RESULT_OK;
}

#else
/* Stubs when CONFIG_FW_NETWORK_WIFI is not defined so linker has both symbols */
#include "tablet_setting_qr_scan.h"
lv_result_t tablet_setting_qr_scan_create(lv_obj_t *parent)
{
    (void)parent;
    return LV_RESULT_OK;
}
lv_result_t tablet_setting_qr_scan_close(void)
{
    return LV_RESULT_OK;
}
#endif
