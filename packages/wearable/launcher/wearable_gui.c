// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/wearable.
 *
 * LVGL GUI thread management: open/close lifecycle for Luna launcher.
 */
#define LOG_TAG "WEARABLE_GUI"

#include "wearable_gui.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_globle_config.h"
#include "bitmap.h"
#include "gui_font.h"
#include "hardware.h"
#include "system_thread.h"
#include "log.h"
#include "lv_port.h"
#include "lvgl.h"

extern int wearable_ui_main(void);

#define GUI_THREAD_STACK_SIZE (256 * 1024)

/* Cap the sleep between lv_timer_handler() calls. Using the return value of
 * lv_timer_handler() (time until next timer) avoids the fixed 1 ms busy-loop
 * while still waking up promptly for the camera preview timer (25 ms) and
 * allowing a quick exit when s_gui_thread_running is cleared. */
#define GUI_SLEEP_MAX_MS 5u

static volatile int s_gui_thread_running = 0;
static pthread_t s_gui_thread_id = (pthread_t)0;
static int s_gui_thread_created = 0;
static int s_lv_opened = 0;

/* GUI yield hook: while set, the loop hands control to the hook instead of
 * running LVGL. Used by fullscreen takeover apps (emu game loop); LVGL
 * timers/rendering freeze until the hook returns. Set/cleared from LVGL
 * callbacks (GUI thread), same thread as this loop — no locking needed. */
static int (*s_gui_yield_hook)(void) = NULL;

void app_gui_set_yield_hook(int (*hook)(void))
{
    s_gui_yield_hook = hook;
}

static void *gui_thread(void *argv)
{
    (void)argv;
    (void)thread_set_current_name("LunaGui");

    log_info("gui thread: bitmap_init");
    (void)bitmap_init();

    log_info("gui thread: lv_init/port");
    lv_init();
    lv_port_init();
    (void)gui_font_init();
    log_info("gui thread: wearable_ui_main");
    wearable_ui_main();
    lv_refr_now(lv_display_get_default());
    log_info("gui thread: enter lv_timer loop");

    while (s_gui_thread_running) {
        if (s_gui_yield_hook) {
            /* Hand control to the takeover app; resumes when it returns. */
            (void)s_gui_yield_hook();
            continue;
        }
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > GUI_SLEEP_MAX_MS)
            sleep_ms = GUI_SLEEP_MAX_MS;
        if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
    }

    lv_port_deinit();
    lv_deinit();
    return NULL;
}

int app_lv_open(const char *cfg_path)
{
    (void)cfg_path;
    if (s_lv_opened)
        return 0;

    /* Do not call bitmap_init()/hw_get_module on the main thread: after
     * notification workers are running, loader locks can stall the boot path
     * with no further logs. GPU/bitmap init runs in the GUI thread instead. */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0)
        pthread_attr_setstacksize(&attr, GUI_THREAD_STACK_SIZE);

    s_gui_thread_running = 1;
    log_info("app_lv_open: creating LunaGui thread");
    if (pthread_create(&s_gui_thread_id, &attr, gui_thread, NULL) != 0) {
        s_gui_thread_running = 0;
        log_error("app_lv_open: pthread_create failed");
        return -1;
    }
    s_gui_thread_created = 1;
    s_lv_opened = 1;
    return 0;
}

int app_lv_close(void)
{
    if (!s_lv_opened)
        return 0;

    s_gui_thread_running = 0;
    if (s_gui_thread_created) {
        (void)pthread_join(s_gui_thread_id, NULL);
        s_gui_thread_created = 0;
        s_gui_thread_id = (pthread_t)0;
    }

    (void)bitmap_release();
    (void)gui_font_release();
    s_lv_opened = 0;
    return 0;
}
