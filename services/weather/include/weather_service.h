// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/services/weather.
 *
 * Weather service: request weather update and invoke callback with image path and text.
 */
#ifndef APPLICATION_WEATHER_SERVICE_H
#define APPLICATION_WEATHER_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "app_globle_config.h"

#if CONFIG_NETWORK_SUPPORTED
typedef int (*update_weather_callback)(const char *image, const char *text);

/**
 * @brief Request weather update and invoke callback with image path and description text.
 * @param callback Callback(image_path, text). Return 0 on success.
 * @return 0 on success, negative on failure.
 */
int app_weather_update_request(update_weather_callback callback);

#endif /* CONFIG_NETWORK_SUPPORTED */

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_WEATHER_SERVICE_H */

