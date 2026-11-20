// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Common headless ipcamera app (packages/ipcamera/ipc_cam).
 * Exports module_init / module_start; module_start blocks to keep ipc_main alive.
 *
 * Link model matches wearable/dashcam (rtsp only). Nebula is optional and loaded
 * with dlopen at module_start; headless stubs + RTLD_GLOBAL cover symbols that
 * libnebula_protocol needs but this product does not build (no player/display).
 *
 * Nebula start order mirrors Nova tablet_call: ensure FW network/WiFi, wait for
 * a usable IPv4, then open Nebula (do not change nebula_protocol itself).
 */
#define LOG_TAG "IPCAMERA_IPC_CAM"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app_main.h"
#include "ipc_cam_record.h"
#include "ipc_cam_rtsp.h"
#include "ipc_cam_storage.h"
#include "log.h"
#include "multimedia.h"
#include "system_time.h"

#ifdef CONFIG_APP_NEBULA
#include "nebula_protocol.h"
#include "network.h"
#endif

/* Same as MULTIMEDIA_CFG_VIDEO_FILE; settings.json config_path should match. */
#define IPC_CAM_VIDEO_CFG_PATH  "/etc/config/fw_video_config.json"
#define IPC_CAM_NEBULA_SO_PATH  "/usr/lib/libnebula_protocol.so"
#define IPC_CAM_ZXING_SO_PATH   "/usr/lib/libzxing.so"

#ifndef IPC_CAM_NEBULA_STATIC
#define IPC_CAM_NEBULA_STATIC 0
#endif

#define MODULE_EXPORT __attribute__((visibility("default"))) __attribute__((used))

static char s_config_path[256];
static volatile int s_running;

#ifdef CONFIG_APP_NEBULA
static nebula_protocol_t *s_nebula;
#if !IPC_CAM_NEBULA_STATIC
static void *s_nebula_dl;
static void *s_zxing_dl;
#endif
#endif

static multimedia_t *ipc_cam_get_multimedia(void)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();

    if (mm == NULL) {
        (void)app_main_open_framework_handles();
        mm = (multimedia_t *)app_main_get_multimedia();
    }
    return mm;
}

#ifdef CONFIG_APP_NEBULA
static int ipc_cam_ipv4_usable(const char *ip)
{
    return ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

/* Kernel ifaddrs: shell udhcpc may own wlan0 without FW station CONNECTED. */
static int ipc_cam_iface_ipv4(const char *iface, char *ip, size_t ip_len)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    int ok = -1;

    if (!iface || !ip || ip_len == 0)
        return -1;
    ip[0] = '\0';
    if (getifaddrs(&ifaddr) != 0)
        return -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, iface) != 0)
            continue;
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr == 0 || sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            continue;
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, (socklen_t)ip_len) != NULL &&
            ipc_cam_ipv4_usable(ip)) {
            ok = 0;
        }
        break;
    }

    freeifaddrs(ifaddr);
    return ok;
}

/** Same idea as tablet_call ipc_get_local_ipv4 / ipc_call_network_ready. */
static int ipc_cam_get_local_ipv4(char *ip, size_t ip_len)
{
    network_t *net = (network_t *)app_main_get_network();
    static const char *ifaces[] = {
        "wlan0", "eth0", "eth1", "net0", "enp0s0",
    };
    unsigned i;

    if (!ip || ip_len == 0)
        return -1;
    ip[0] = '\0';

    if (net) {
        if (net->wifi && net->wifi->get_ip &&
            net->wifi->get_ip(ip) == NETWORK_OK && ipc_cam_ipv4_usable(ip))
            return 0;

        if (net->wifi && net->wifi->station) {
            network_wifi_station_state_e state;

            if (net->wifi->station->get_state(&state) == NETWORK_OK &&
                state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
                if (net->wifi->station->get_ip_address(ip, ip_len) == NETWORK_OK &&
                    ipc_cam_ipv4_usable(ip))
                    return 0;
            }
        }

        if (net->ethernet) {
            if (net->ethernet->get_ip && net->ethernet->get_ip(ip) == NETWORK_OK &&
                ipc_cam_ipv4_usable(ip))
                return 0;
            if (net->ethernet->get_ip_by_name) {
                for (i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
                    memset(ip, 0, ip_len);
                    if (net->ethernet->get_ip_by_name(ifaces[i], ip, ip_len) == NETWORK_OK &&
                        ipc_cam_ipv4_usable(ip))
                        return 0;
                }
            }
        }
    }

    for (i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        if (ipc_cam_iface_ipv4(ifaces[i], ip, ip_len) == 0)
            return 0;
    }
    return -1;
}

static void ipc_cam_enable_iface_multicast(const char *iface)
{
    int fd;
    struct ifreq ifr;

    if (!iface || !iface[0])
        return;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= (IFF_MULTICAST | IFF_ALLMULTI);
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0)
            log_warn("ipc_cam: set %s MULTICAST/ALLMULTI failed errno=%d", iface, errno);
        else
            log_info("ipc_cam: %s multicast/allmulti enabled", iface);
    }
    close(fd);
}

/**
 * Nova tablet brings WiFi up via FW station (settings + connect) before Nebula.
 * Headless: app.sh may already have udhcpc IP — only associate via FW when empty.
 */
static void ipc_cam_ensure_wifi_for_nebula(void)
{
    network_t *net;
    char ip[64] = {0};

    (void)app_main_open_network_handle();
    net = (network_t *)app_main_get_network();
    if (!net || !net->wifi) {
        log_warn("ipc_cam: network/wifi unavailable before nebula");
        return;
    }

    if (net->wifi->init)
        (void)net->wifi->init();
    if (net->wifi->enable)
        (void)net->wifi->enable();

    if (net->wifi->station && net->wifi->station->init)
        (void)net->wifi->station->init();

    if (ipc_cam_get_local_ipv4(ip, sizeof(ip)) == 0) {
        log_info("ipc_cam: ipv4 ready before nebula: %s", ip);
        return;
    }

    if (net->wifi->station && net->wifi->station->connect_from_config) {
        int ret = net->wifi->station->connect_from_config();

        log_info("ipc_cam: connect_from_config ret=%d (no IP yet)", ret);
    }
}

static int ipc_cam_wait_network_ready(int timeout_sec)
{
    int waited = 0;
    char ip[64] = {0};

    while (waited <= timeout_sec) {
        if (ipc_cam_get_local_ipv4(ip, sizeof(ip)) == 0) {
            log_info("ipc_cam: network ready ip=%s (waited=%ds)", ip, waited);
            /* Brief settle so IGMP join in Nebula sees a stable iface IP. */
            if (waited == 0)
                system_sleep_ms(1000);
            return 1;
        }
        if (waited == 0 || (waited % 5) == 0)
            log_info("ipc_cam: waiting for ipv4 before nebula (%ds/%ds)...",
                     waited, timeout_sec);
        system_sleep_ms(1000);
        waited++;
    }
    log_warn("ipc_cam: no usable ipv4 after %ds; nebula discovery may fail", timeout_sec);
    return 0;
}

static int ipc_cam_nebula_open(nebula_protocol_t **nebula)
{
#if IPC_CAM_NEBULA_STATIC
    return nebula_protocol_open(nebula);
#else
    typedef int (*nebula_open_fn)(nebula_protocol_t **);
    nebula_open_fn open_fn;

    s_zxing_dl = dlopen(IPC_CAM_ZXING_SO_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (s_zxing_dl == NULL)
        log_warn("dlopen %s failed: %s (QR pairing unavailable)", IPC_CAM_ZXING_SO_PATH,
                 dlerror() ? dlerror() : "(null)");

    s_nebula_dl = dlopen(IPC_CAM_NEBULA_SO_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (s_nebula_dl == NULL) {
        log_warn("dlopen %s failed: %s", IPC_CAM_NEBULA_SO_PATH,
                 dlerror() ? dlerror() : "(null)");
        return -1;
    }

    open_fn = (nebula_open_fn)dlsym(s_nebula_dl, "nebula_protocol_open");
    if (open_fn == NULL) {
        log_warn("dlsym nebula_protocol_open failed");
        dlclose(s_nebula_dl);
        s_nebula_dl = NULL;
        return -1;
    }
    return open_fn(nebula);
#endif
}

static void ipc_cam_start_nebula(void)
{
    nebula_protocol_t *nebula = NULL;

    ipc_cam_ensure_wifi_for_nebula();
    (void)ipc_cam_wait_network_ready(30);
    ipc_cam_enable_iface_multicast("wlan0");

    if (ipc_cam_nebula_open(&nebula) != NEBULA_PROTOCOL_OK || nebula == NULL) {
        log_warn("nebula_protocol_open failed");
#if !IPC_CAM_NEBULA_STATIC
        if (s_nebula_dl) {
            dlclose(s_nebula_dl);
            s_nebula_dl = NULL;
        }
#endif
        return;
    }
    /* Headless IPC has no LCD player. Drop Nebula VideoPushDown/Up; keep audio talk. */
    {
        typedef void (*nebula_talk_video_en_fn)(int enable);
        nebula_talk_video_en_fn set_en = NULL;
#if IPC_CAM_NEBULA_STATIC
        extern void nebula_talk_video_decode_set_enabled(int enable);
        set_en = nebula_talk_video_decode_set_enabled;
#else
        set_en = (nebula_talk_video_en_fn)dlsym(s_nebula_dl, "nebula_talk_video_decode_set_enabled");
#endif
        if (set_en) {
            set_en(0);
            log_info("nebula downlink video ignored");
        } else {
            log_warn("nebula_talk_video_decode_set_enabled missing; downlink video still parsed");
        }
    }
    if (nebula->http) {
        if (nebula->http->init)
            nebula->http->init();
        if (nebula->http->start)
            nebula->http->start();
    }
    if (nebula->network) {
        if (nebula->network->init)
            nebula->network->init();
        if (nebula->network->start)
            nebula->network->start();
    }
    s_nebula = nebula;
    app_main_register_nebula(nebula);
    log_info("nebula started (http+network)");
}
#endif

static int ipc_cam_start_pipeline(void)
{
    multimedia_t *mm = ipc_cam_get_multimedia();
    const char *video_cfg = s_config_path[0] ? s_config_path : IPC_CAM_VIDEO_CFG_PATH;

    if (mm == NULL || mm->camera == NULL) {
        log_error("multimedia/camera unavailable");
        return -1;
    }

    (void)ipc_cam_storage_init();

    /* fw_video_config.json: main+sub only (no LCD preview / AI). Pass "" for
     * ai_cfg and keep AI off so configure_streams skips ai_register_hal_config. */
    if (mm->camera->load_config == NULL ||
        mm->camera->load_config(video_cfg, "") != 0) {
        log_warn("load_config(%s) failed; keep existing", video_cfg);
    } else {
        log_info("camera config loaded video=%s (AI off)", video_cfg);
    }

    if (mm->camera->set_ai_enabled)
        mm->camera->set_ai_enabled(0);

    ipc_cam_audio_ensure_g711a();

    /* Record only when TF/SD is mounted and writable; RTSP + nebula always run. */
    if (ipc_cam_storage_is_ready()) {
        if (ipc_cam_record_start() != 0)
            log_warn("record start failed; continue without local recording");
    } else {
        log_info("no SD/TF card; skip local recording");
    }

    if (ipc_cam_rtsp_start() != 0) {
        log_error("rtsp start failed");
        return -1;
    }

#ifdef CONFIG_APP_NEBULA
    ipc_cam_start_nebula();
#endif
    return 0;
}

MODULE_EXPORT int module_init(const char *config_path)
{
    s_config_path[0] = '\0';
    if (config_path && config_path[0]) {
        strncpy(s_config_path, config_path, sizeof(s_config_path) - 1);
        s_config_path[sizeof(s_config_path) - 1] = '\0';
    }
    log_info("module_init config=%s", s_config_path[0] ? s_config_path : "(default)");
    return 0;
}

MODULE_EXPORT int module_start(void)
{
    log_info("module_start: capture+RTSP(+nebula); record if SD present");
    if (ipc_cam_start_pipeline() != 0) {
        log_error("pipeline start failed");
        return -1;
    }

    s_running = 1;
    log_info("pipeline running; blocking to keep ipc_main alive");
    while (s_running)
        system_sleep_ms(1000);
    return 0;
}

static int (*const s_keep_module_init)(const char *) = module_init;
static int (*const s_keep_module_start)(void) = module_start;
static void *const s_keep_ptrs[] __attribute__((used)) = {
    (void *)&s_keep_module_init,
    (void *)&s_keep_module_start,
};
