// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Stella LVGL entry: display init, main loop, and demo launcher.
 */
/*********************
 *      INCLUDES
 *********************/
#include "tablet_gui.h"

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "lv_port.h"

#include "tablet_font.h"
#include "tablet_home.h"

#define LOG_TAG "TABLET_GUI"
#include "log.h"

#include "hardware.h"
#include "system_thread.h"

#include "soc_platform.h"
#include "bitmap.h"

#include <pthread.h>

#include "app_globle_config.h"

#include "tablet_common.h"
#define GUI_THREAD_STACK_SIZE       (256 * 1024)
/* 两次 lv_timer_handler() 之间的休眠上限（ms）：及时响应输入/相机预览类
 * 短周期定时器，同时避免待机 1ms 忙轮询。 */
#define GUI_SLEEP_MAX_MS            5u
static volatile int s_gui_thread_running = 0;
static pthread_t s_gui_thread_id = (pthread_t)0;
static int s_gui_thread_created = 0;
static int s_lv_opened = 0;

/* GUI 让出钩子：全屏接管型应用（emu 游戏帧循环）在点开始后设置、退出后
 * 清除。钩子置位期间本循环不跑 lv_timer_handler——桌面渲染/定时器/来电
 * 感知随之冻结，应用独占 fb/IPU/AO；钩子返回后 LVGL 原样恢复（应用侧
 * 负责一次全量 invalidate）。设置与清除均发生在 LVGL 回调（GUI 线程），
 * 与本循环同线程，无需加锁。 */
static int (*s_gui_yield_hook)(void) = NULL;

void app_gui_set_yield_hook(int (*hook)(void))
{
    s_gui_yield_hook = hook;
}


/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  EXTERN PROTOTYPES
 **********************/
/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/


/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void *gui_thread(void *argv)
{
    (void)argv;
    (void)thread_set_current_name("GuiThread");
    /* LVGL init */
    lv_init();

    lv_port_init();

    /* Initialize font */
    tablet_font_init();

    // main of gui
    tablet_home();

    while (s_gui_thread_running)
    {
        if (s_gui_yield_hook) {
            /* 让出控制权：阻塞跑接管应用的帧循环，返回后恢复 LVGL 调度 */
            (void)s_gui_yield_hook();
            continue;
        }
        /* LVGL 9.3 lv_timer_handler() 返回距下一个到期定时器的毫秒数
         * （无定时器时为 0xFFFFFFFF）。按返回值休眠（上限 5ms）代替固定
         * usleep(1000)：待机无重绘时 GuiThread 从 ~13% 单核占用降到 <2%，
         * 与 wearable_gui.c 同款写法。 */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > GUI_SLEEP_MAX_MS)
            sleep_ms = GUI_SLEEP_MAX_MS;
        if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
    }

#ifdef CONFIG_APP_TABLET_LV_DEMO_STORAGE
    msgbox_hold_restart_frame_if_needed();
#endif /* CONFIG_APP_TABLET_LV_DEMO_STORAGE */

    lv_port_deinit();
    lv_deinit();

    return NULL;
}


int app_lv_open(const char* cfg_path)
{
    if (s_lv_opened) {
        return 0;
    }
    if (cfg_path == NULL)
    {
        return -1;
    }

    g_app_lv_data = (struct app_lv_data *)malloc(sizeof(struct app_lv_data));
    if (g_app_lv_data == NULL)
    {
        log_error("malloc app_lv_data failed\n");
        return -1;
    }

    {
        size_t cfg_len = sizeof(g_app_lv_data->cfg_path);
        strncpy(g_app_lv_data->cfg_path, cfg_path, cfg_len - 1);
        g_app_lv_data->cfg_path[cfg_len - 1] = '\0';
    }
    app_lv_read_config();

    bitmap_init();
#if 1
    int core = 1;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
#endif
    /* Create gui thread */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0)
    {
        pthread_attr_setstacksize(&attr, GUI_THREAD_STACK_SIZE);
    }

    pthread_t GuiThreadId;
    s_gui_thread_running = 1;
    int err = pthread_create(&GuiThreadId, &attr, gui_thread, NULL);
    if (err < 0)
    {
        log_error("failed to create gui thread\n");
        s_gui_thread_running = 0;
        free(g_app_lv_data);
        g_app_lv_data = NULL;
        return -1;
    }
    s_gui_thread_id = GuiThreadId;
    s_gui_thread_created = 1;
    s_lv_opened = 1;

    /* Set thread CPU affinity */
    pthread_setaffinity_np(GuiThreadId, sizeof(cpu_set_t), &cpuset);

    return 0;
}

void app_lv_stop_gui_loop(void)
{
    s_gui_thread_running = 0;
}

int app_lv_close( void )
{
    if (!s_lv_opened) {
        return 0;
    }

    s_gui_thread_running = 0;
    if (s_gui_thread_created) {
        (void)pthread_join(s_gui_thread_id, NULL);
        s_gui_thread_created = 0;
        s_gui_thread_id = (pthread_t)0;
    }

    (void)bitmap_release();
    if (g_app_lv_data) {
        free(g_app_lv_data);
        g_app_lv_data = NULL;
    }
    s_lv_opened = 0;
    return 0;
}
