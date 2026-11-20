// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * LVGL demo common resources and helpers implementation.
 */
#define LOG_TAG "TABLET_COMMON"

/*********************
 *      INCLUDES
 *********************/
#include "tablet_common.h"

#include "tablet_font.h"

#include "multimedia.h"
#include "system_config.h"
#include "log.h"
#include "cJSON.h"
#include "iniparser.h"

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

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


/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

static void tablet_camera_fill_ch(multimedia_t *mm, int idx, int *w, int *h)
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

int tablet_camera_streams_resolve(void *multimedia, tablet_camera_streams_t *out)
{
    multimedia_t *mm = (multimedia_t *)multimedia;
    if (!mm || !mm->camera || !out)
        return -1;
    if (!mm->camera->stream_index_by_role) {
        log_error("tablet_camera_streams: stream_index_by_role missing");
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

    tablet_camera_fill_ch(mm, out->idx_main, &out->main_w, &out->main_h);
    tablet_camera_fill_ch(mm, out->idx_preview, &out->preview_w, &out->preview_h);
    tablet_camera_fill_ch(mm, out->idx_ai, &out->ai_w, &out->ai_h);

    if (out->idx_main >= 0 || out->idx_preview >= 0 || out->idx_ai >= 0 || out->idx_qr >= 0) {
        out->valid = 1;
    } else {
        log_warn("tablet_camera_streams_resolve: no stream matched roles");
        return -1;
    }

    return 0;
}

#define NOVA_AF_ROI_FRAC       (1.0f / 8.0f)
#define NOVA_AF_FOCUS_BOX_MS   500

static void nova_af_focus_ui_timer_cb(lv_timer_t *timer)
{
    nova_af_focus_ui_t *ui = (nova_af_focus_ui_t *)lv_timer_get_user_data(timer);

    if (ui) {
        ui->timer = NULL;
        if (ui->box && lv_obj_is_valid(ui->box)) {
            lv_obj_delete(ui->box);
        }
        ui->box = NULL;
    }
}

void nova_af_focus_ui_hide(nova_af_focus_ui_t *ui)
{
    if (!ui) {
        return;
    }
    if (ui->timer) {
        lv_timer_delete(ui->timer);
        ui->timer = NULL;
    }
    if (ui->box && lv_obj_is_valid(ui->box)) {
        lv_obj_delete(ui->box);
    }
    ui->box = NULL;
}

static void nova_af_focus_ui_show(nova_af_focus_ui_t *ui, lv_obj_t *parent,
                                  lv_coord_t cx, lv_coord_t cy,
                                  lv_coord_t box_w, lv_coord_t box_h)
{
    lv_coord_t x;
    lv_coord_t y;

    if (!ui || !parent) {
        return;
    }
    nova_af_focus_ui_hide(ui);

    if (box_w < 24) {
        box_w = 24;
    }
    if (box_h < 24) {
        box_h = 24;
    }
    x = cx - box_w / 2;
    y = cy - box_h / 2;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    ui->box = lv_obj_create(parent);
    if (!ui->box) {
        return;
    }
    lv_obj_remove_style_all(ui->box);
    lv_obj_set_size(ui->box, box_w, box_h);
    lv_obj_set_pos(ui->box, x, y);
    lv_obj_set_style_bg_color(ui->box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(ui->box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ui->box, 2, 0);
    lv_obj_set_style_border_color(ui->box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(ui->box, LV_OPA_80, 0);
    lv_obj_set_style_radius(ui->box, 4, 0);
    lv_obj_clear_flag(ui->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(ui->box);

    ui->timer = lv_timer_create(nova_af_focus_ui_timer_cb, NOVA_AF_FOCUS_BOX_MS, ui);
    if (ui->timer) {
        lv_timer_set_repeat_count(ui->timer, 1);
    }
}

int nova_af_handle_preview_tap(void *multimedia, lv_obj_t *preview_obj, nova_af_focus_ui_t *ui)
{
    multimedia_t *mm = (multimedia_t *)multimedia;
    lv_point_t pt;
    lv_area_t coords;
    lv_coord_t w;
    lv_coord_t h;
    lv_coord_t lx;
    lv_coord_t ly;
    float nx;
    float ny;
    media_camera_af_roi_t roi;

    if (!mm || !mm->camera || !mm->camera->trigger_af || !preview_obj ||
        !lv_obj_is_valid(preview_obj)) {
        return -1;
    }
    if (mm->camera->af_is_supported && !mm->camera->af_is_supported(0)) {
        return -1;
    }

    lv_indev_get_point(lv_indev_active(), &pt);
    lv_obj_get_coords(preview_obj, &coords);
    w = coords.x2 - coords.x1 + 1;
    h = coords.y2 - coords.y1 + 1;
    if (w <= 0 || h <= 0) {
        return -1;
    }
    lx = pt.x - coords.x1;
    ly = pt.y - coords.y1;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) {
        return -1;
    }

    nx = (float)lx / (float)w;
    ny = (float)ly / (float)h;
    roi.nw = NOVA_AF_ROI_FRAC;
    roi.nh = NOVA_AF_ROI_FRAC;
    roi.nx = nx - roi.nw * 0.5f;
    roi.ny = ny - roi.nh * 0.5f;
    if (roi.nx < 0.0f) {
        roi.nx = 0.0f;
    }
    if (roi.ny < 0.0f) {
        roi.ny = 0.0f;
    }
    if (roi.nx + roi.nw > 1.0f) {
        roi.nx = 1.0f - roi.nw;
    }
    if (roi.ny + roi.nh > 1.0f) {
        roi.ny = 1.0f - roi.nh;
    }

    if (ui) {
        nova_af_focus_ui_show(ui, preview_obj, lx, ly,
                              (lv_coord_t)(w / 8), (lv_coord_t)(h / 8));
    }

    if (mm->camera->trigger_af(0, &roi) != 0) {
        log_warn("nova_af: tap trigger failed");
        return -1;
    }
    log_info("nova_af: tap roi=(%.3f,%.3f,%.3f,%.3f)",
             (double)roi.nx, (double)roi.ny, (double)roi.nw, (double)roi.nh);
    return 0;
}

int nova_af_trigger_center_once(void *multimedia, int *triggered_flag)
{
    multimedia_t *mm = (multimedia_t *)multimedia;

    if (triggered_flag && *triggered_flag) {
        return -1;
    }
    if (!mm || !mm->camera || !mm->camera->trigger_af) {
        return -1;
    }
    if (mm->camera->af_is_supported && !mm->camera->af_is_supported(0)) {
        return -1;
    }
    if (mm->camera->trigger_af(0, NULL) != 0) {
        return -1;
    }
    if (triggered_flag) {
        *triggered_flag = 1;
    }
    log_info("nova_af: trigger center once");
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
    /* ipc_camera screencam UI (keep before any entry sharing these prefixes) */
    {"Video call", "视频通话"},
    {"Video preview", "视频预览"},
    {"Local preview", "本机预览"},
    {"Hang up", "挂断"},
    {"Answer", "接听"},
    {"Incoming call", "来电"},
    {"Stop", "停止"},
    {"In call", "通话中"},
    {"Waiting for peer", "等待对端"},
    {"Not connected", "未联网"},
    {"Video Call", "视频电话"},
    {"IPC Camera", "带屏摄像机"},
    {"Camera", "相机"},
    {"Gallery", "相册"},
    {"Photo saved", "拍照成功"},
    {"Photos", "照片"},
    {"Photo", "拍照"},
    {"Recording", "开始录像"},
    {"Record", "录像"},
    {"Videos", "视频"},
    {"Capture failed", "拍照失败"},
    {"Video saved", "录像已保存"},
    {"Saving...", "正在保存中..."},
    {"Failed to start recording", "开始录像失败"},
    {"Stop recording first", "请先停止录像"},
    {"Cannot save", "无法保存"},
    {"No files", "暂无文件"},
    {"Delete failed", "删除失败"},
    {"Deleted", "已删除"},
    {"Delete", "删除"},
    {"Back", "返回"},
    {"Play", "播放"},
    {"Play failed", "播放失败"},
    {"Last one", "已是最后一个"},
    {"Tap Play to preview", "点击播放预览录像"},
    {"MP4 video (preview N/A on device)", "MP4 录像（设备端预览暂不支持）"},
    {"Music", "音乐"},
    {"Video", "视频"},
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

    {"Storage", "存储"},
    {"Storage mode", "存储模式"},
    {"Choose how the SD card is used.", "选择 SD 卡的使用方式。"},
    {"Current mode", "当前模式"},
    {"Switch to", "切换为"},
    {"Running apps from SD card", "从 SD 卡运行应用与程序"},
    {"Export SD to PC for file updates", "将 SD 卡导出给电脑以更新文件"},
    {"Local Storage", "本地存储"},
    {"USB Disk Mode", "U盘模式"},
    {"Will switch from local storage to USB disk mode!", "将从本地存储切换为U盘模式！"},
    {"Will switch from USB disk mode to local storage and restart the app.", "将从U盘模式切换为本地存储且重启应用！"},
    {"Main app is restarting, please wait.", "主应用重启中，请稍候…"},
    {"USB mode works only on this page; leaving restores local storage.", "U盘模式仅在本页面有效，离开后将恢复本地存储并重启应用。"},
    {"USB mode exports SD to PC. Apps will stop. Eject on PC then set Local Storage and reboot.", "U盘模式将把 SD 卡导出给电脑，应用将退出。在电脑上安全弹出后，改回本地存储并重启设备。"},
    {"Local mode remounts SD and reboots to load the latest afht.sqsh4.", "本地存储将重新挂载 SD 卡并重启，以加载最新的 afht.sqsh4 程序。"},

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

    {"Show or hide apps on the home screen", "在主屏幕上显示或隐藏应用"},
    {"Please scan WiFi QR code", "请扫描 WiFi 二维码"},
    {"Waiting for network IP...", "等待获取网络 IP..."},
    {"wifi connect failed, please check wifi and close route !", "WiFi 连接失败，请检查网络并关闭路由"},
    {"WiFi driver not loaded", "WiFi 驱动未加载"},
    {"wifi driver unavailable", "WiFi 驱动不可用"},
    {"WiFi network list", "WiFi 网络列表"},
    {"Camera initializing...", "相机初始化中..."},
    {"Camera preview init failed", "相机预览初始化失败"},
    {"Camera preview starting...", "相机预览启动中..."},
    {"Preview unavailable", "无法预览"},
    {"Start video device fail", "视频设备启动失败"},
    {"No apps found", "未找到应用"},
    {"No AI config", "无智能算法配置"},
    {"WaitForScanning", "等待扫描"},
    {"Select SSID", "请选择 SSID"},
    {"Enter password", "请输入密码"},
    {"Please enter the SSID", "请输入SSID"},
    {"Please enter the SSID and password", "请输入SSID和密码"},
    {"WiFi status", "WiFi 状态"},
    {"connect wifi ok", "WiFi 连接成功"},
    {"password is error", "密码错误"},
    {"ssid is null", "SSID 为空"},
    {"connecting", "连接中"},
    {"Scanning", "扫描中"},
    {"Connect", "连接"},
    {"Refresh", "刷新"},
    {"Apps", "应用"},
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

static int language_index_from_settings(void)
{
    cJSON *root;
    cJSON *language;
    cJSON *id;
    int index = 0;

    root = app_user_load_root();
    if (!root)
        return 0;
    language = cJSON_GetObjectItem(root, "language");
    if (cJSON_IsObject(language)) {
        id = cJSON_GetObjectItem(language, "id");
        if (cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint <= 1)
            index = id->valueint;
    }
    cJSON_Delete(root);
    return index;
}

int get_lan_index()
{
    static int cached = -1;

    if (g_app_lv_data)
        return g_app_lv_data->language_index;
    /* App .so copies of this file are not the launcher instance. */
    if (cached < 0)
        cached = language_index_from_settings();
    return cached;
}

int set_lan_index(int lan_index)
{
    log_info( "in: \n");

    if(lan_index >= 0 && lan_index < 2)
    {
        g_app_lv_data->language_index = lan_index;
        app_lv_write_config();
        /* Language switched in Settings: restart the process when the current
         * app exits so every label (launcher + apps) comes up translated. */
        tablet_app_settings_dirty_mark();
        log_warn( "set lan to %d: \n", lan_index);
    }

    log_info( "out: \n");
    return 0;
}

/* Settings-changed marker under /tmp: the launcher and every app SO are
 * separate compilation units, so a file is the only state they share. */
#define TABLET_SETTINGS_DIRTY_FILE "/tmp/studio_settings_dirty"

void tablet_app_settings_dirty_mark(void)
{
    FILE *fp = fopen(TABLET_SETTINGS_DIRTY_FILE, "w");
    if (fp)
        fclose(fp);
}

bool tablet_app_settings_dirty_take(void)
{
    FILE *fp = fopen(TABLET_SETTINGS_DIRTY_FILE, "r");
    if (!fp)
        return false;
    fclose(fp);
    remove(TABLET_SETTINGS_DIRTY_FILE);
    return true;
}

char* lan(char* name)
{
    if (name == NULL)
        return name;
    if (get_lan_index() == 0)
        return name;

    for(int i=0; i<(int)(sizeof(g_chinese)/sizeof(g_chinese[0])); i++)
    {
        if (strcmp(g_chinese[i].name, name) == 0)
        {
            return g_chinese[i].text;
        }
    }

    return name;
}

void lan_app_name(const char *name, char *out, size_t out_len)
{
    const char *open_paren;
    const char *close_paren;
    const char *picked;
    size_t n;

    if (out == NULL || out_len == 0)
        return;
    out[0] = '\0';
    if (name == NULL || name[0] == '\0')
        return;

    /* app.json name is "中文(English)". Show one side for the current language. */
    open_paren = strchr(name, '(');
    close_paren = open_paren ? strrchr(open_paren, ')') : NULL;
    if (open_paren && close_paren && close_paren[1] == '\0' &&
        open_paren > name && close_paren > open_paren + 1) {
        if (get_lan_index() == 1) {
            picked = name;
            n = (size_t)(open_paren - name);
            while (n > 0 && picked[n - 1] == ' ')
                n--;
        } else {
            picked = open_paren + 1;
            n = (size_t)(close_paren - picked);
            while (n > 0 && *picked == ' ') {
                picked++;
                n--;
            }
            while (n > 0 && picked[n - 1] == ' ')
                n--;
        }
        if (n > 0) {
            if (n >= out_len)
                n = out_len - 1;
            memcpy(out, picked, n);
            out[n] = '\0';
            return;
        }
    }

    picked = lan((char *)name);
    if (picked == NULL || picked[0] == '\0')
        picked = name;
    snprintf(out, out_len, "%s", picked);
}

static int tablet_image_path_exists(const char *abs_path)
{
    return (abs_path != NULL && abs_path[0] != '\0' && access(abs_path, F_OK) == 0);
}

int tablet_resolve_image_path(const char *rel_name, char *out, size_t out_len)
{
    char try_path[384];
    char rel_with_res[256];
    char rel_with_image_res[320];
    const char *bases[] = {
        RES_IMAGE_PATH,
        RES_IMAGE_ROOT,
        RES_IMAGE_PATH_LEGACY,
    };
    int i;
    int j;

    if (rel_name == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    out[0] = '\0';

    snprintf(rel_with_res, sizeof(rel_with_res), "%s/%s", GUI_IMAGE_RES_PREFIX, rel_name);
    snprintf(rel_with_image_res, sizeof(rel_with_image_res),
             "image/%s/%s", GUI_IMAGE_RES_PREFIX, rel_name);

    for (i = 0; i < (int)(sizeof(bases) / sizeof(bases[0])); i++) {
        const char *rels[3];
        int rel_count = 0;

        rels[rel_count++] = rel_name;
        if (strncmp(rel_name, "image/", 6) != 0) {
            rels[rel_count++] = rel_with_res;
            rels[rel_count++] = rel_with_image_res;
        }

        for (j = 0; j < rel_count; j++) {
            snprintf(try_path, sizeof(try_path), "%s/%s", bases[i], rels[j]);
            if (tablet_image_path_exists(try_path)) {
                snprintf(out, out_len, "%s", try_path);
                return 0;
            }
            if (bases[i] == RES_IMAGE_ROOT && strncmp(rels[j], "image/", 6) != 0) {
                snprintf(try_path, sizeof(try_path), "%s/image/%s", bases[i], rels[j]);
                if (tablet_image_path_exists(try_path)) {
                    snprintf(out, out_len, "%s", try_path);
                    return 0;
                }
            }
        }
    }

    snprintf(out, out_len, "%s/%s", RES_IMAGE_PATH, rel_name);
    return -1;
}

void tablet_log_pointer_event(lv_event_t *e, const char *tag)
{
    lv_indev_t *indev;
    lv_point_t point;
    lv_obj_t *target;
    lv_area_t area;

    if (e == NULL) {
        return;
    }

    indev = lv_indev_active();
    if (indev != NULL) {
        lv_indev_get_point(indev, &point);
        log_debug("[%s] event=%d point=(%d,%d)",
                 tag != NULL ? tag : "input",
                 (int)lv_event_get_code(e), (int)point.x, (int)point.y);
    } else {
        log_debug("[%s] event=%d point=(n/a,no_indev)",
                 tag != NULL ? tag : "input", (int)lv_event_get_code(e));
    }

    target = (lv_obj_t *)lv_event_get_target(e);
    if (target != NULL) {
        lv_obj_get_coords(target, &area);
        log_debug("[%s] target coords (%d,%d)-(%d,%d)",
                 tag != NULL ? tag : "input",
                 (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2);
    }
}

void tablet_log_obj_bounds(const char *tag, lv_obj_t *obj)
{
    lv_area_t area;
    const char *name = (tag != NULL) ? tag : "obj";

    if (obj == NULL) {
        log_info("[layout][%s] obj=NULL", name);
        return;
    }

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &area);

    log_info("[layout][%s] xy=(%d,%d) wh=(%d,%d) box=(%d,%d)-(%d,%d) hidden=%d clickable=%d",
             name,
             (int)area.x1, (int)area.y1,
             (int)lv_obj_get_width(obj), (int)lv_obj_get_height(obj),
             (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
             lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
             lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? 1 : 0);
}

void tablet_input_log_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED ||
        code == LV_EVENT_RELEASED) {
        tablet_log_pointer_event(e, "input_cb");
    }
}

/**
* @brief   set image source
* @param   [IN]obj: image object
* @param   [IN]name: name of image
* @return  lv_res_t
* @retval  see definition of lv_res_t.
*/
lv_res_t tablet_image_set_src(lv_obj_t *obj, const char *name)
{
    if ((NULL == obj) || (NULL == name))
    {
        LV_LOG_ERROR("parameter error.\n");
        return LV_RESULT_INVALID;
    }

    const char *use_name = name;
    char resolved[384];
    char fs_path[384];

    /* Fallback for 音源定位 (SSL) icon when ssl.png is missing on device */
    if (strcmp(name, ICON_SSL_NAME) == 0) {
        if (tablet_resolve_image_path(name, fs_path, sizeof(fs_path)) != 0) {
            use_name = ICON_SSL_FALLBACK;
            log_warn("ssl.png not found, use %s for SoundSource icon", ICON_SSL_FALLBACK);
        }
    }

    if (tablet_resolve_image_path(use_name, resolved, sizeof(resolved)) != 0) {
        log_warn("image not found, try load: %s", resolved);
    }

    char path[384] = {0};
    size_t len = strlen(use_name);
    if ((len >= 3 && strcmp(use_name + len - 3, "jpg") == 0) ||
        (len >= 4 && strcmp(use_name + len - 4, "jpeg") == 0) ||
        (len >= 3 && strcmp(use_name + len - 3, "png") == 0) ||
        (len >= 3 && strcmp(use_name + len - 3, "qoi") == 0))
    {
        snprintf(path, sizeof(path), "%c:%s", LV_FS_STDIO_LETTER, resolved);
    }
    else
    {
        LV_LOG_ERROR("unsupported image\n");
        return LV_RESULT_INVALID;
    }

    lv_image_set_src(obj, path);

    return LV_RESULT_OK;
}

/*********************
 *  Home indicator bar
 *********************/
/* Phone-style bottom home bar: floats above all app content on the top layer,
 * tap or swipe up re-sends CLICKED to the app's back button. Apps with full-width
 * bottom bars (camera/volc/gallery) reserve this height for the indicator zone. */
#define TABLET_HOME_BAR_W       GUI_SCALE(220)
#define TABLET_HOME_PILL_W      GUI_SCALE(108)
#define TABLET_HOME_PILL_H      GUI_SCALE_VER(5)
#define TABLET_HOME_BAR_MARGIN  GUI_SCALE_VER(4)
/* Deliberate exit: pull up ~1/5 of the panel height. A plain tap does nothing. */
#define TABLET_HOME_SWIPE_DY    GUI_SCALE_VER(96)

static lv_point_t s_home_bar_press;

static void tablet_home_bar_event_cb(lv_event_t *e)
{
    lv_obj_t *back_btn = (lv_obj_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev;
    lv_point_t pt;

    if (code != LV_EVENT_PRESSED && code != LV_EVENT_CLICKED && code != LV_EVENT_RELEASED)
        return;
    if (!back_btn || !lv_obj_is_valid(back_btn))
        return;

    if (code == LV_EVENT_PRESSED) {
        indev = lv_indev_active();
        if (indev)
            lv_indev_get_point(indev, &s_home_bar_press);
        return;
    }

    if (code == LV_EVENT_CLICKED)
        return; /* tap alone must not leave the app */

    /* RELEASED: only a real upward pull from the bar leaves the app. */
    indev = lv_indev_active();
    if (!indev)
        return;
    lv_indev_get_point(indev, &pt);
    if (s_home_bar_press.y - pt.y > TABLET_HOME_SWIPE_DY)
        lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
}

/* App window goes away -> its home bar goes with it. Async: the bar may be
 * mid-dispatch (tap -> back handler -> window delete) and must not be freed
 * while its own event callback is still on the stack. */
static void tablet_home_bar_window_delete_cb(lv_event_t *e)
{
    lv_obj_t *bar = (lv_obj_t *)lv_event_get_user_data(e);

    if (bar && lv_obj_is_valid(bar))
        lv_obj_delete_async(bar);
}

/* Fired after the app's own back handler on the same anchor: when Settings
 * changed (language / app switches) this session, restart the whole process
 * so the launcher menu and every label come up with the new settings. */
static void tablet_app_exit_settings_restart_cb(lv_event_t *e)
{
    (void)e;
    if (!tablet_app_settings_dirty_take())
        return;
    log_warn("settings changed this session; relaunching the main process");
    (void)system("(sleep 1; killall nova_main ipc_main ipc_home 2>/dev/null; sleep 1; "
                 "/usr/sbin/start_ipc_main.sh --bg) >/tmp/studio_relaunch.log 2>&1 &");
    exit(0);
}

void tablet_create_home_bar(lv_obj_t *window, lv_event_cb_t on_back, void *user_data)
{
    lv_obj_t *bar;
    lv_obj_t *pill;
    lv_obj_t *anchor;

    if (!window || !on_back)
        return;

    bar = lv_obj_create(lv_layer_top());
    if (!bar)
        return;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, TABLET_HOME_BAR_W, TABLET_HOME_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -TABLET_HOME_BAR_MARGIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(bar, GUI_SCALE(6));

    /* Invisible anchor carrying the app's back handler; the bar re-sends
     * CLICKED here on tap / swipe-up. */
    anchor = lv_button_create(bar);
    if (!anchor) {
        lv_obj_delete(bar);
        return;
    }
    lv_obj_remove_style_all(anchor);
    lv_obj_add_flag(anchor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(anchor, 1, 1);
    lv_obj_add_event_cb(anchor, on_back, LV_EVENT_CLICKED, user_data);
    /* Runs after the app's back handler (registration order): restart the
     * process when Settings changed while this app was open. */
    lv_obj_add_event_cb(anchor, tablet_app_exit_settings_restart_cb, LV_EVENT_CLICKED, NULL);

    pill = lv_obj_create(bar);
    if (pill) {
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, TABLET_HOME_PILL_W, TABLET_HOME_PILL_H);
        lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_add_event_cb(bar, tablet_home_bar_event_cb, LV_EVENT_ALL, anchor);
    lv_obj_add_event_cb(window, tablet_home_bar_window_delete_cb, LV_EVENT_DELETE, bar);
}

lv_obj_t *tablet_create_top(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop)
{
    lv_obj_t *bar = lv_obj_create(parent);
    if (NULL == bar)
    {
        LV_LOG_ERROR("failed to create bar\n");
        return NULL;
    }

    lv_obj_remove_style_all(bar);

    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(bar, LV_PCT(100), LV_PCT(10));
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x060606), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_0, 0);

    /* Title only. No on-screen back control: leaving the app is the home bar. */
    lv_font_t *font = tablet_font_get_freetype_font(GUI_SCALE(40));
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

    /* Phone-style home indicator: swipe up to leave the app. */
    tablet_create_home_bar(parent, btn_back_event_handle, desktop);

    return bar;
}
lv_obj_t *tablet_create_top_2(lv_obj_t *parent, const char *label, lv_event_cb_t btn_back_event_handle, lv_obj_t *desktop)
{
    /* Same chrome as tablet_create_top(); kept for the ipc_camera call site. */
    return tablet_create_top(parent, label, btn_back_event_handle, desktop);
}

lv_obj_t *tablet_create_obj(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    return obj;
}

/**
 * @brief Show note message box.
 * @param str Message string.
 * @return
 */
void show_msgbox_note(const char *str)
{
    /* Create message box */
    lv_obj_t * mbox = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox, LV_PCT(40), LV_PCT(30));

    lv_obj_t * background = lv_obj_get_parent(mbox);
    lv_obj_set_style_bg_opa(background, LV_OPA_0, 0);

    lv_obj_center(mbox);

    lv_font_t *font = tablet_font_get_freetype_font(GUI_SCALE(20));

    lv_obj_t * title = lv_msgbox_add_title(mbox, lan("note"));
    lv_obj_set_style_text_font(title, font, 0);

    lv_obj_t *txt = lv_msgbox_add_text(mbox, str);
    lv_obj_set_style_text_font(txt, font, 0);

    lv_obj_t * btn = lv_msgbox_add_close_button(mbox);
    lv_obj_set_size(btn, LV_PCT(20), LV_PCT(100));
}

/* Saving hint: non-closeable toast overlay, dismissed programmatically. */
static lv_obj_t *s_saving_hint_mbox;

#define PILL_BG_COLOR     0x202124
#define PILL_OK_COLOR     0x34C759
#define PILL_ERR_COLOR    0xFF453A
#define PILL_FADE_MS      180

static void pill_opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void pill_fade(lv_obj_t *obj, int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, PILL_FADE_MS);
    lv_anim_set_exec_cb(&a, pill_opa_anim_cb);
    if (done)
        lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

/* Rounded dark pill on the top layer with a row layout for icon + text.
 * Not clickable, so touches go to the screen underneath. */
static lv_obj_t *pill_create(const char *str, lv_obj_t **label_out)
{
    lv_obj_t *cont;
    lv_obj_t *label;
    lv_font_t *font;

    cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(cont, LV_PCT(80), 0);
    lv_obj_set_style_pad_hor(cont, GUI_SCALE(22), 0);
    lv_obj_set_style_pad_ver(cont, GUI_SCALE_VER(12), 0);
    lv_obj_set_style_pad_column(cont, GUI_SCALE(12), 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(PILL_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_90, 0);
    lv_obj_set_style_radius(cont, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(cont, GUI_SCALE(16), 0);
    lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(cont, LV_OPA_40, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    font = tablet_font_get_freetype_font(GUI_SCALE(22));
    label = lv_label_create(cont);
    lv_label_set_text(label, str ? str : "");
    if (font)
        lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    if (label_out)
        *label_out = label;
    return cont;
}

void show_saving_hint(const char *str)
{
    lv_obj_t *cont;
    lv_obj_t *label;
    lv_obj_t *spin;
    int32_t sz = GUI_SCALE(24);

    close_saving_hint();  /* close any previous instance first */

    cont = pill_create(str, &label);
    spin = lv_spinner_create(cont);
    lv_obj_set_size(spin, sz, sz);
    lv_obj_set_style_arc_width(spin, GUI_SCALE(3), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, GUI_SCALE(3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x5A5A5E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_move_to_index(spin, 0);
    (void)label;
    lv_obj_center(cont);

    /* No close button — only close_saving_hint() can dismiss this dialog */
    s_saving_hint_mbox = cont;
}

void close_saving_hint(void)
{
    if (!s_saving_hint_mbox)
        return;
    if (!lv_obj_is_valid(s_saving_hint_mbox)) {
        s_saving_hint_mbox = NULL;
        return;
    }
    lv_obj_delete(s_saving_hint_mbox);
    s_saving_hint_mbox = NULL;
}

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_close(void)
{
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast && lv_obj_is_valid(s_toast)) {
        lv_anim_delete(s_toast, NULL);
        lv_obj_delete(s_toast);
    }
    s_toast = NULL;
}

static void toast_fade_out_done(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;

    if (obj == s_toast)
        s_toast = NULL;
    if (obj && lv_obj_is_valid(obj))
        lv_obj_delete_async(obj);
}

static void toast_timer_cb(lv_timer_t *t)
{
    s_toast_timer = NULL;
    lv_timer_delete(t);
    if (s_toast && lv_obj_is_valid(s_toast))
        pill_fade(s_toast, LV_OPA_COVER, LV_OPA_TRANSP, toast_fade_out_done);
    else
        s_toast = NULL;
}

void show_toast_note(const char *str, uint32_t duration_ms, int is_error)
{
    lv_obj_t *dot;
    int32_t d = GUI_SCALE(10);

    if (!str || !str[0])
        return;
    toast_close();

    s_toast = pill_create(str, NULL);
    dot = lv_obj_create(s_toast);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(is_error ? PILL_ERR_COLOR : PILL_OK_COLOR), 0);
    lv_obj_move_to_index(dot, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(int32_t)(LV_VER_RES / 4));

    lv_obj_set_style_opa(s_toast, LV_OPA_TRANSP, 0);
    pill_fade(s_toast, LV_OPA_TRANSP, LV_OPA_COVER, NULL);
    s_toast_timer = lv_timer_create(toast_timer_cb, duration_ms > 0 ? duration_ms : 1500u, NULL);
}

/* Extended data for message box */
typedef struct {
    lv_obj_t *footer;
    lv_obj_t *content;
    footer_btn_cb_t btn_cb;
} msgbox_ext_t;

#define STORAGE_RESTART_UI_LOCK "/tmp/storage_restart_ui_lock"

static lv_obj_t *s_restart_block_overlay;
static lv_obj_t *s_restart_wait_msgbox;
static lv_obj_t *s_restart_wait_text;

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
    lv_obj_set_style_text_font(obj_title, tablet_font_get_freetype_font(GUI_SCALE(20)), 0);
    lv_obj_t * obj_content = lv_msgbox_add_text(msgbox, content);
    lv_obj_set_style_text_font(obj_content, tablet_font_get_freetype_font(GUI_SCALE(20)), 0);
    lv_obj_set_size(msgbox, GUI_SCALE(400), GUI_SCALE_VER(150));
    if(has_close_btn) lv_msgbox_add_close_button(msgbox);

    /* Allocate extended data */
    msgbox_ext_t * ext = lv_malloc(sizeof(msgbox_ext_t));
    //lv_memset_00(ext, sizeof(msgbox_ext_t));
    memset(ext, 0, sizeof(msgbox_ext_t));
    lv_obj_set_user_data(msgbox, ext);
    ext->btn_cb = btn_cb;
    ext->content = obj_content;

    /* Create footer container */
    ext->footer = lv_obj_create(msgbox);
    lv_obj_set_size(ext->footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ext->footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ext->footer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ext->footer, GUI_SCALE(10), 0);
    lv_obj_set_style_border_width(ext->footer, 1, 0);
    lv_obj_set_style_border_color(ext->footer, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_border_side(ext->footer, LV_BORDER_SIDE_TOP, 0);

    /* Add buttons */
    for (uint32_t i = 0; btn_texts[i] != NULL; i++) {
        lv_obj_t * btn = lv_button_create(ext->footer);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, GUI_SCALE_VER(40));
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_min_width(btn, GUI_SCALE(80), 0);

        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, btn_texts[i]);
        lv_obj_set_style_text_font(label, tablet_font_get_freetype_font(GUI_SCALE(20)), 0);
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

void msgbox_show_restart_waiting(const char *message)
{
    lv_obj_t *top;
    const char *text;
    lv_font_t *font;
    lv_obj_t *title;

    if (!message || !message[0]) {
        text = lan("Main app is restarting, please wait.");
        if (!text) {
            text = "Main app is restarting, please wait.";
        }
    } else {
        text = message;
    }

    top = lv_layer_top();
    font = tablet_font_get_freetype_font(20);

    /* No full-screen black mask: keep launcher/settings visible behind the dialog. */
    if (s_restart_block_overlay && lv_obj_is_valid(s_restart_block_overlay)) {
        lv_obj_add_flag(s_restart_block_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_restart_wait_msgbox || !lv_obj_is_valid(s_restart_wait_msgbox)) {
        s_restart_wait_msgbox = lv_msgbox_create(top);
        lv_obj_set_width(s_restart_wait_msgbox, LV_PCT(70));
        lv_obj_center(s_restart_wait_msgbox);
        title = lv_msgbox_add_title(s_restart_wait_msgbox, lan("note"));
        if (font && title) {
            lv_obj_set_style_text_font(title, font, 0);
        }
        s_restart_wait_text = lv_msgbox_add_text(s_restart_wait_msgbox, text);
        if (font && s_restart_wait_text) {
            lv_obj_set_style_text_font(s_restart_wait_text, font, 0);
        }
    } else {
        if (s_restart_wait_text && lv_obj_is_valid(s_restart_wait_text)) {
            lv_label_set_text(s_restart_wait_text, text);
        }
        lv_obj_clear_flag(s_restart_wait_msgbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_restart_wait_msgbox);
    }
    lv_obj_move_foreground(s_restart_wait_msgbox);
    lv_obj_update_layout(s_restart_wait_msgbox);
}

void msgbox_flush_restart_waiting_frame(void)
{
    lv_refr_now(NULL);
}

void msgbox_hold_restart_frame_if_needed(void)
{
    if (access(STORAGE_RESTART_UI_LOCK, F_OK) != 0) {
        return;
    }
    /* Second insurance before lv_port_deinit: ensure wait dialog is in fb0. */
    msgbox_show_restart_waiting(NULL);
    lv_refr_now(NULL);
}

void msgbox_enter_restart_blocking(lv_obj_t *msgbox, const char *message)
{
    lv_obj_t *top;
    const char *text;

    if (!message || !message[0]) {
        text = lan("Main app is restarting, please wait.");
        if (!text) {
            text = "Main app is restarting, please wait.";
        }
    } else {
        text = message;
    }

    top = lv_layer_top();
    if (!s_restart_block_overlay || !lv_obj_is_valid(s_restart_block_overlay)) {
        s_restart_block_overlay = lv_obj_create(top);
        lv_obj_remove_style_all(s_restart_block_overlay);
        lv_obj_set_size(s_restart_block_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_restart_block_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_restart_block_overlay, LV_OPA_50, 0);
        lv_obj_add_flag(s_restart_block_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_restart_block_overlay, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clear_flag(s_restart_block_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_restart_block_overlay);
    }

    if (!msgbox || !lv_obj_is_valid(msgbox)) {
        return;
    }

    {
        msgbox_ext_t *ext = lv_obj_get_user_data(msgbox);

        if (ext) {
            if (ext->footer) {
                lv_obj_add_flag(ext->footer, LV_OBJ_FLAG_HIDDEN);
            }
            if (ext->content) {
                lv_label_set_text(ext->content, text);
            }
        }
    }

    lv_obj_set_parent(msgbox, top);
    lv_obj_center(msgbox);
    lv_obj_move_foreground(msgbox);
    lv_obj_update_layout(msgbox);
}
