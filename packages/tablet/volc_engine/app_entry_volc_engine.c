#define LOG_TAG "TABLET_APP_ENTRY_VOLC_ENGINE"

#include "config.h"
#include "app_main.h"
#include "network.h"

#include "tablet_volc.h"

#include "log.h"

#include <string.h>

#define APP_ENTRY_EXPORT __attribute__((visibility("default"))) __attribute__((used))

static int network_ready(void)
{
#if CONFIG_NETWORK_SUPPORTED
    network_t *s_network = (network_t *)app_main_get_network();
    if (!s_network || !s_network->wifi || !s_network->wifi->station)
        return 0;

    network_wifi_station_state_e state;
    if (s_network->wifi->station->get_state(&state) != NETWORK_OK)
        return 0;

    if (state != NETWORK_WIFI_STATION_STATE_CONNECTED)
        return 0;

    char ip[64] = {0};
    s_network->wifi->station->get_ip_address(ip, sizeof(ip));
    if (ip[0] == '\0')
        return 0;
    if (strcmp(ip, "0.0.0.0") == 0)
        return 0;
#endif
    return 1;
}

APP_ENTRY_EXPORT int app_entry_start_volc_engine(lv_obj_t *main)
{
    if (!main)
        return -1;
    if (!network_ready())
        log_warn("volc_engine: network not ready; open Volc after WiFi ready (RTC init is deferred)");

    (void)tablet_volc_init();
    (void)tablet_volc_create(main);
    return 0;
}

static int (*const s_keep_app_entry_volc_engine)(lv_obj_t *) = app_entry_start_volc_engine;
