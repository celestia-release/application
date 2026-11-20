// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/common.
 *
 * LVGL demo common resources and helpers implementation.
 */
#define LOG_TAG "GUI_COMMON"

/*********************
 *      INCLUDES
 *********************/
#include "gui_common.h"

#include "gui_font.h"

#include "multimedia.h"
#include "system_config.h"
#include "log.h"
#include "iniparser.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*********************
 *      DEFINES
 *********************/
#define ICON_SSL_NAME       "icon/ssl.png"
#define ICON_SSL_FALLBACK   "icon/music.png"
#define APP_SETTINGS_CONFIG_PATH "/etc/config/settings.json"
#define APP_RUNTIME_CONFIG_PATH  "/etc/config/app_runtime.json" /* legacy */
#define APP_USER_CONFIG_PATH     "/etc/config/app_user.json"    /* older legacy */

static FILE *app_settings_fopen_read(void)
{
    static const char *const paths[] = {
        APP_SETTINGS_CONFIG_PATH,
        APP_RUNTIME_CONFIG_PATH,
        APP_USER_CONFIG_PATH,
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *fp = fopen(paths[i], "rb");
        if (fp)
            return fp;
    }
    return NULL;
}


/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/


/**********************
 *  STATIC VARIABLES
 **********************/

static lv_obj_t *s_toast_obj = NULL;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

static void gui_camera_fill_ch(multimedia_t *mm, int idx, int *w, int *h)
{
    *w = 0;
    *h = 0;
    if (idx < 0 || !mm || !mm->camera || !mm->camera->stream_get_channel_size)
        return;
    if (mm->camera->stream_get_channel_size(idx, w, h) != 0) {
        *w = 0;
        *h = 0;
    }
}

int gui_camera_streams_resolve(void *multimedia, gui_camera_streams_t *out)
{
    multimedia_t *mm = (multimedia_t *)multimedia;
    if (!mm || !mm->camera || !out)
        return -1;
    if (!mm->camera->stream_index_by_role) {
        log_error("gui_camera_streams: stream_index_by_role missing");
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->idx_main = -1;
    out->idx_preview = -1;
    out->idx_ai = -1;
    out->idx_qr = -1;

    if (mm->camera->stream_index_by_role("main", &out->idx_main) != 0)
        out->idx_main = -1;
    if (mm->camera->stream_index_by_role("preview", &out->idx_preview) != 0)
        out->idx_preview = -1;
    if (mm->camera->stream_index_by_role("ai", &out->idx_ai) != 0)
        out->idx_ai = -1;
    if (mm->camera->stream_index_by_role("qr", &out->idx_qr) != 0)
        out->idx_qr = -1;

    gui_camera_fill_ch(mm, out->idx_main, &out->main_w, &out->main_h);
    gui_camera_fill_ch(mm, out->idx_preview, &out->preview_w, &out->preview_h);
    gui_camera_fill_ch(mm, out->idx_ai, &out->ai_w, &out->ai_h);

    if (out->idx_main >= 0 || out->idx_preview >= 0 || out->idx_ai >= 0 || out->idx_qr >= 0) {
        out->valid = 1;
    } else {
        log_warn("gui_camera_streams_resolve: no stream matched roles");
        return -1;
    }

    return 0;
}


struct lan_info_t
{
    char *name;
    char *text;
};

struct app_lv_data *g_app_lv_data = NULL;

static struct lan_info_t g_chinese[] =
{
    {"Video Call", "视频电话"},
    {"IPC Camera", "带屏摄像机"},
    {"Camera", "相机"},
    {"Music", "音乐"},
    {"Video", "视频"},
    {"Clock", "时钟"},
    {"File Manager", "文件管理"},
    {"Settings", "设置"},
    {"System Information", "系统信息"},
    {"Volc Engine", "火山引擎"},
    {"Audio Algo", "音频算法"},
    {"Qwen", "通义千问"},

    {"select object", "选择对象类型"},
    {"select all", "全选"},
    {"cancel select all", "取消全选"},

    {"body", "人形"},
    {"face", "人脸"},
    {"pet", "宠物"},
    {"vehicle", "机动车"},
    {"non_motor_vehicle", "非机动车"},
    {"hand", "手"},
    {"package", "包裹"},
    {"cry", "哭声"},
    {"upper", "头肩"},

    {"AI", "智能算法"},
    {"AI options", "算法选项"},
    {"Backlight", "背光"},
    {"WLAN", "无线网络"},
    {"language", "语言"},
    {"config ok", "配置完成"},
    {"ok", "确定"},
    {"note", "提示"},

    {"Sunday", "星期日"},
    {"Monday", "星期一"},
    {"Tuesday", "星期二"},
    {"Wednesday", "星期三"},
    {"Thursday", "星期四"},
    {"Friday", "星期五"},
    {"Saturday", "星期六"},

    {"reboot now", "立即重启"},
    {"reboot later", "稍后重启"},
    {"cancel", "取消"},
    {"Changes will take effect after reboot", "修改需要重启后生效"},

    {"mode already reach max", "所选对象的模型数已达到最大值"},
    {"mode Conflict", "模型冲突"},
    {"do not connect to server", "还没连接服务器"},
    {"SoundSource", "音源定位"},
    {"QR Code Setup", "二维码配网"},
    {"connected", "已连接"},
    {"connect failed", "连接失败"},
    {"refresh", "刷新"},
    {"encryption type", "加密类型"},
    {"signal strength", "信号强度"},
    {"No networks found", "无可用网络"},
};

static cJSON *app_user_load_root(void)
{
    FILE *fp;
    long size;
    size_t n;
    char *buf = NULL;
    cJSON *root = NULL;

    fp = app_settings_fopen_read();
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size <= 0 || size > (1024 * 1024)) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static int app_user_save_root(cJSON *root)
{
    char *printed;
    FILE *fp;

    if (!root)
        return -1;
    printed = cJSON_Print(root);
    if (!printed)
        return -1;
    fp = fopen(APP_SETTINGS_CONFIG_PATH, "wb");
    if (!fp) {
        free(printed);
        return -1;
    }
    fputs(printed, fp);
    fclose(fp);
    free(printed);
    return 0;
}

int app_lv_write_config()
{
    cJSON *root;
    cJSON *language;

    log_info("in: \n");

    if (g_app_lv_data == NULL)
        return -1;

    /* Prefer RMW /etc/config/settings.json language.id (preserve wifi/apps/program). */
    root = app_user_load_root();
    if (!root)
        root = cJSON_CreateObject();
    if (!root) {
        log_error("create app_user root failed");
        return -1;
    }

    language = cJSON_GetObjectItem(root, "language");
    if (!cJSON_IsObject(language)) {
        cJSON_DeleteItemFromObject(root, "language");
        language = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "language", language);
    }
    cJSON_DeleteItemFromObject(language, "id");
    cJSON_AddNumberToObject(language, "id", g_app_lv_data->language_index);

    if (app_user_save_root(root) != 0) {
        cJSON_Delete(root);
        log_error("cannot write %s", APP_SETTINGS_CONFIG_PATH);
        return -1;
    }
    cJSON_Delete(root);

    log_info("out: ok \n");
    return 0;
}

int app_lv_read_config()
{
    cJSON *root;
    cJSON *language;
    cJSON *id;
    int found = 0;

    log_info("in: \n");

    if (g_app_lv_data == NULL)
        return -1;

    /* Primary: settings.json language.id */
    root = app_user_load_root();
    if (root) {
        language = cJSON_GetObjectItem(root, "language");
        if (cJSON_IsObject(language)) {
            id = cJSON_GetObjectItem(language, "id");
            if (cJSON_IsNumber(id)) {
                g_app_lv_data->language_index = id->valueint;
                found = 1;
            }
        }
        cJSON_Delete(root);
    }

    /* Fallback: legacy app_config.ini [language] for old images */
    if (!found && g_app_lv_data->cfg_path) {
        dictionary *handle = iniparser_load(g_app_lv_data->cfg_path);
        if (handle) {
            g_app_lv_data->language_index = iniparser_getint(handle, "language:id", 0);
            iniparser_freedict(handle);
            found = 1;
        }
    }

    if (!found) {
        log_warn("language config missing; default language_index=0");
        g_app_lv_data->language_index = 0;
    }

    log_info("out: ok \n");
    return 0;
}

int get_lan_index()
{
    if (g_app_lv_data == NULL)
        return 0;
    return g_app_lv_data->language_index;
}

int set_lan_index(int lan_index)
{
    log_info( "in: \n");

    if(lan_index >= 0 && lan_index < 2)
    {
        g_app_lv_data->language_index = lan_index;
        app_lv_write_config();
        log_warn( "set lan to %d: \n", lan_index);
    }   

    log_info( "out: \n");
    return 0;
}

char* lan(char* name)
{
    if(g_app_lv_data == NULL || g_app_lv_data->language_index == 0)
        return name;

    for(int i=0; i<(int)(sizeof(g_chinese)/sizeof(g_chinese[0])); i++)
    {
        if( strncmp(g_chinese[i].name, name,strlen(g_chinese[i].name)) == 0)
        {
            return g_chinese[i].text;
        }
    }

    return name;
}

/**
* @brief   set image source
* @param   [IN]obj: image object
* @param   [IN]name: name of image
* @return  lv_res_t
* @retval  see definition of lv_res_t.
*/
lv_res_t gui_image_set_src(lv_obj_t *obj, const char *name)
{
    if ((NULL == obj) || (NULL == name))
    {
        LV_LOG_ERROR("parameter error.\n");
        return LV_RESULT_INVALID;
    }

    const char *use_name = name;
    /* Fallback for ???? (SSL) icon when ssl.png is missing on device */
    if (strcmp(name, ICON_SSL_NAME) == 0)
    {
        char fs_path[256];
        snprintf(fs_path, sizeof(fs_path), "%s/%s", RES_IMAGE_PATH, name);
        if (access(fs_path, F_OK) != 0)
        {
            use_name = ICON_SSL_FALLBACK;
            log_warn("ssl.png not found at %s, use %s for SoundSource icon", fs_path, ICON_SSL_FALLBACK);
        }
    }

    char path[256] = {0};
    size_t len = strlen(use_name);
    if ((len >= 3 && strcmp(use_name + len - 3, "jpg") == 0) ||
        (len >= 4 && strcmp(use_name + len - 4, "jpeg") == 0) ||
        (len >= 3 && strcmp(use_name + len - 3, "png") == 0) ||
        (len >= 3 && strcmp(use_name + len - 3, "qoi") == 0))
    {
        /* Verify the file exists before handing it to LVGL; a missing file
         * otherwise floods the log with image_decoder_get_info open failures. */
        char fs_path[256];
        snprintf(fs_path, sizeof(fs_path), "%s/%s", RES_IMAGE_PATH, use_name);
        if (access(fs_path, R_OK) != 0)
        {
            log_warn("image not found: %s (skip set_src)", fs_path);
            return LV_RESULT_INVALID;
        }
        snprintf(path, sizeof(path), "%c:%s/%s", LV_FS_STDIO_LETTER, RES_IMAGE_PATH, use_name);
    }
    else
    {
        LV_LOG_ERROR("unsupported image\n");
        return LV_RESULT_INVALID;
    }

    lv_image_set_src(obj, path);

    return LV_RESULT_OK;
}

lv_obj_t *gui_create_top(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop)
{
    lv_obj_t *bar = lv_obj_create(parent);
    if (NULL == bar)
    {
        LV_LOG_ERROR("failed to create bar\n");
        return NULL;
    }

    lv_obj_remove_style_all(bar);

    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, LV_PCT(100), LV_PCT(10));
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x060606), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_0, 0);

    lv_obj_t *btn = lv_button_create(bar);
    if (NULL == btn)
    {
        LV_LOG_ERROR("failed to create botton\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(16), LV_PCT(100));
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, LV_PCT(5), 0);

    lv_obj_t *label_btn = lv_label_create(btn);
    lv_obj_align(label_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_back_event_handle, LV_EVENT_CLICKED, desktop);

    lv_font_t *font = gui_font_get_montserrat_font(40);
    if (NULL == font)
    {
        LV_LOG_ERROR("failed to get montserrat font\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_set_style_text_font(label_btn, font, 0);
    lv_obj_set_style_text_color(label_btn, lv_color_hex(0xffffff), 0);
    lv_label_set_text(label_btn, LV_SYMBOL_CLOSE);

    font = gui_font_get_freetype_font(40);
    if (NULL == font)
    {
        LV_LOG_ERROR("failed to get freetype font\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title, label);

    return bar;
}
lv_obj_t *gui_create_top_2(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop)
{
    lv_obj_t *bar = lv_obj_create(parent);
    if (NULL == bar)
    {
        LV_LOG_ERROR("failed to create bar\n");
        return NULL;
    }

    lv_obj_remove_style_all(bar);

    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, LV_PCT(100), LV_PCT(10));
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x060606), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_0, 0);

    lv_obj_t *btn = lv_button_create(bar);
    if (NULL == btn)
    {
        LV_LOG_ERROR("failed to create botton\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(16), LV_PCT(100));
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, LV_PCT(5), 0);

    lv_obj_t *label_btn = lv_label_create(btn);
    lv_obj_align(label_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_back_event_handle, LV_EVENT_CLICKED, desktop);

    lv_font_t *font = gui_font_get_montserrat_font(40);
    if (NULL == font)
    {
        LV_LOG_ERROR("failed to get montserrat font\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_set_style_text_font(label_btn, font, 0);
    lv_obj_set_style_text_color(label_btn, lv_color_hex(0xffffff), 0);
    lv_label_set_text(label_btn, LV_SYMBOL_CLOSE);

    font = gui_font_get_freetype_font(40);
    if (NULL == font)
    {
        LV_LOG_ERROR("failed to get freetype font\n");
        lv_obj_delete(bar);
        return NULL;
    }

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title, label);

    return bar;
}

lv_obj_t *gui_create_obj(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    return obj;
}

static void toast_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_toast_obj) {
        lv_obj_delete(s_toast_obj);
        s_toast_obj = NULL;
    }
}

void show_msgbox_note(const char *str)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *label;
    lv_font_t *font;
    lv_timer_t *t;

    if (!str || str[0] == '\0')
        return;

    if (s_toast_obj) {
        lv_obj_delete(s_toast_obj);
        s_toast_obj = NULL;
    }

    disp = lv_display_get_default();
    screen = disp ? lv_display_get_screen_active(disp) : NULL;
    if (!screen) {
        log_warn("show_msgbox_note: no active screen");
        return;
    }

    s_toast_obj = lv_obj_create(screen);
    lv_obj_remove_style_all(s_toast_obj);
    lv_obj_set_size(s_toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast_obj, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_toast_obj, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_toast_obj, 8, 0);
    lv_obj_set_style_pad_all(s_toast_obj, 10, 0);
    lv_obj_align(s_toast_obj, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(s_toast_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_toast_obj, LV_OBJ_FLAG_CLICKABLE);

    label = lv_label_create(s_toast_obj);
    font = gui_font_get_freetype_font(20);
    if (font)
        lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_label_set_text(label, str);

    t = lv_timer_create(toast_timer_cb, 1000, NULL);
    if (t)
        lv_timer_set_repeat_count(t, 1);
}

/* Extended data for message box */
typedef struct {
    lv_obj_t * footer;
    footer_btn_cb_t btn_cb;
} msgbox_ext_t;

/* Button click callback */
static void footer_btn_event_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t *msgbox = lv_obj_get_parent(lv_obj_get_parent(btn));  /* btn -> footer -> msgbox */
    msgbox_ext_t * ext = lv_obj_get_user_data(msgbox);
    
    /* Get button ID from its position in footer */
    uint32_t btn_id = lv_obj_get_index(btn);
    
    if (ext->btn_cb) {
        ext->btn_cb(msgbox, btn_id);
    }
}

/* Message box delete callback (cleanup) */
static void msgbox_delete_event_cb(lv_event_t * e) {
    lv_obj_t * msgbox = lv_event_get_target(e);
    msgbox_ext_t * ext = lv_obj_get_user_data(msgbox);
    
    /* Free extended data */
    if (ext) {
        lv_free(ext);
        lv_obj_set_user_data(msgbox, NULL);
    }
}

lv_obj_t * msgbox_create_with_footer(
    lv_obj_t * parent,
    const char * title,
    const char * content,
    const char * btn_texts[],
    footer_btn_cb_t btn_cb, int has_close_btn) 
{
    /* Create base message box */
    lv_obj_t * msgbox = lv_msgbox_create(parent);
    lv_obj_t * obj_title = lv_msgbox_add_title(msgbox, title);
    lv_obj_set_style_text_font(obj_title, gui_font_get_freetype_font(20), 0);
    lv_obj_t * obj_content = lv_msgbox_add_text(msgbox, content);
    lv_obj_set_style_text_font(obj_content, gui_font_get_freetype_font(20), 0);
    lv_obj_set_size(msgbox, 400,150);
    if(has_close_btn) lv_msgbox_add_close_button(msgbox);
    
    /* Allocate extended data */
    msgbox_ext_t * ext = lv_malloc(sizeof(msgbox_ext_t));
    //lv_memset_00(ext, sizeof(msgbox_ext_t));
    memset(ext, 0, sizeof(msgbox_ext_t));
    lv_obj_set_user_data(msgbox, ext);
    ext->btn_cb = btn_cb;
    
    /* Create footer container */
    ext->footer = lv_obj_create(msgbox);
    lv_obj_set_size(ext->footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ext->footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ext->footer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ext->footer, 10, 0);
    lv_obj_set_style_border_width(ext->footer, 1, 0);
    lv_obj_set_style_border_color(ext->footer, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_border_side(ext->footer, LV_BORDER_SIDE_TOP, 0);
    
    /* Add buttons */
    for (uint32_t i = 0; btn_texts[i] != NULL; i++) {
        lv_obj_t * btn = lv_button_create(ext->footer);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, 40);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_min_width(btn, 80, 0);
        
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, btn_texts[i]);
        lv_obj_set_style_text_font(label, gui_font_get_freetype_font(20), 0);
        lv_obj_center(label);
        
        /* Add click event */
        lv_obj_add_event_cb(btn, footer_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }
    
    /* Add delete callback for cleanup */
    lv_obj_add_event_cb(msgbox, msgbox_delete_event_cb, LV_EVENT_DELETE, NULL);
    
    /* Update layout */
    lv_obj_update_layout(msgbox);
    
    return msgbox;
}
