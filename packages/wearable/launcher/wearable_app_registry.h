// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna installed-apps registry (loaded from installed_apps.json).
 */
#ifndef APPLICATION_WEARABLE_APP_REGISTRY_H
#define APPLICATION_WEARABLE_APP_REGISTRY_H

#include <stddef.h>

#include "gui_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEARABLE_APP_MAX_DYNAMIC 32

typedef struct {
    int index;
    char app_id[64];
    char display_name[64];
    char icon_path[256];
    char install_path[256];
    char entry_so[256];
    char entry_symbol[64];
    char destroy_symbol[64];
    int enabled;
} wearable_app_desc_t;

/** Load installed_apps from /etc/config/settings.json (legacy: installed_apps.json). */
void wearable_app_registry_refresh(void);
/** Scan /usr/apps when installed apps list is missing (same as Nova). */
void wearable_app_registry_scan_installed(void);
int wearable_app_registry_save(void);
const wearable_app_desc_t *wearable_app_registry_list(int *count);

/* Build gui_shell_app_desc_t list (enabled apps only). Returns count written. */
unsigned int wearable_app_registry_fill_gui_shell(gui_shell_app_desc_t *out, unsigned int out_max);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_WEARABLE_APP_REGISTRY_H */
