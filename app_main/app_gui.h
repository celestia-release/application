// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Unified GUI launcher API (Luna / Nova share the same symbols; one variant is linked per product).
 */
#ifndef APPLICATION_APP_GUI_H
#define APPLICATION_APP_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

/** Open LVGL UI thread (cfg_path may be NULL; product-specific). */
int app_lv_open(const char *cfg_path);

/** Tear down LVGL UI thread. */
int app_lv_close(void);

/**
 * Set/clear the GUI yield hook (NULL clears). While set, the GUI loop hands
 * control to the hook instead of running lv_timer_handler(): desktop
 * rendering/timers freeze and the takeover app (fullscreen game) owns the
 * framebuffer exclusively. The hook runs on the GUI thread and blocks until
 * the takeover ends; the app must invalidate-all after clearing it.
 * Only call from LVGL callbacks (GUI thread). */
void app_gui_set_yield_hook(int (*hook)(void));

/** Run after app_main_start_framework_modules(); blocks until shutdown (does not return). */
void app_gui_run(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_APP_GUI_H */
