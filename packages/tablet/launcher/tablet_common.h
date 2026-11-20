// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo common resources and helpers.
 */
#ifndef APPLICATION_TABLET_COMMON_H
#define APPLICATION_TABLET_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include "app_globle_config.h"
#include "lv_gui_layout.h"
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

/* Relative to <app>/res then RES_IMAGE_*; matches .../res/image/800x480/clock/ on device */
#define CLOCK_BG                    "image/" GUI_IMAGE_RES_PREFIX "/clock/clock_bg.png"
#define CLOCK_H                     "image/" GUI_IMAGE_RES_PREFIX "/clock/clock_h.qoi"
#define CLOCK_M                     "image/" GUI_IMAGE_RES_PREFIX "/clock/clock_m.qoi"
#define CLOCK_S                     "image/" GUI_IMAGE_RES_PREFIX "/clock/clock_s.qoi"

/* Launcher wallpaper; packaged as install/gui/image/icon/background.qoi (flat) */
#define ICON_BACKGROUND             "icon/background.qoi"

#define COLOR_BACKGROUND            0x000000
#define COLOR_LABEL                 0x05fbe2

/* Phone-style home indicator zone at the screen bottom (created by
 * tablet_create_top). Apps with full-width bottom bars must shift them up
 * by this height so controls stay clear of the tap/swipe hit area. */
#define TABLET_HOME_BAR_H           GUI_SCALE_VER(32)

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief Resolve shared image absolute path (afht launcher res, optional resolution subdir, legacy).
 * @return 0 if an existing file was found, -1 if only a best-guess path was written.
 */
int tablet_resolve_image_path(const char *rel_name, char *out, size_t out_len);

/** Log touch/pointer position and widget bounds (debug calibration vs hit target). */
void tablet_log_pointer_event(lv_event_t *e, const char *tag);

/** Log widget absolute position and size after layout (debug launcher/clock geometry). */
void tablet_log_obj_bounds(const char *tag, lv_obj_t *obj);

/** Generic input trace callback for LV_EVENT_PRESSED/CLICKED/RELEASED on buttons. */
void tablet_input_log_cb(lv_event_t *e);

/**
 * @brief Set image widget source by path name.
 * @param obj  Image object.
 * @param name Image path or resource name.
 * @return LV_RES_OK on success.
 */
lv_res_t tablet_image_set_src(lv_obj_t *obj, const char *name);

/** Create top bar with centered title; app exit is the home bar, no back button. */
lv_obj_t *tablet_create_top(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop);
/** Kept for the ipc_camera call site; identical to tablet_create_top(). */
lv_obj_t *tablet_create_top_2(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop);
/**
 * Phone-style bottom home indicator (swipe up ~1/5 panel height -> on_back
 * CLICKED; a plain tap does nothing).
 * Apps that do not use tablet_create_top (e.g. ipc_camera standby) call this
 * directly. The bar floats on the top layer and dies with @p window.
 * @param window   App root object; its LV_EVENT_DELETE removes the bar.
 * @param on_back  Handler invoked on exit (receives CLICKED).
 * @param user_data User data for @p on_back (may be NULL).
 */
void tablet_create_home_bar(lv_obj_t *window, lv_event_cb_t on_back, void *user_data);
/** Create a simple container with given size. */
lv_obj_t *tablet_create_obj(lv_obj_t *parent, int32_t w, int32_t h);


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

/**
 * @brief Show a short note that disappears by itself and does not block touch.
 * @param duration_ms  Display time; 0 uses 1500 ms.
 * @param is_error     Non-zero shows a red status dot, else green.
 */
void show_toast_note(const char *str, uint32_t duration_ms, int is_error);

/**
 * @brief Show a non-closeable "saving" hint dialog (no close button).
 *        Call close_saving_hint() to dismiss it programmatically.
 */
void show_saving_hint(const char *str);

/** @brief Destroy the saving hint dialog created by show_saving_hint(). */
void close_saving_hint(void);

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
 * @brief Full-screen opaque overlay + top-layer restart wait dialog (no buttons).
 */
void msgbox_show_restart_waiting(const char *message);

/**
 * @brief Flush the restart-wait dialog to framebuffer (call on GuiThread after msgbox_show_restart_waiting).
 */
void msgbox_flush_restart_waiting_frame(void);

/**
 * @brief If storage-restart UI lock exists, paint restart-wait frame before LVGL teardown.
 */
void msgbox_hold_restart_frame_if_needed(void);

/**
 * @brief Turn a footer msgbox into a non-interactive restart wait state.
 * @details Adds a full-screen dim overlay, hides footer buttons, updates body text.
 */
void msgbox_enter_restart_blocking(lv_obj_t *msgbox, const char *message);

/**
 * @brief Get localized string by key.
 * @param name Key (e.g. from language table).
 * @return Localized string pointer.
 */
char *lan(char *name);

/**
 * @brief App display name for the current language.
 * @details "中文(English)" keeps the side that matches the language setting.
 *          Other names go through lan().
 */
void lan_app_name(const char *name, char *out, size_t out_len);

/**
 * @brief Get current language index.
 * @return 0 = English, 1 = Chinese, etc.
 */
int get_lan_index(void);

/**
 * @brief Mark that settings changed (language / app enable switches) so the
 *        running process should restart when the current app exits.
 * @details Writes a marker file under /tmp: the launcher and every app SO are
 *          separate compilation units, so a file is the only state they share.
 */
void tablet_app_settings_dirty_mark(void);

/**
 * @brief Consume the settings-changed marker.
 * @return true when the marker was present (caller should restart the process).
 */
bool tablet_app_settings_dirty_take(void);

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
} tablet_camera_streams_t;

/**
 * Fill stream indices using stream_index_by_role (main, preview, ai, qr) and channel sizes.
 * @param multimedia multimedia_t * from app_main (void * avoids pulling multimedia.h here).
 * @return 0 on success, -1 on error.
 */
int tablet_camera_streams_resolve(void *multimedia, tablet_camera_streams_t *out);

/** Semi-transparent focus-box UI for tap-to-AF (0.5s). */
typedef struct {
    lv_obj_t *box;
    lv_timer_t *timer;
} nova_af_focus_ui_t;

void nova_af_focus_ui_hide(nova_af_focus_ui_t *ui);

/**
 * Tap on preview → 1/8 ROI + focus box + trigger_af.
 * @param multimedia multimedia_t *
 * @param preview_obj clickable preview image/area
 * @param ui focus-box state (may be NULL to skip UI)
 * @return 0 on trigger ok, <0 on skip/fail
 */
int nova_af_handle_preview_tap(void *multimedia, lv_obj_t *preview_obj, nova_af_focus_ui_t *ui);

/**
 * Center ONCE AF (first preview frame). If triggered_flag non-NULL, only runs once.
 * @return 0 on trigger ok, <0 otherwise
 */
int nova_af_trigger_center_once(void *multimedia, int *triggered_flag);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APPLICATION_TABLET_COMMON_H */
