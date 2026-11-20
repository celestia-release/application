// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/common.
 *
 * Shared GUI common resources and helpers.
 */
#ifndef APPLICATION_GUI_COMMON_H
#define APPLICATION_GUI_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "app_globle_config.h"
#include "lvgl.h"
#include "lv_conf.h"

/*********************
 *      DEFINES
 *********************/
#if 0
/* Icon image paths (launcher app tiles use app.json "icon" via nova registry, not these) */
#define ICON_CAMERA                 "icon/camera.png"
#define ICON_MUSIC                  "icon/music.png"
#define ICON_VIDEO                  "icon/video.png"
#define ICON_SETTING                "icon/setting.png"
#define ICON_VIDEOCALL              "icon/videocall.png"
#define ICON_FILES                  "icon/files.png"
#define ICON_INFO                   "icon/info.png"
#define ICON_DOUBAO                 "icon/doubao.png"
#define ICON_QWEN                 	"icon/qwen.png"
#define ICON_SSL                    "icon/ssl.png"
#endif

/* Relative to app res prefix or gui_shell_resolve_asset_path() tier under RES_IMAGE_PATH */
#define CLOCK_BG                    "clock/clock_bg.png"
#define CLOCK_H                     "clock/clock_h.qoi"
#define CLOCK_M                     "clock/clock_m.qoi"
#define CLOCK_S                     "clock/clock_s.qoi"

#define COLOR_BACKGROUND            0x000000
#define COLOR_LABEL                 0x05fbe2

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief Set image widget source by path name.
 * @param obj  Image object.
 * @param name Image path or resource name.
 * @return LV_RES_OK on success.
 */
lv_res_t gui_image_set_src(lv_obj_t *obj, const char *name);

/** Create top bar with title and back button (single desktop target). */
lv_obj_t *gui_create_top(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop);
/** Create top bar with title and back button (dual desktop). */
lv_obj_t *gui_create_top_2(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop);
/** Create a simple container with given size. */
lv_obj_t *gui_create_obj(lv_obj_t *parent, int32_t w, int32_t h);


struct app_lv_data {
    char cfg_path[128];
    int language_index;  /**< 0 = English, 1 = Chinese */
};
extern struct app_lv_data *g_app_lv_data;

/** Write app LV config to file. Return 0 on success. */
int app_lv_write_config(void);
/** Read app LV config from file. Return 0 on success. */
int app_lv_read_config(void);

/**
 * @brief Show a note message box.
 * @param str Message string to display.
 */
void show_msgbox_note(const char *str);

/** Callback type for footer button click. */
typedef void (*footer_btn_cb_t)(lv_obj_t *msgbox, uint32_t btn_id);

/**
 * @brief Create message box with footer buttons.
 * @param parent     Parent LVGL object.
 * @param title      Title text.
 * @param content    Body text.
 * @param btn_texts  Button label array (NULL-terminated).
 * @param btn_cb     Callback on button click.
 * @param has_close_btn  Whether to show close button.
 * @return Message box object, or NULL on failure.
 */
lv_obj_t *msgbox_create_with_footer(
    lv_obj_t * parent,
    const char * title,
    const char * content,
    const char * btn_texts[],
    footer_btn_cb_t btn_cb, int has_close_btn);

/**
 * @brief Get localized string by key.
 * @param name Key (e.g. from language table).
 * @return Localized string pointer.
 */
char *lan(char *name);

/**
 * @brief Get current language index.
 * @return 0 = English, 1 = Chinese, etc.
 */
int get_lan_index(void);

/**
 * @brief Set current language index.
 * @param lan_index 0 = English, 1 = Chinese, etc.
 * @return 0 on success.
 */
int set_lan_index(int lan_index);

/** Stream indices and sizes from video_cfg.json (via multimedia camera API, lazy JSON load). */
typedef struct {
    int idx_main;
    int idx_preview;
    int idx_ai;
    int idx_qr;
    int main_w;
    int main_h;
    int preview_w;
    int preview_h;
    int ai_w;
    int ai_h;
    int valid;
} gui_camera_streams_t;

/**
 * Fill stream indices using stream_index_by_role (main, preview, ai, qr) and channel sizes.
 * @param multimedia multimedia_t * from app_main (void * avoids pulling multimedia.h here).
 * @return 0 on success, -1 on error.
 */
int gui_camera_streams_resolve(void *multimedia, gui_camera_streams_t *out);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APPLICATION_GUI_COMMON_H */
