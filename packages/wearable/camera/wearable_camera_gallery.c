// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * In-app photo/video gallery (sub-screen inside Camera app).
 * Lists and plays LCM2 composite media (device reads embedded preview NV12 only).
 */
#define LOG_TAG "WEARABLE_CAMERA_GALLERY"

#include "wearable_camera_gallery.h"
#include "wearable_camera_container.h"
#include "wearable_camera_media.h"
#include "wearable_camera_tfcard.h"

#include "app_main.h"
#include "gui_common.h"
#include "gui_camera.h"
#include "gui_font.h"
#include "lv_display_config.h"
#include "multimedia.h"
#include "log.h"

#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GALLERY_FILE_MAX   128
#define GALLERY_NAME_MAX   64
#define GALLERY_PATH_MAX   320
#define GALLERY_PLAY_FPS   15
#define GALLERY_TOP_H              36
#define GALLERY_BACK_W             48
#define GALLERY_TAB_W              76
#define GALLERY_TAB_H              28
#define GALLERY_TAB_PHOTO_X        52
#define GALLERY_TAB_VIDEO_X        132
#define GALLERY_PREVIEW_BAR_H        44
#define GALLERY_LIST_INPUT_BLOCK_MS  400

typedef struct {
    char path[GALLERY_PATH_MAX];
    char name[GALLERY_NAME_MAX];
    time_t mtime;
} gallery_item_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *parent_hide;
    lv_obj_t *preview_root;
    lv_obj_t *preview_image;
    lv_image_dsc_t preview_dsc;
    void *preview_buffer;
    uint32_t preview_w;
    uint32_t preview_h;
    wearable_gallery_type_t tab;
    wearable_gallery_close_cb_t on_close;
    void *user_data;
    gallery_item_t items[GALLERY_FILE_MAX];
    int item_count;
    int preview_item_idx;
    wearable_lcm_hdr_t lcm_hdr;
    FILE *nv12_play_fp;
    lv_timer_t *nv12_play_timer;
} gallery_ctx_t;

static gallery_ctx_t s_gallery;
static uint32_t s_list_block_until_tick;

static void gallery_obj_strip_pad(lv_obj_t *obj);
static void gallery_schedule_preview_close(void);
static lv_font_t *gallery_tab_font(void);
static lv_obj_t *gallery_create_preview_action_bar(lv_obj_t *parent);
static void gallery_add_preview_actions(lv_obj_t *bar);

static void event_preview_delete(lv_event_t *e);
static void event_preview_back(lv_event_t *e);
static void event_list_item(lv_event_t *e);
static void gallery_fill_list(void);

static int starts_with_ci(const char *s, const char *prefix)
{
    size_t n;

    if (!s || !prefix)
        return 0;
    n = strlen(prefix);
    if (strlen(s) < n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl;
    size_t su;

    if (!s || !suffix)
        return 0;
    sl = strlen(s);
    su = strlen(suffix);
    if (sl < su)
        return 0;
    for (size_t i = 0; i < su; i++) {
        if (tolower((unsigned char)s[sl - su + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

static int path_is_jpeg(const char *path)
{
    return ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
}

static int path_is_mp4(const char *path)
{
    return ends_with_ci(path, ".mp4");
}

static int match_tab(const char *name, wearable_gallery_type_t tab)
{
    if (!name)
        return 0;

    if (tab == WEARABLE_GALLERY_PHOTO) {
        if (!starts_with_ci(name, WEARABLE_PHOTO_PREFIX))
            return 0;
        return path_is_jpeg(name) || ends_with_ci(name, WEARABLE_PHOTO_EXT);
    }

    if (!starts_with_ci(name, WEARABLE_VIDEO_PREFIX))
        return 0;
    return path_is_mp4(name) || ends_with_ci(name, WEARABLE_VIDEO_EXT);
}

static int cmp_item_desc(const void *a, const void *b)
{
    const gallery_item_t *ia = (const gallery_item_t *)a;
    const gallery_item_t *ib = (const gallery_item_t *)b;
    if (ia->mtime > ib->mtime) return -1;
    if (ia->mtime < ib->mtime) return 1;
    return 0;
}

static void gallery_free_preview_buffer(void)
{
    if (s_gallery.preview_buffer) {
        free(s_gallery.preview_buffer);
        s_gallery.preview_buffer = NULL;
    }
    memset(&s_gallery.preview_dsc, 0, sizeof(s_gallery.preview_dsc));
    s_gallery.preview_w = 0;
    s_gallery.preview_h = 0;
}

static void gallery_stop_nv12_player(void)
{
    if (s_gallery.nv12_play_timer) {
        lv_timer_delete(s_gallery.nv12_play_timer);
        s_gallery.nv12_play_timer = NULL;
    }
    if (s_gallery.nv12_play_fp) {
        fclose(s_gallery.nv12_play_fp);
        s_gallery.nv12_play_fp = NULL;
    }
}

static void gallery_close_preview(void)
{
    gallery_stop_nv12_player();
    gallery_free_preview_buffer();
    s_gallery.preview_item_idx = -1;
    if (s_gallery.preview_root) {
        lv_obj_delete(s_gallery.preview_root);
        s_gallery.preview_root = NULL;
        s_gallery.preview_image = NULL;
    }
}

static void gallery_close_all(void)
{
    gallery_close_preview();
    if (s_gallery.root) {
        lv_obj_delete(s_gallery.root);
        s_gallery.root = NULL;
    }
    if (s_gallery.parent_hide)
        lv_obj_remove_flag(s_gallery.parent_hide, LV_OBJ_FLAG_HIDDEN);
    if (s_gallery.on_close)
        s_gallery.on_close(s_gallery.user_data);
    s_list_block_until_tick = 0;
    memset(&s_gallery, 0, sizeof(s_gallery));
}

static void gallery_obj_strip_pad(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static lv_font_t *gallery_tab_font(void)
{
    lv_font_t *font = gui_font_get_cjk_font(16);

    if (!font)
        font = (lv_font_t *)&lv_font_montserrat_14;
    return font;
}

static void preview_close_defer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    gallery_close_preview();
}

static void gallery_schedule_preview_close(void)
{
    s_list_block_until_tick = lv_tick_get() + GALLERY_LIST_INPUT_BLOCK_MS;
    {
        lv_timer_t *tm = lv_timer_create(preview_close_defer_cb, 50, NULL);

        if (tm)
            lv_timer_set_repeat_count(tm, 1);
    }
}

static lv_obj_t *gallery_create_preview_action_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);

    gallery_obj_strip_pad(bar);
    lv_obj_set_size(bar, lv_pct(100), GALLERY_PREVIEW_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static void gallery_add_preview_actions(lv_obj_t *bar)
{
    lv_obj_t *del;
    lv_obj_t *back;
    lv_obj_t *lb_del;
    lv_obj_t *lb_back;
    lv_font_t *font = gallery_tab_font();

    del = lv_button_create(bar);
    lv_obj_set_size(del, 96, 36);
    lv_obj_align(del, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(del, event_preview_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(del, event_preview_delete, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_ext_click_area(del, 8);
    lb_del = lv_label_create(del);
    lv_label_set_text(lb_del, get_lan_index() == 1 ? "删除" : "Delete");
    lv_obj_set_style_text_font(lb_del, font, 0);
    lv_obj_center(lb_del);

    back = lv_button_create(bar);
    lv_obj_set_size(back, 96, 36);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(back, event_preview_back, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(back, event_preview_back, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_ext_click_area(back, 8);
    lb_back = lv_label_create(back);
    lv_label_set_text(lb_back, get_lan_index() == 1 ? "返回" : "Back");
    lv_obj_set_style_text_font(lb_back, font, 0);
    lv_obj_center(lb_back);
}

static void event_gallery_back(lv_event_t *e)
{
    (void)e;
    gallery_close_all();
}

static void event_preview_back(lv_event_t *e)
{
    (void)e;
    gallery_schedule_preview_close();
}

static int gallery_scan(wearable_gallery_type_t tab)
{
    char dir[GALLERY_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    const char *subdir = (tab == WEARABLE_GALLERY_PHOTO) ? WEARABLE_PHOTO_SUBDIR : WEARABLE_RECORD_SUBDIR;

    s_gallery.item_count = 0;
    if (!wearable_tfcard_is_ready())
        return 0;
    if (wearable_tfcard_build_subdir(subdir, dir, sizeof(dir)) != 0)
        return 0;

    d = opendir(dir);
    if (!d)
        return 0;

    while ((ent = readdir(d)) != NULL && s_gallery.item_count < GALLERY_FILE_MAX) {
        char full[GALLERY_PATH_MAX];
        struct stat st;

        if (ent->d_name[0] == '.')
            continue;
        if (!match_tab(ent->d_name, tab))
            continue;

        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (stat(full, &st) != 0)
            continue;

        gallery_item_t *it = &s_gallery.items[s_gallery.item_count++];
        snprintf(it->path, sizeof(it->path), "%s", full);
        snprintf(it->name, sizeof(it->name), "%s", ent->d_name);
        it->mtime = st.st_mtime;
    }
    closedir(d);
    qsort(s_gallery.items, (size_t)s_gallery.item_count, sizeof(gallery_item_t), cmp_item_desc);
    return s_gallery.item_count;
}

static int gallery_bind_nv12_image(lv_obj_t *img, uint32_t width, uint32_t height, const void *data)
{
    size_t rgb_size;

    if (!img || !data || width == 0 || height == 0)
        return -1;

    /* Luna KM01W has no GPU/IPU: raw NV12 cannot be blitted to the SW framebuffer
     * (GPU path is skipped for non-DMA buffers and the SW renderer can't handle
     * planar YUV -> crash). Convert to RGB565 up front, same as the live preview. */
    rgb_size = (size_t)width * (size_t)height * 2u;
    gallery_free_preview_buffer();

    s_gallery.preview_buffer = malloc(rgb_size);
    if (!s_gallery.preview_buffer)
        return -1;

    if (gui_camera_nv12_to_rgb565(data, width, height, 0,
                                  (uint16_t *)s_gallery.preview_buffer) != 0) {
        gallery_free_preview_buffer();
        return -1;
    }

    s_gallery.preview_w = width;
    s_gallery.preview_h = height;

    memset(&s_gallery.preview_dsc, 0, sizeof(s_gallery.preview_dsc));
    s_gallery.preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_gallery.preview_dsc.header.cf = lv_display_rgb565_format();
    s_gallery.preview_dsc.header.w = (uint16_t)width;
    s_gallery.preview_dsc.header.h = (uint16_t)height;
    s_gallery.preview_dsc.header.stride = (uint16_t)(width * 2u);
    s_gallery.preview_dsc.data = (const uint8_t *)s_gallery.preview_buffer;
    s_gallery.preview_dsc.data_size = rgb_size;

    lv_image_set_src(img, &s_gallery.preview_dsc);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    return 0;
}

static void gallery_show_photo_preview(const char *path)
{
    uint32_t w = 0;
    uint32_t h = 0;
    size_t size;
    void *buf;
    lv_obj_t *img;
    lv_obj_t *action_bar;
    char fs_path[GALLERY_PATH_MAX + 8];
    lv_display_t *disp = lv_display_get_default();
    int scr_h = disp ? (int)lv_display_get_vertical_resolution(disp) : 240;

    gallery_close_preview();

    s_gallery.preview_root = lv_obj_create(lv_display_get_screen_active(disp));
    gallery_obj_strip_pad(s_gallery.preview_root);
    lv_obj_set_size(s_gallery.preview_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.preview_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_gallery.preview_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.preview_root, LV_OBJ_FLAG_SCROLLABLE);

    img = lv_image_create(s_gallery.preview_root);
    s_gallery.preview_image = img;
    lv_obj_set_size(img, lv_pct(100), scr_h - GALLERY_PREVIEW_BAR_H);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);

    if (path_is_jpeg(path)) {
        snprintf(fs_path, sizeof(fs_path), "%c:%s", LV_FS_STDIO_LETTER, path);
        lv_image_set_src(img, fs_path);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    } else {
        if (wearable_lcm_read_header(path, &s_gallery.lcm_hdr) != 0) {
            gallery_close_preview();
            show_msgbox_note(get_lan_index() == 1 ? "无法打开照片" : "Cannot open photo");
            return;
        }

        size = wearable_lcm_preview_frame_bytes(s_gallery.lcm_hdr.preview_w, s_gallery.lcm_hdr.preview_h);
        buf = malloc(size);
        if (!buf) {
            gallery_close_preview();
            return;
        }

        if (wearable_lcm_read_photo_preview(path, buf, size, &w, &h) != 0) {
            free(buf);
            gallery_close_preview();
            show_msgbox_note(get_lan_index() == 1 ? "无法打开照片" : "Cannot open photo");
            return;
        }

        (void)gallery_bind_nv12_image(img, w, h, buf);
        free(buf);
    }

    action_bar = gallery_create_preview_action_bar(s_gallery.preview_root);
    gallery_add_preview_actions(action_bar);
}

static void nv12_play_timer_cb(lv_timer_t *timer)
{
    size_t frame_size;
    void *frame_buf;

    (void)timer;

    if (!s_gallery.nv12_play_fp || !s_gallery.preview_image)
        return;

    frame_size = wearable_lcm_preview_frame_bytes(s_gallery.lcm_hdr.preview_w,
                                              s_gallery.lcm_hdr.preview_h);
    frame_buf = malloc(frame_size);
    if (!frame_buf)
        return;

    if (fread(frame_buf, 1, frame_size, s_gallery.nv12_play_fp) != frame_size) {
        free(frame_buf);
        fseek(s_gallery.nv12_play_fp, (long)s_gallery.lcm_hdr.preview_offset, SEEK_SET);
        return;
    }

    (void)gallery_bind_nv12_image(s_gallery.preview_image,
                                  s_gallery.lcm_hdr.preview_w,
                                  s_gallery.lcm_hdr.preview_h, frame_buf);
    free(frame_buf);
    lv_obj_invalidate(s_gallery.preview_image);
}

static void gallery_show_video_preview(const char *path, const char *name)
{
    lv_obj_t *title;
    lv_obj_t *info;
    lv_obj_t *action_bar;
    lv_display_t *disp = lv_display_get_default();
    int scr_h = disp ? (int)lv_display_get_vertical_resolution(disp) : 240;

    gallery_close_preview();

    if (path_is_mp4(path)) {
        s_gallery.preview_root = lv_obj_create(lv_display_get_screen_active(disp));
        gallery_obj_strip_pad(s_gallery.preview_root);
        lv_obj_set_size(s_gallery.preview_root, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_gallery.preview_root, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(s_gallery.preview_root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_gallery.preview_root, LV_OBJ_FLAG_SCROLLABLE);

        title = lv_label_create(s_gallery.preview_root);
        lv_label_set_text(title, name ? name : path);
        lv_obj_set_width(title, lv_pct(95));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

        info = lv_label_create(s_gallery.preview_root);
        lv_label_set_text(info, get_lan_index() == 1 ? "MP4 录像（设备端预览暂不支持）" : "MP4 video (preview N/A on device)");
        lv_obj_set_width(info, lv_pct(90));
        lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_text_color(info, lv_color_hex(0xcccccc), 0);
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

        action_bar = gallery_create_preview_action_bar(s_gallery.preview_root);
        gallery_add_preview_actions(action_bar);
        return;
    }

    s_gallery.nv12_play_fp = wearable_lcm_open_video_preview(path, &s_gallery.lcm_hdr);
    if (!s_gallery.nv12_play_fp) {
        show_msgbox_note(get_lan_index() == 1 ? "无法打开录像" : "Cannot open video");
        return;
    }

    s_gallery.preview_root = lv_obj_create(lv_display_get_screen_active(disp));
    gallery_obj_strip_pad(s_gallery.preview_root);
    lv_obj_set_size(s_gallery.preview_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.preview_root, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_gallery.preview_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.preview_root, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(s_gallery.preview_root);
    lv_label_set_text(title, name ? name : path);
    lv_obj_set_width(title, lv_pct(95));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    s_gallery.preview_image = lv_image_create(s_gallery.preview_root);
    lv_obj_set_size(s_gallery.preview_image, lv_pct(100), scr_h - GALLERY_PREVIEW_BAR_H - 22);
    lv_obj_align(s_gallery.preview_image, LV_ALIGN_TOP_MID, 0, 20);

    s_gallery.nv12_play_timer = lv_timer_create(nv12_play_timer_cb, 1000 / GALLERY_PLAY_FPS, NULL);
    nv12_play_timer_cb(NULL);

    action_bar = gallery_create_preview_action_bar(s_gallery.preview_root);
    gallery_add_preview_actions(action_bar);
}

static void event_preview_delete(lv_event_t *e)
{
    const char *path;
    int idx;

    (void)e;
    idx = s_gallery.preview_item_idx;
    if (idx < 0 || idx >= s_gallery.item_count)
        return;

    path = s_gallery.items[idx].path;
    if (wearable_camera_media_delete_file(path) != 0) {
        show_msgbox_note(get_lan_index() == 1 ? "删除失败" : "Delete failed");
        return;
    }

    gallery_schedule_preview_close();
    gallery_fill_list();
    show_msgbox_note(get_lan_index() == 1 ? "已删除" : "Deleted");
}

static void event_list_item(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    if (lv_tick_get() < s_list_block_until_tick)
        return;

    if (idx < 0 || idx >= s_gallery.item_count)
        return;

    s_gallery.preview_item_idx = idx;

    if (s_gallery.tab == WEARABLE_GALLERY_PHOTO)
        gallery_show_photo_preview(s_gallery.items[idx].path);
    else
        gallery_show_video_preview(s_gallery.items[idx].path, s_gallery.items[idx].name);
}

static void gallery_fill_list(void)
{
    int i;

    if (!s_gallery.list)
        return;
    lv_obj_clean(s_gallery.list);

    if (!wearable_tfcard_is_ready()) {
        lv_list_add_text(s_gallery.list, get_lan_index() == 1 ? "请插入 T 卡" : "Please insert T card");
        return;
    }

    gallery_scan(s_gallery.tab);
    if (s_gallery.item_count == 0) {
        lv_list_add_text(s_gallery.list, get_lan_index() == 1 ? "暂无文件" : "No files");
        return;
    }

    for (i = 0; i < s_gallery.item_count; i++) {
        lv_obj_t *btn = lv_list_add_button(s_gallery.list, LV_SYMBOL_FILE, s_gallery.items[i].name);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, event_list_item, LV_EVENT_CLICKED, NULL);
    }
}

static void event_tab_switch(lv_event_t *e)
{
    wearable_gallery_type_t tab = (wearable_gallery_type_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);

    gallery_close_preview();
    s_gallery.tab = tab;
    if (btn) {
        lv_obj_t *tabs = lv_obj_get_parent(btn);
        uint32_t i;
        uint32_t cnt = tabs ? lv_obj_get_child_count(tabs) : 0;

        for (i = 0; i < cnt; i++) {
            lv_obj_t *ch = lv_obj_get_child(tabs, (int32_t)i);
            if (!ch)
                continue;
            lv_obj_set_style_text_color(lv_obj_get_child(ch, 0),
                                        lv_color_hex(ch == btn ? 0xffcc00 : 0x888888), 0);
        }
    }
    gallery_fill_list();
}

static lv_obj_t *create_tab_button(lv_obj_t *parent, const char *text, int x, wearable_gallery_type_t tab,
                                   int active)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb;

    lv_obj_set_size(btn, GALLERY_TAB_W, GALLERY_TAB_H);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, event_tab_switch, LV_EVENT_CLICKED, (void *)(intptr_t)tab);
    lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, gallery_tab_font(), 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(active ? 0xffcc00 : 0x888888), 0);
    lv_obj_center(lb);
    return btn;
}

int wearable_camera_gallery_show(lv_obj_t *parent_hide, wearable_gallery_type_t initial_tab,
                             wearable_gallery_close_cb_t on_close, void *user_data)
{
    lv_obj_t *screen;
    lv_obj_t *tabs;
    lv_obj_t *back;
    lv_display_t *disp;
    int scr_h;

    if (s_gallery.root)
        gallery_close_all();

    (void)wearable_tfcard_ensure_init();

    disp = lv_display_get_default();
    scr_h = disp ? (int)lv_display_get_vertical_resolution(disp) : 240;
    screen = lv_display_get_screen_active(disp);
    memset(&s_gallery, 0, sizeof(s_gallery));
    s_gallery.parent_hide = parent_hide;
    s_gallery.tab = initial_tab;
    s_gallery.on_close = on_close;
    s_gallery.user_data = user_data;
    s_gallery.preview_item_idx = -1;
    s_list_block_until_tick = 0;

    if (parent_hide)
        lv_obj_add_flag(parent_hide, LV_OBJ_FLAG_HIDDEN);

    s_gallery.root = lv_obj_create(screen);
    gallery_obj_strip_pad(s_gallery.root);
    lv_obj_set_size(s_gallery.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.root, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(s_gallery.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.root, LV_OBJ_FLAG_SCROLLABLE);

    tabs = lv_obj_create(s_gallery.root);
    gallery_obj_strip_pad(tabs);
    lv_obj_set_size(tabs, lv_pct(100), GALLERY_TOP_H);
    lv_obj_align(tabs, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);

    back = lv_button_create(tabs);
    lv_obj_set_size(back, GALLERY_BACK_W, GALLERY_TOP_H);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_0, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_add_event_cb(back, event_gallery_back, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(back, event_gallery_back, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_t *lb_back = lv_label_create(back);
    lv_label_set_text(lb_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lb_back, lv_color_hex(0xffffff), 0);
    lv_obj_center(lb_back);

    create_tab_button(tabs, get_lan_index() == 1 ? "照片" : "Photos", GALLERY_TAB_PHOTO_X,
                      WEARABLE_GALLERY_PHOTO, initial_tab == WEARABLE_GALLERY_PHOTO);
    create_tab_button(tabs, get_lan_index() == 1 ? "视频" : "Videos", GALLERY_TAB_VIDEO_X,
                      WEARABLE_GALLERY_VIDEO, initial_tab == WEARABLE_GALLERY_VIDEO);

    s_gallery.list = lv_list_create(s_gallery.root);
    /* Do not remove_style_all on lv_list: flex_flow is a style in LVGL 9. */
    lv_obj_set_style_pad_all(s_gallery.list, 0, 0);
    lv_obj_set_style_border_width(s_gallery.list, 0, 0);
    lv_obj_set_flex_flow(s_gallery.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_gallery.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_gallery.list, LV_DIR_VER);
    lv_obj_set_size(s_gallery.list, lv_pct(100), scr_h - GALLERY_TOP_H);
    lv_obj_align(s_gallery.list, LV_ALIGN_TOP_LEFT, 0, GALLERY_TOP_H);
    lv_obj_set_style_bg_color(s_gallery.list, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_gallery.list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_row(s_gallery.list, 2, 0);

    gallery_fill_list();
    return 0;
}

void wearable_camera_gallery_close(void)
{
    if (s_gallery.root)
        gallery_close_all();
}
