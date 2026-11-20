// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * App main: unified register, config-driven, shared handles. Registers framework handles
 * (notification, network, multimedia, nebula, runtime) at startup; drives startup mode and
 * modules (e.g. nova) from settings.json. Nova, UVC, cloud, etc. get handles via getters
 * here; callers must include the corresponding framework header and cast the returned void*.
 */
#ifndef APPLICATION_APP_MAIN_H
#define APPLICATION_APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------- Handle registry (register from app_main, get from application modules) -------- */
void app_main_register_notification(void *n);
void app_main_register_network(void *n);
void app_main_register_multimedia(void *m);
void app_main_register_nebula(void *n);
void app_main_register_runtime(void *r);

void *app_main_get_notification(void);
void *app_main_get_network(void);
void *app_main_get_multimedia(void);
void *app_main_get_nebula(void);
void *app_main_get_runtime(void);

/**
 * Unified platform startup: soc_platform, log, thread, hw_module, notification (open+input init).
 * Call once at process start (app_main or standalone nova main).
 * @return 0 on success, negative on failure
 */
int app_main_platform_startup(void);

/**
 * Open multimedia and register for app_main_get_multimedia().
 * On tablet/Nova this is deferred until after Home first frame (never concurrent with LVGL init).
 * @return 0 on success, negative on failure
 */
int app_main_open_multimedia_handle(void);

/**
 * Open network and register for app_main_get_network().
 * Prefer calling after Home first frame (/tmp/ipc_gui_ready) to keep boot critical path short.
 * @return 0 on success, negative on failure
 */
int app_main_open_network_handle(void);

/**
 * Open framework handles used before UI.
 * Tablet/Nova: no-op here (multimedia/network opened post-Home).
 * Other products: opens multimedia + network.
 * Call after app_main_platform_startup(); app_main_start_framework_modules() calls this internally.
 * @return 0 on success, negative on failure
 */
int app_main_open_framework_handles(void);

/**
 * Start framework modules (multimedia, network) and optional UI.
 * Call after app_main_run() (SO load). Opens modules, registers handles, runs UI script if not headless.
 * @param headless 0 = UI mode (require /dev/fb0, then app_lv_open), 1 = headless
 * @param gui_config_path Path for app_config.ini, or NULL for default
 * @return 0 on success, negative on failure
 */
int app_main_start_framework_modules(int headless, const char *gui_config_path);

/* -------- Graceful shutdown flag (set by signal handler, polled by long-running loops) -------- */
void app_main_request_shutdown(void);
int app_main_shutdown_requested(void);

/** Release UI (GPU/IPU), multimedia, etc. Call from main thread after shutdown flag is set. */
void app_main_graceful_teardown(void);

/**
 * Scheme B: dlopen libfreetype.so + libpng.so after Home (not on DT_NEEDED).
 * Call before tablet_font_ensure_full / PNG decode.
 * @return 0 on success, negative if either so failed to load
 */
int app_main_load_deferred_gui_libs(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_APP_MAIN_H */
