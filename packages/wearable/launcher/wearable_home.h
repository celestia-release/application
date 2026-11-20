// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna watch-style home launcher: clock face in center, apps on left/right.
 */
#ifndef WEARABLE_HOME_H
#define WEARABLE_HOME_H

#include "gui_shell.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create Luna home launcher on the given screen.
 * @param parent Active LVGL screen or root container.
 * @param apps     App descriptors from wearable_app_registry_fill_gui_shell.
 * @param app_count Number of enabled apps (may be 0).
 * @return 0 on success, -1 on failure.
 */
int wearable_home_create(lv_obj_t *parent,
                     const gui_shell_app_desc_t *apps,
                     unsigned int app_count);

/** Gesture handler for watch launcher (swipe/tap). */
int wearable_home_on_gesture(gui_shell_gesture_t gesture);

/** Root object used for input capture (same as parent passed to create). */
lv_obj_t *wearable_home_get_root(void);

/** Close in-app UI and return to home launcher (callable from app SO via dlsym). */
__attribute__((visibility("default"))) void wearable_home_return_to_launcher(void);

/** Launch app by id (e.g. "camera"); used when WEARABLE_BOOT_APP is set for NFS debug. */
int wearable_home_launch_app_by_id(const char *app_id);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_HOME_H */
