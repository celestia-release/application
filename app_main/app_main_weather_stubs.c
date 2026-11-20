// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Scheme B: do not static-link libweather_service.a into ipc_main (it pulls
 * libcurl → DT_NEEDED at process start). Provide the same export symbol and
 * dlopen libweather_service.so on first weather request (after Home / WiFi).
 */
#define LOG_TAG "APP MAIN"

#include "config.h"

#if defined(CONFIG_APP_WEATHER)

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

#include "log.h"

/* Keep signature aligned with weather_service.h (avoid include gate on CONFIG_NETWORK). */
typedef int (*update_weather_callback)(const char *image, const char *text);
typedef int (*app_weather_update_request_fn)(update_weather_callback callback);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void *s_weather_so;
static app_weather_update_request_fn s_real;

static int weather_load_so(void)
{
    static const char *const candidates[] = {
        "libweather_service.so",
        "/usr/lib/libweather_service.so",
        "/mnt/install/lib/libweather_service.so",
        NULL
    };
    const char *err = NULL;
    int i;

    if (s_real)
        return 0;

    for (i = 0; candidates[i]; i++) {
        s_weather_so = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (s_weather_so) {
            log_info("dlopen %s ok", candidates[i]);
            break;
        }
        err = dlerror();
    }
    if (!s_weather_so) {
        log_error("dlopen libweather_service failed: %s", err ? err : "unknown");
        return -1;
    }

    s_real = (app_weather_update_request_fn)dlsym(s_weather_so, "app_weather_update_request");
    if (!s_real) {
        log_error("dlsym app_weather_update_request: %s", dlerror());
        return -1;
    }
    return 0;
}

int app_weather_update_request(update_weather_callback callback)
{
    int rc;

    pthread_mutex_lock(&s_lock);
    rc = weather_load_so();
    pthread_mutex_unlock(&s_lock);
    if (rc != 0 || !s_real)
        return -1;
    return s_real(callback);
}

#endif /* CONFIG_APP_WEATHER */
