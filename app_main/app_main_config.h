// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * JSON config and exec API (from former framework/runtime).
 * For registry and app_main_start_framework_modules, use app_main.h only.
 */
#ifndef APPLICATION_APP_MAIN_CONFIG_H
#define APPLICATION_APP_MAIN_CONFIG_H

#include <stddef.h>
#include "app_main_error.h"

#ifdef __cplusplus
extern "C" {
#endif

char *app_main_module_version(void);
int app_main_config_init(const char *path, void **config);
int app_main_config_release(void **config);
int app_main_config_get_icon(void *config, char *icon, size_t icon_size);
int app_main_config_get_name(void *config, const char *language, char *name, size_t name_size);
int app_main_config_get_program(void *config, char *program, size_t program_size);
/** Exec application (execv); does not return on success. Config is released before exec. */
int app_main_exec_app(void **config);
int app_main_save_config(void *config, const char *file);
int app_main_printf_config(void *config);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_APP_MAIN_CONFIG_H */
