// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * Settings config (settings.json) parser: program, launch_mode, modules (enable/type/so_path/config_path).
 * Default: current-dir settings.json, else /etc/config/settings.json
 * (legacy: app_runtime.json / app_user.json).
 */
#ifndef APPLICATION_APP_CONFIG_H
#define APPLICATION_APP_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_LAUNCH_MODE_UI       "ui"
#define APP_CONFIG_LAUNCH_MODE_HEADLESS  "headless"
#define APP_CONFIG_LAUNCH_MODE_IPCAMERA  "ipcamera" /* alias of headless for packages/ipcamera */
#define APP_CONFIG_MODULE_TYPE_STATIC   "static"
#define APP_CONFIG_MODULE_TYPE_SO        "so"
/* Default: current-dir settings.json (for NFS), else device path. */
#define APP_CONFIG_DEFAULT_RELATIVE    "settings.json"
#define APP_CONFIG_DEFAULT_SYSTEM      "/etc/config/settings.json"
#define APP_CONFIG_LEGACY_RELATIVE     "app_runtime.json"
#define APP_CONFIG_LEGACY_SYSTEM       "/etc/config/app_runtime.json"
#define APP_CONFIG_LEGACY_USER         "/etc/config/app_user.json"

/* Single module entry (enable, type, so_path, config_path). */
typedef struct app_module_item {
    const char *name;          /* key in modules */
    bool enable;
    const char *type;          /* "static" | "so" */
    const char *so_path;       /* when type=so */
    const char *config_path;   /* optional */
    struct app_module_item *next;
} app_module_item_t;

/* Parsed runtime config (opaque; implementation uses cJSON). */
typedef struct app_runtime_config app_runtime_config_t;

/**
 * app_main_init - Initialize app_main internal state. Call once after framework_init/runtime_bootstrap.
 */
void app_main_init(void);

/**
 * app_main_load_config - Load and parse settings.json from path.
 * @path: NULL or "" to use default APP_CONFIG_DEFAULT_PATH.
 * @return 0 on success, non-zero (app_main/system error code) on failure.
 */
int app_main_load_config(const char *path);

/**
 * app_main_get_launch_mode_ui - True if launch_mode is "ui"; false for headless/ipcamera/missing.
 */
bool app_main_get_launch_mode_ui(void);

/**
 * app_main_get_gui_config_path - Config path for GUI (e.g. tablet config_path). NULL if not set.
 */
const char *app_main_get_gui_config_path(void);

/**
 * app_main_run - dlopen enabled type=so modules and call module_init only.
 * Call module_start via app_main_start_so_modules() after framework handles are open.
 */
void app_main_run(void);

/**
 * app_main_start_so_modules - Call module_start() for each loaded SO.
 * Headless/ipcamera apps should block inside module_start to keep ipc_main alive.
 */
void app_main_start_so_modules(void);

/**
 * app_main_cleanup - Release config and loaded SO handles. Optional, e.g. on exit.
 */
void app_main_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_APP_CONFIG_H */
