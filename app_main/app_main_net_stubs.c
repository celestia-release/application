// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Scheme B: ipc_main is not linked with -lnetwork, but static libs (e.g. nebula
 * ntp_sync) still reference wifi_event_*. Provide trampolines that dlopen
 * libnetwork.so on first use.
 *
 * libnetwork.so needs symbols from libmultimedia.so (e.g.
 * media_video_payload_is_keyframe). Always load multimedia first with
 * RTLD_GLOBAL before RTLD_NOW on network.
 */
#define LOG_TAG "APP MAIN"

#include <stddef.h>
#include <dlfcn.h>

#include "config.h"
#include "log.h"

typedef void (*wifi_event_handler_t)(const void *event);
typedef int (*wifi_event_init_fn)(void);
typedef int (*wifi_event_reg_fn)(int type, wifi_event_handler_t handler);

static void *app_main_ensure_multimedia_so(void)
{
    static void *mm_handle;

    if (!mm_handle) {
        mm_handle = dlopen("libmultimedia.so", RTLD_NOW | RTLD_GLOBAL);
        if (!mm_handle)
            log_error("dlopen libmultimedia.so (stub): %s", dlerror());
    }
    return mm_handle;
}

static void *app_main_network_so(void)
{
    static void *handle;

    if (!handle) {
        if (!app_main_ensure_multimedia_so())
            return NULL;
        handle = dlopen("libnetwork.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
            log_error("dlopen libnetwork.so (stub): %s", dlerror());
    }
    return handle;
}

static void *app_main_network_sym(const char *name)
{
    void *h = app_main_network_so();
    void *p;

    if (!h)
        return NULL;
    p = dlsym(h, name);
    if (!p)
        log_error("dlsym %s: %s", name, dlerror());
    return p;
}

int wifi_event_init(void)
{
    wifi_event_init_fn fn = (wifi_event_init_fn)app_main_network_sym("wifi_event_init");
    return fn ? fn() : -1;
}

int wifi_event_register_handler(int type, wifi_event_handler_t handler)
{
    wifi_event_reg_fn fn = (wifi_event_reg_fn)app_main_network_sym("wifi_event_register_handler");
    return fn ? fn(type, handler) : -1;
}

int wifi_event_unregister_handler(int type, wifi_event_handler_t handler)
{
    wifi_event_reg_fn fn = (wifi_event_reg_fn)app_main_network_sym("wifi_event_unregister_handler");
    return fn ? fn(type, handler) : -1;
}

typedef int (*network_init_fn)(void);

/*
 * app_main.c calls network_init() directly (headless start). ipc_main does not
 * link -lnetwork, so trampoline through libnetwork.so like the wifi_event_*
 * wrappers above.
 */
int network_init(void)
{
    network_init_fn fn = (network_init_fn)app_main_network_sym("network_init");
    return fn ? fn() : -1;
}

