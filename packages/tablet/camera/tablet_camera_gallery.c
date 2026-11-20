// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Nova camera gallery: list/delete photos and videos; view JPEG; play MP4.
 */
#define LOG_TAG "TABLET_CAMERA_GALLERY"

#include "tablet_camera_gallery.h"
#include "tablet_camera_nv12.h"
#include "tablet_camera_storage.h"
#include "tablet_camera_capture.h"

#include "tablet_common.h"
#include "tablet_font.h"
#include "app_main.h"
#include "multimedia.h"
#include "bitmap.h"
#include "log.h"
#include "system_memory.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GALLERY_FILE_MAX            128
#define GALLERY_NAME_MAX            64
#define GALLERY_PATH_MAX            320
#define GALLERY_LIST_INPUT_BLOCK_MS 400

/* Palette; accent matches CAM_COLOR_ACTIVE in tablet_camera.c */
#define GALLERY_COLOR_BG            0x101014
#define GALLERY_COLOR_TOPBAR        0x1a1a1f
#define GALLERY_COLOR_CARD          0x26262c
#define GALLERY_COLOR_CARD_PRESSED  0x33333a
#define GALLERY_COLOR_ACCENT        0xffcc00
#define GALLERY_COLOR_TEXT          0xf2f2f4
#define GALLERY_COLOR_TEXT_DIM      0x9a9aa0
#define GALLERY_COLOR_ICON_DIM      0x4a4a52
#define GALLERY_COLOR_BORDER        0x2c2c32

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
    lv_obj_t *preview_bar;
    lv_timer_t *bar_hide_timer;
    lv_timer_t *nav_timer;
    lv_obj_t *play_img;
    lv_timer_t *play_timer;
    tablet_gallery_type_t tab;
    tablet_gallery_close_cb_t on_close;
    void *user_data;
    gallery_item_t items[GALLERY_FILE_MAX];
    int item_count;
    int preview_item_idx;
    multimedia_t *mm;
    media_player_handle_t player;
    int playing;
    int play_pending;
    uint8_t *play_buf;
    uint32_t play_w;
    uint32_t play_h;
    lv_image_dsc_t play_dsc;
    pthread_mutex_t play_lock;
    /* NV12 trailer playback state */
    FILE *nv12_fp;
    nova_preview_footer_t nv12_footer;
    uint64_t nv12_data_offset;
    pthread_t nv12_play_thread;
    volatile int nv12_play_running;
    uint8_t *nv12_raw_buf;  /* raw frame at footer dims; background thread writes here */
    uint8_t *photo_jpg;
    lv_image_dsc_t photo_dsc;
} gallery_ctx_t;

static gallery_ctx_t s_gallery;
static uint32_t s_list_block_until_tick;

static void gallery_fill_list(void);
static void event_preview_delete(lv_event_t *e);
static void event_preview_back(lv_event_t *e);
static void event_list_item(lv_event_t *e);
static void event_gallery_back(lv_event_t *e);
static void event_tab_switch(lv_event_t *e);
static void event_preview_tap(lv_event_t *e);
static void event_preview_prev(lv_event_t *e);
static void event_preview_next(lv_event_t *e);
static void gallery_stop_video_play(void);
static void gallery_preview_controls_show(void);

static int gallery_top_h(void)
{
    return GUI_SCALE_VER(64);
}

static lv_font_t *gallery_font(void)
{
    lv_font_t *font = tablet_font_get_freetype_font(GUI_SCALE(28));

    if (!font)
        font = tablet_font_get_montserrat_font(GUI_SCALE(26));
    return font;
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

static lv_obj_t *gallery_make_solid_btn(lv_obj_t *parent, const char *text, int w, int h,
                                        uint32_t bg, lv_opa_t bg_opa, int radius,
                                        lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb;
    lv_font_t *font = gallery_font();

    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, bg_opa, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* Pill buttons: keep the glyph clear of the rounded ends. */
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, GUI_SCALE(24), 0);
    /* Press feedback: darkens and solidifies on press. */
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_ext_click_area(btn, 10);
    lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xffffff), 0);
    if (font)
        lv_obj_set_style_text_font(lb, font, 0);
    lv_obj_center(lb);
    return btn;
}

static int starts_with_ci(const char *s, const char *prefix)
{
    size_t n;
    size_t i;

    if (!s || !prefix)
        return 0;
    n = strlen(prefix);
    if (strlen(s) < n)
        return 0;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl;
    size_t su;
    size_t i;

    if (!s || !suffix)
        return 0;
    sl = strlen(s);
    su = strlen(suffix);
    if (sl < su)
        return 0;
    for (i = 0; i < su; i++) {
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

static int match_tab(const char *name, tablet_gallery_type_t tab)
{
    if (!name)
        return 0;
    if (tab == TABLET_GALLERY_PHOTO) {
        if (!starts_with_ci(name, TABLET_PHOTO_PREFIX))
            return 0;
        return path_is_jpeg(name);
    }
    if (!starts_with_ci(name, TABLET_VIDEO_PREFIX))
        return 0;
    return path_is_mp4(name);
}

static int cmp_item_desc(const void *a, const void *b)
{
    const gallery_item_t *ia = (const gallery_item_t *)a;
    const gallery_item_t *ib = (const gallery_item_t *)b;

    if (ia->mtime > ib->mtime)
        return -1;
    if (ia->mtime < ib->mtime)
        return 1;
    return 0;
}

static int gallery_frame_is_nv12(const media_frame_pixel_t *frame)
{
    if (!frame || !frame->data[0] || !frame->data[1])
        return 0;
    if (frame->data[2] == NULL)
        return 1;
    /* Interleaved UV uses a full-width stride. Planar U is width/2. */
    return frame->linesize[1] >= frame->width;
}

static void gallery_i420_to_nv12_scaled(const media_frame_pixel_t *frame,
                                        uint8_t *dst, uint32_t dw, uint32_t dh)
{
    uint32_t sw = (uint32_t)frame->width;
    uint32_t sh = (uint32_t)frame->height;
    uint32_t vis_w;
    uint32_t y;
    const uint8_t *ys = frame->data[0];
    const uint8_t *us;
    const uint8_t *vs;
    int y_stride = frame->linesize[0];
    int u_stride;
    int v_stride;
    int nv12 = gallery_frame_is_nv12(frame);
    uint8_t *yd = dst;
    uint8_t *uv = dst + dw * dh;

    if (nv12) {
        us = frame->data[1];
        vs = NULL;
        u_stride = frame->linesize[1] > 0 ? frame->linesize[1] : (int)sw;
        v_stride = 0;
    } else {
        /* Software decoder is I420 (U then V). Hardware NV12 is handled above
         * and matches the live preview, which is already the right order. */
        us = frame->data[1];
        vs = frame->data[2];
        u_stride = frame->linesize[1];
        v_stride = frame->linesize[2];
    }

    if (!ys || !us || sw < 2 || sh < 2 || dw < 2 || dh < 2)
        return;
    if (!nv12 && !vs)
        return;
    if (y_stride <= 0)
        y_stride = (int)sw;
    if (!nv12 && u_stride <= 0)
        u_stride = (int)(sw / 2u);
    if (!nv12 && v_stride <= 0)
        v_stride = (int)(sw / 2u);
    vis_w = sw;

    for (y = 0; y < dh; y++) {
        uint32_t sy = y * sh / dh;
        uint32_t x;

        for (x = 0; x < dw; x++) {
            uint32_t sx = x * vis_w / dw;

            yd[y * dw + x] = ys[sy * (uint32_t)y_stride + sx];
        }
    }
    for (y = 0; y < dh / 2; y++) {
        uint32_t sy = (y * 2) * sh / dh;
        uint32_t x;

        if (sy >= sh)
            sy = sh - 1;
        sy /= 2;
        for (x = 0; x < dw / 2; x++) {
            uint32_t sx = (x * 2) * vis_w / dw;

            if (sx >= sw)
                sx = sw - 1;
            sx /= 2;
            if (nv12) {
                const uint8_t *pair = us + sy * (uint32_t)u_stride + sx * 2u;

                uv[(y * (dw / 2) + x) * 2] = pair[0];
                uv[(y * (dw / 2) + x) * 2 + 1] = pair[1];
            } else {
                uv[(y * (dw / 2) + x) * 2] = us[sy * (uint32_t)u_stride + sx];
                uv[(y * (dw / 2) + x) * 2 + 1] = vs[sy * (uint32_t)v_stride + sx];
            }
        }
    }
}

static void gallery_play_timer_cb(lv_timer_t *t)
{
    (void)t;
    pthread_mutex_lock(&s_gallery.play_lock);
    if (s_gallery.play_pending && s_gallery.play_img && s_gallery.play_buf) {
        s_gallery.play_pending = 0;
        lv_image_set_src(s_gallery.play_img, &s_gallery.play_dsc);
        lv_obj_invalidate(s_gallery.play_img);
    }
    pthread_mutex_unlock(&s_gallery.play_lock);
}

/* Scale NV12 to NV12. Same layout as live preview, so red stays red. */
static void gallery_nv12_scaled(const uint8_t *src, uint32_t sw, uint32_t sh,
                                uint8_t *dst, uint32_t dw, uint32_t dh)
{
    const uint8_t *sy;
    const uint8_t *suv;
    uint8_t *dy;
    uint8_t *duv;
    uint32_t vis_w;
    uint32_t x, y;

    if (!src || !dst || sw < 4 || sh < 2 || dw < 2 || dh < 2)
        return;

    vis_w = sw;

    sy = src;
    suv = src + (size_t)sw * sh;
    dy = dst;
    duv = dst + (size_t)dw * dh;

    for (y = 0; y < dh; y++) {
        uint32_t ry = y * sh / dh;

        for (x = 0; x < dw; x++) {
            uint32_t rx = x * vis_w / dw;

            dy[(size_t)y * dw + x] = sy[(size_t)ry * sw + rx];
        }
    }
    for (y = 0; y < dh / 2u; y++) {
        uint32_t ry = (y * 2u) * sh / dh;

        ry /= 2u;
        for (x = 0; x < dw / 2u; x++) {
            uint32_t rx = (x * 2u) * vis_w / dw;
            const uint8_t *pair;

            rx = (rx / 2u) * 2u;
            pair = suv + (size_t)ry * sw + rx;
            duv[((size_t)y * (dw / 2u) + x) * 2u] = pair[0];
            duv[((size_t)y * (dw / 2u) + x) * 2u + 1u] = pair[1];
        }
    }
}

static void *gallery_nv12_player_thread(void *arg)
{
    uint32_t sleep_us;

    (void)arg;

    while (s_gallery.nv12_play_running) {
        size_t frame_bytes;
        size_t n;

        if (!s_gallery.nv12_fp || !s_gallery.nv12_raw_buf) {
            usleep(20000);
            continue;
        }

        frame_bytes = nova_nv12_frame_size(s_gallery.nv12_footer.width,
                                           s_gallery.nv12_footer.height);
        n = fread(s_gallery.nv12_raw_buf, 1, frame_bytes, s_gallery.nv12_fp);
        if (n < frame_bytes) {
            /* EOF: loop back to first frame */
            fseek(s_gallery.nv12_fp, (long)s_gallery.nv12_data_offset, SEEK_SET);
            n = fread(s_gallery.nv12_raw_buf, 1, frame_bytes, s_gallery.nv12_fp);
            if (n < frame_bytes) {
                usleep(20000);
                continue;
            }
        }

        pthread_mutex_lock(&s_gallery.play_lock);
        if (s_gallery.play_buf && s_gallery.play_w && s_gallery.play_h) {
            gallery_nv12_scaled(s_gallery.nv12_raw_buf,
                                s_gallery.nv12_footer.width, s_gallery.nv12_footer.height,
                                s_gallery.play_buf, s_gallery.play_w, s_gallery.play_h);
            s_gallery.play_pending = 1;
        }
        pthread_mutex_unlock(&s_gallery.play_lock);

        sleep_us = s_gallery.nv12_footer.fps > 0 ?
                   1000000u / s_gallery.nv12_footer.fps : 66666u;
        usleep(sleep_us);
    }

    return NULL;
}

static void gallery_video_show_cb(int play_handle, media_frame_pixel_t *frame, void *user)
{
    (void)play_handle;
    (void)user;
    if (!s_gallery.playing || !frame || frame->data_count < 2)
        return;
    if (!frame->data[0] || !frame->data[1])
        return;
    if (!gallery_frame_is_nv12(frame) && !frame->data[2])
        return;
    if (frame->width <= 0 || frame->height <= 0)
        return;

    pthread_mutex_lock(&s_gallery.play_lock);
    if (s_gallery.play_buf && s_gallery.play_w && s_gallery.play_h) {
        gallery_i420_to_nv12_scaled(frame, s_gallery.play_buf, s_gallery.play_w, s_gallery.play_h);
        s_gallery.play_pending = 1;
    }
    pthread_mutex_unlock(&s_gallery.play_lock);
}

static void gallery_stop_video_play(void)
{
    if (s_gallery.play_timer) {
        lv_timer_delete(s_gallery.play_timer);
        s_gallery.play_timer = NULL;
    }
    s_gallery.playing = 0;
    s_gallery.play_pending = 0;

    /* Stop NV12 background player thread before touching shared state */
    if (s_gallery.nv12_play_running) {
        s_gallery.nv12_play_running = 0;
        pthread_join(s_gallery.nv12_play_thread, NULL);
    }

    if (s_gallery.mm && s_gallery.mm->player && s_gallery.player) {
        s_gallery.mm->player->set_video_show_callback(s_gallery.player, NULL, NULL);
        s_gallery.mm->player->stop(s_gallery.player);
        s_gallery.mm->player->destroy(s_gallery.player);
        s_gallery.player = NULL;
    }
    if (s_gallery.nv12_fp) {
        fclose(s_gallery.nv12_fp);
        s_gallery.nv12_fp = NULL;
    }
    if (s_gallery.nv12_raw_buf) {
        system_free(s_gallery.nv12_raw_buf);
        s_gallery.nv12_raw_buf = NULL;
    }
    memset(&s_gallery.nv12_footer, 0, sizeof(s_gallery.nv12_footer));
    s_gallery.nv12_data_offset = 0;
    if (s_gallery.play_img && lv_obj_is_valid(s_gallery.play_img))
        lv_image_set_src(s_gallery.play_img, NULL);
    if (s_gallery.play_buf) {
        bitmap_buffer_free(s_gallery.play_buf);
        s_gallery.play_buf = NULL;
    }
    memset(&s_gallery.play_dsc, 0, sizeof(s_gallery.play_dsc));
    s_gallery.play_w = 0;
    s_gallery.play_h = 0;
}

static void gallery_close_preview(void)
{
    gallery_stop_video_play();
    s_gallery.preview_item_idx = -1;
    if (s_gallery.nav_timer) {
        lv_timer_delete(s_gallery.nav_timer);
        s_gallery.nav_timer = NULL;
    }
    if (s_gallery.bar_hide_timer) {
        lv_timer_delete(s_gallery.bar_hide_timer);
        s_gallery.bar_hide_timer = NULL;
    }
    if (s_gallery.preview_root) {
        lv_obj_delete(s_gallery.preview_root);
        s_gallery.preview_root = NULL;
        s_gallery.preview_image = NULL;
        s_gallery.preview_bar = NULL;
        s_gallery.play_img = NULL;
    }
    if (s_gallery.photo_jpg) {
        free(s_gallery.photo_jpg);
        s_gallery.photo_jpg = NULL;
    }
    memset(&s_gallery.photo_dsc, 0, sizeof(s_gallery.photo_dsc));
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
    pthread_mutex_destroy(&s_gallery.play_lock);
    memset(&s_gallery, 0, sizeof(s_gallery));
    pthread_mutex_init(&s_gallery.play_lock, NULL);
}

static void preview_close_defer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    gallery_close_preview();
}

static void gallery_schedule_preview_close(void)
{
    lv_timer_t *tm;

    s_list_block_until_tick = lv_tick_get() + GALLERY_LIST_INPUT_BLOCK_MS;
    tm = lv_timer_create(preview_close_defer_cb, 50, NULL);
    if (tm)
        lv_timer_set_repeat_count(tm, 1);
}

static int gallery_scan(tablet_gallery_type_t tab)
{
    char dir[GALLERY_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    const char *subdir = (tab == TABLET_GALLERY_PHOTO) ? TABLET_PHOTO_SUBDIR : TABLET_RECORD_SUBDIR;

    s_gallery.item_count = 0;
    if (!tablet_album_is_ready())
        return 0;
    if (tablet_album_ensure_subdir(subdir, dir, sizeof(dir)) != 0)
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
        if (snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name) >= (int)sizeof(full))
            continue;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        snprintf(s_gallery.items[s_gallery.item_count].path,
                 sizeof(s_gallery.items[s_gallery.item_count].path), "%s", full);
        snprintf(s_gallery.items[s_gallery.item_count].name,
                 sizeof(s_gallery.items[s_gallery.item_count].name), "%s", ent->d_name);
        s_gallery.items[s_gallery.item_count].mtime = st.st_mtime;
        s_gallery.item_count++;
    }
    closedir(d);

    if (s_gallery.item_count > 1)
        qsort(s_gallery.items, (size_t)s_gallery.item_count, sizeof(gallery_item_t), cmp_item_desc);
    return s_gallery.item_count;
}

/* Full-screen transparent control layer: bottom Delete/Back pills + side
 * prev/next arrows. Hidden by default; a tap anywhere shows it for 3 s. */
static lv_obj_t *gallery_create_preview_action_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);

    gallery_obj_strip_pad(bar);
    lv_obj_set_size(bar, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* Tapping the controls layer itself keeps it on screen. */
    lv_obj_add_event_cb(bar, event_preview_tap, LV_EVENT_CLICKED, NULL);
    s_gallery.preview_bar = bar;
    return bar;
}

/* Translucent buttons need a hairline outline to read on bright video. */
static void gallery_style_overlay_btn(lv_obj_t *btn)
{
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
}

/* Round prev/next button. Icon-only: keep the default symbol font (FreeType
 * Chinese.ttf has no FontAwesome range), do not apply gallery_font(). */
static lv_obj_t *gallery_make_icon_btn(lv_obj_t *parent, const char *sym, int size,
                                       lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb;

    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lb = lv_label_create(btn);
    lv_label_set_text(lb, sym);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lb, tablet_font_get_montserrat_font(GUI_SCALE_VER(40)), 0);
    lv_obj_center(lb);
    return btn;
}

static void gallery_add_preview_actions(lv_obj_t *bar)
{
    int btn_w = GUI_SCALE(150);
    int btn_h = GUI_SCALE_VER(52);
    int nav_size = GUI_SCALE_VER(64);
    lv_obj_t *del;
    lv_obj_t *back;

    /* Solid pills: red delete stands out, dark back stays quiet on any frame. */
    del = gallery_make_solid_btn(bar, lan("Delete"), btn_w, btn_h, 0xd93025,
                                 (lv_opa_t)217, btn_h / 2, event_preview_delete);
    gallery_style_overlay_btn(del);
    lv_obj_align(del, LV_ALIGN_BOTTOM_LEFT, GUI_SCALE(12), -TABLET_HOME_BAR_H);

    back = gallery_make_solid_btn(bar, lan("Back"), btn_w, btn_h, 0x1c1c20,
                                  (lv_opa_t)191, btn_h / 2, event_preview_back);
    gallery_style_overlay_btn(back);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -GUI_SCALE(12), -TABLET_HOME_BAR_H);

    /* Side arrows for prev/next; only useful with more than one item. */
    if (s_gallery.item_count > 1) {
        lv_obj_t *prev = gallery_make_icon_btn(bar, LV_SYMBOL_LEFT, nav_size,
                                               event_preview_prev);
        lv_obj_t *next = gallery_make_icon_btn(bar, LV_SYMBOL_RIGHT, nav_size,
                                               event_preview_next);

        lv_obj_align(prev, LV_ALIGN_LEFT_MID, GUI_SCALE(10), 0);
        lv_obj_align(next, LV_ALIGN_RIGHT_MID, -GUI_SCALE(10), 0);
        /* On the first item prev is inert: dim it and make it unpressable. */
        if (s_gallery.preview_item_idx <= 0) {
            lv_obj_set_style_opa(prev, LV_OPA_30, 0);
            lv_obj_clear_flag(prev, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void gallery_preview_bar_hide_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_gallery.bar_hide_timer = NULL;
    if (s_gallery.preview_bar && lv_obj_is_valid(s_gallery.preview_bar))
        lv_obj_add_flag(s_gallery.preview_bar, LV_OBJ_FLAG_HIDDEN);
}

/* Show controls (re)arm the 3 s auto-hide. */
static void gallery_preview_controls_show(void)
{
    if (!s_gallery.preview_bar || !lv_obj_is_valid(s_gallery.preview_bar))
        return;
    lv_obj_remove_flag(s_gallery.preview_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_gallery.bar_hide_timer) {
        lv_timer_reset(s_gallery.bar_hide_timer);
    } else {
        s_gallery.bar_hide_timer = lv_timer_create(gallery_preview_bar_hide_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_gallery.bar_hide_timer, 1);
    }
}

static void event_preview_tap(lv_event_t *e)
{
    (void)e;
    gallery_preview_controls_show();
}

static void gallery_show_photo_preview(const char *path, int idx)
{
    lv_obj_t *img;
    lv_obj_t *action_bar;
    char fs_path[GALLERY_PATH_MAX + 8];
    lv_display_t *disp = lv_display_get_default();

    gallery_close_preview();
    /* Set before the UI builds: the prev/next arrows dim themselves from it. */
    s_gallery.preview_item_idx = idx;

    s_gallery.preview_root = lv_obj_create(lv_display_get_screen_active(disp));
    gallery_obj_strip_pad(s_gallery.preview_root);
    lv_obj_set_size(s_gallery.preview_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.preview_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_gallery.preview_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.preview_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Full-bleed like video playback; CONTAIN letterboxes on black and the
     * action bar floats over the bottom. */
    img = lv_image_create(s_gallery.preview_root);
    s_gallery.preview_image = img;
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);

    {
        void *sub = NULL;
        size_t sub_len = 0;
        unsigned sub_w = 0;
        unsigned sub_h = 0;

        if (tablet_camera_photo_load_sub(path, &sub, &sub_len, &sub_w, &sub_h) == 0 &&
            sub && sub_w > 0 && sub_h > 0) {
            s_gallery.photo_jpg = (uint8_t *)sub;
            memset(&s_gallery.photo_dsc, 0, sizeof(s_gallery.photo_dsc));
            s_gallery.photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_gallery.photo_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
            s_gallery.photo_dsc.header.w = (uint16_t)sub_w;
            s_gallery.photo_dsc.header.h = (uint16_t)sub_h;
            /* RGB888 row stride; without it LVGL mis-addresses every line. */
            s_gallery.photo_dsc.header.stride = (uint16_t)(sub_w * 3u);
            s_gallery.photo_dsc.data = s_gallery.photo_jpg;
            s_gallery.photo_dsc.data_size = (uint32_t)sub_len;
            lv_image_set_src(img, &s_gallery.photo_dsc);
            log_info("gallery: photo sub jpeg %ux%u", sub_w, sub_h);
        } else {
            snprintf(fs_path, sizeof(fs_path), "%c:%s", LV_FS_STDIO_LETTER, path);
            lv_image_set_src(img, fs_path);
        }
    }
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);

    /* Taps anywhere (controls hidden) bring the controls back. */
    lv_obj_add_event_cb(s_gallery.preview_root, event_preview_tap, LV_EVENT_CLICKED, NULL);

    action_bar = gallery_create_preview_action_bar(s_gallery.preview_root);
    gallery_add_preview_actions(action_bar);
    gallery_preview_controls_show();
}

static int gallery_check_nv12_trailer(const char *path,
                                      nova_preview_footer_t *out_footer,
                                      uint64_t *out_data_offset)
{
    FILE *fp;
    long file_size;
    nova_preview_footer_t f;
    uint64_t expected_data;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size < (long)sizeof(nova_preview_footer_t)) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, -(long)sizeof(nova_preview_footer_t), SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    if (fread(&f, 1, sizeof(f), fp) != sizeof(f)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (memcmp(f.magic, NOVA_PREVIEW_MAGIC, 8) != 0)
        return -1;
    if (f.width == 0 || f.height == 0 || f.n_frames == 0 || f.fps == 0)
        return -1;

    expected_data = (uint64_t)f.n_frames * nova_nv12_frame_size(f.width, f.height);
    if (f.data_size != expected_data)
        return -1;
    if ((uint64_t)file_size < f.data_size + sizeof(nova_preview_footer_t))
        return -1;

    *out_footer = f;
    *out_data_offset = (uint64_t)file_size - f.data_size - sizeof(nova_preview_footer_t);
    return 0;
}

/* All-zero NV12 is green; chroma must sit at 128 for black. */
static void gallery_fill_nv12_black(uint8_t *buf, uint32_t w, uint32_t h)
{
    size_t y_size = (size_t)w * (size_t)h;

    memset(buf, 0, y_size);
    memset(buf + y_size, 128, y_size / 2u);
}

/* DMA NV12 so the GPU draws it. The LVGL software blender rejects NV12. */
static int gallery_bind_nv12_view(uint32_t dw, uint32_t view_h)
{
    size_t bytes = (size_t)dw * (size_t)view_h * 3u / 2u;

    s_gallery.play_buf = (uint8_t *)bitmap_buffer_alloc((uint32_t)bytes);
    if (!s_gallery.play_buf)
        return -1;
    gallery_fill_nv12_black(s_gallery.play_buf, dw, view_h);
    s_gallery.play_w = dw;
    s_gallery.play_h = view_h;
    s_gallery.play_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_gallery.play_dsc.header.cf = LV_COLOR_FORMAT_NV12;
    s_gallery.play_dsc.header.w = (uint16_t)dw;
    s_gallery.play_dsc.header.h = (uint16_t)view_h;
    s_gallery.play_dsc.header.stride = (uint16_t)dw;
    s_gallery.play_dsc.data = s_gallery.play_buf;
    s_gallery.play_dsc.data_size = (uint32_t)bytes;
    if (!s_gallery.play_img) {
        s_gallery.play_img = lv_image_create(s_gallery.preview_root);
        lv_obj_set_size(s_gallery.play_img, lv_pct(100), (int)view_h);
        lv_obj_align(s_gallery.play_img, LV_ALIGN_TOP_MID, 0, 0);
    }
    lv_image_set_src(s_gallery.play_img, &s_gallery.play_dsc);
    /* Video frame is created after the action bar; keep the bar (and its
     * translucent buttons) composited on top of the playing video. */
    if (s_gallery.preview_bar && lv_obj_is_valid(s_gallery.preview_bar))
        lv_obj_move_foreground(s_gallery.preview_bar);
    return 0;
}

static int gallery_start_video_play(const char *path)
{
    lv_display_t *disp = lv_display_get_default();
    uint32_t dw = disp ? (uint32_t)lv_display_get_horizontal_resolution(disp) : (uint32_t)LV_HOR_RES;
    uint32_t dh = disp ? (uint32_t)lv_display_get_vertical_resolution(disp) : (uint32_t)LV_VER_RES;
    uint32_t view_h;
    nova_preview_footer_t footer;
    uint64_t nv12_offset;
    lv_font_t *font;

    if (!path)
        return -1;

    gallery_stop_video_play();

    /* Full-bleed video; the translucent action bar floats over the bottom. */
    view_h = dh;
    dw    &= ~1u;
    view_h &= ~1u;
    if (dw < 2 || view_h < 2)
        return -1;

    if (!s_gallery.preview_root)
        return -1;

    if (gallery_check_nv12_trailer(path, &footer, &nv12_offset) == 0) {
        /* Has NOVAPREV trailer: background thread reads+scales NV12 frames,
         * gallery_play_timer_cb (reused) flips play_pending on LVGL thread. */
        size_t raw_bytes;

        if (footer.width < 2 || footer.height < 2)
            goto no_preview;
        if (gallery_bind_nv12_view(dw, view_h) != 0)
            goto no_preview;

        /* raw_buf at footer dimensions for background thread reads */
        raw_bytes = nova_nv12_frame_size(footer.width, footer.height);
        s_gallery.nv12_raw_buf = (uint8_t *)system_malloc(raw_bytes);
        if (!s_gallery.nv12_raw_buf) {
            gallery_stop_video_play();
            goto no_preview;
        }

        s_gallery.nv12_fp = fopen(path, "rb");
        if (!s_gallery.nv12_fp) {
            gallery_stop_video_play();
            goto no_preview;
        }
        fseek(s_gallery.nv12_fp, (long)nv12_offset, SEEK_SET);
        s_gallery.nv12_footer      = footer;
        s_gallery.nv12_data_offset = nv12_offset;

        s_gallery.playing          = 1;
        s_gallery.nv12_play_running = 1;
        if (pthread_create(&s_gallery.nv12_play_thread, NULL,
                           gallery_nv12_player_thread, NULL) != 0) {
            s_gallery.nv12_play_running = 0;
            gallery_stop_video_play();
            goto no_preview;
        }
        /* Reuse gallery_play_timer_cb: it checks play_pending set by the thread */
        s_gallery.play_timer = lv_timer_create(gallery_play_timer_cb, 33, NULL);

        log_info("gallery: NV12 trailer %ux%u @%ufps %u frames offset=%llu display=%ux%u",
                 footer.width, footer.height, footer.fps, footer.n_frames,
                 (unsigned long long)nv12_offset, dw, view_h);
        return 0;
    }

    /* New recordings store preview_bg H.264 as video track 1. Do not fall back to the 8MP track. */
    if (!s_gallery.mm || !s_gallery.mm->player || !s_gallery.mm->player->create ||
        !s_gallery.mm->player->set_video_track || !s_gallery.mm->player->set_input_file ||
        !s_gallery.mm->player->prepare || !s_gallery.mm->player->start) {
        goto no_preview;
    }
    {
        int pret;

        if (gallery_bind_nv12_view(dw, view_h) != 0)
            goto no_preview;

        pret = s_gallery.mm->player->create(&s_gallery.player);
        if (pret != 0 || !s_gallery.player)
            goto player_fail;
        pret = s_gallery.mm->player->set_input_file(s_gallery.player, path);
        if (pret != 0)
            goto player_fail;
        pret = s_gallery.mm->player->set_video_track(s_gallery.player, 1);
        if (pret != 0)
            goto player_fail;
        s_gallery.playing = 1;
        pret = s_gallery.mm->player->prepare(s_gallery.player);
        if (pret != 0)
            goto player_fail;
        pret = s_gallery.mm->player->start(s_gallery.player);
        if (pret != 0)
            goto player_fail;
        pret = s_gallery.mm->player->set_video_show_callback(s_gallery.player,
                                                            gallery_video_show_cb, NULL);
        if (pret != 0)
            goto player_fail;
        s_gallery.play_timer = lv_timer_create(gallery_play_timer_cb, 33, NULL);
        log_info("gallery: decode sub H.264 track %s display=%ux%u", path, dw, view_h);
        return 0;

player_fail:
        log_warn("gallery: sub H.264 track unavailable in %s", path);
        gallery_stop_video_play();
    }

no_preview:
    /* Old NOVAPREV files already returned above. Track 1 missing or decode failed. */
    font = gallery_font();
    {
        lv_obj_t *msg = lv_label_create(s_gallery.preview_root);

        lv_label_set_text(msg, lan("Preview unavailable"));
        lv_obj_set_width(msg, lv_pct(80));
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, -GUI_SCALE_VER(8));
        lv_obj_set_style_text_color(msg, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        if (font)
            lv_obj_set_style_text_font(msg, font, 0);
    }
    log_info("gallery: preview unavailable for %s", path);
    return 0;
}

static void gallery_show_video_preview(const char *path, const char *name, int idx)
{
    lv_obj_t *title;
    lv_obj_t *action_bar;
    lv_display_t *disp = lv_display_get_default();
    lv_font_t *font = gallery_font();

    gallery_close_preview();
    /* Set before the UI builds: the prev/next arrows dim themselves from it. */
    s_gallery.preview_item_idx = idx;

    s_gallery.preview_root = lv_obj_create(lv_display_get_screen_active(disp));
    gallery_obj_strip_pad(s_gallery.preview_root);
    lv_obj_set_size(s_gallery.preview_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.preview_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_gallery.preview_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.preview_root, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(s_gallery.preview_root);
    lv_label_set_text(title, name ? name : "");
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, GUI_SCALE_VER(16));
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    if (font)
        lv_obj_set_style_text_font(title, font, 0);

    /* Taps anywhere (controls hidden) bring the controls back. */
    lv_obj_add_event_cb(s_gallery.preview_root, event_preview_tap, LV_EVENT_CLICKED, NULL);

    action_bar = gallery_create_preview_action_bar(s_gallery.preview_root);
    gallery_add_preview_actions(action_bar);

    /* Tap-to-view plays immediately, same as photo preview. */
    if (gallery_start_video_play(path) != 0)
        show_toast_note(lan("Play failed"), 2000, 1);
    /* Playback view is created after the title; keep the name visible. */
    if (title && lv_obj_is_valid(title))
        lv_obj_move_foreground(title);
    gallery_preview_controls_show();
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
    gallery_stop_video_play();
    if (unlink(path) != 0) {
        show_toast_note(lan("Delete failed"), 2000, 1);
        return;
    }

    gallery_schedule_preview_close();
    gallery_fill_list();
}

static void event_preview_back(lv_event_t *e)
{
    (void)e;
    gallery_schedule_preview_close();
}

/* Reopen the preview on the neighbour item. Deferred: the tap lands on a
 * button inside preview_root, and the reopen deletes that subtree. */
static void preview_nav_defer_cb(lv_timer_t *t)
{
    int idx;

    lv_timer_delete(t);
    s_gallery.nav_timer = NULL;
    idx = s_gallery.preview_item_idx;
    if (idx < 0 || idx >= s_gallery.item_count)
        return;
    if (s_gallery.tab == TABLET_GALLERY_PHOTO)
        gallery_show_photo_preview(s_gallery.items[idx].path, idx);
    else
        gallery_show_video_preview(s_gallery.items[idx].path, s_gallery.items[idx].name, idx);
}

static void gallery_preview_nav(int dir)
{
    int n = s_gallery.item_count;
    int idx = s_gallery.preview_item_idx + dir;

    if (n < 2 || s_gallery.preview_item_idx < 0 || s_gallery.preview_item_idx >= n)
        return;
    /* Stop at the ends: past the last item only a hint, not a wrap. */
    if (idx < 0 || idx >= n) {
        show_toast_note(lan("Last one"), 1500, 0);
        return;
    }
    s_gallery.preview_item_idx = idx;
    if (s_gallery.nav_timer)
        return;
    s_gallery.nav_timer = lv_timer_create(preview_nav_defer_cb, 30, NULL);
    lv_timer_set_repeat_count(s_gallery.nav_timer, 1);
}

static void event_preview_prev(lv_event_t *e)
{
    (void)e;
    gallery_preview_nav(-1);
}

static void event_preview_next(lv_event_t *e)
{
    (void)e;
    gallery_preview_nav(1);
}

static void event_list_item(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    if (lv_tick_get() < s_list_block_until_tick)
        return;
    if (idx < 0 || idx >= s_gallery.item_count)
        return;

    /* idx goes in before the UI builds: the prev/next arrows dim themselves
     * from preview_item_idx while the action bar is created. */
    if (s_gallery.tab == TABLET_GALLERY_PHOTO)
        gallery_show_photo_preview(s_gallery.items[idx].path, idx);
    else
        gallery_show_video_preview(s_gallery.items[idx].path, s_gallery.items[idx].name, idx);
}

static void gallery_add_empty_state(lv_obj_t *parent, const char *text)
{
    lv_obj_t *wrap;
    lv_obj_t *col;
    lv_obj_t *icon;
    lv_obj_t *lb;
    lv_font_t *font_big = tablet_font_get_montserrat_font(GUI_SCALE(48));
    lv_font_t *font = gallery_font();

    /* Flex-column list needs a full-size wrapper to center the content. */
    wrap = lv_obj_create(parent);
    gallery_obj_strip_pad(wrap);
    lv_obj_set_size(wrap, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    col = lv_obj_create(wrap);
    gallery_obj_strip_pad(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -GUI_SCALE_VER(24));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, GUI_SCALE_VER(12), 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    icon = lv_label_create(col);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(icon, lv_color_hex(GALLERY_COLOR_ICON_DIM), 0);
    if (font_big)
        lv_obj_set_style_text_font(icon, font_big, 0);

    lb = lv_label_create(col);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, lv_color_hex(GALLERY_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    if (font)
        lv_obj_set_style_text_font(lb, font, 0);
}

static lv_obj_t *gallery_create_item_card(lv_obj_t *parent, const gallery_item_t *item,
                                          int idx, int is_video)
{
    lv_obj_t *card;
    lv_obj_t *tile;
    lv_obj_t *icon;
    lv_obj_t *col;
    lv_obj_t *name;
    lv_obj_t *date;
    lv_obj_t *chev;
    lv_font_t *font = gallery_font();
    lv_font_t *font_small = tablet_font_get_freetype_font(GUI_SCALE(22));
    struct tm tmv;
    char tbuf[32];

    card = lv_button_create(parent);
    lv_obj_set_size(card, lv_pct(100), GUI_SCALE_VER(76));
    lv_obj_set_style_radius(card, GUI_SCALE(16), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(GALLERY_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(GALLERY_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, GUI_SCALE(12), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(GALLERY_COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, event_list_item, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(card, (void *)(intptr_t)idx);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    tile = lv_obj_create(card);
    gallery_obj_strip_pad(tile);
    lv_obj_set_size(tile, GUI_SCALE_VER(52), GUI_SCALE_VER(52));
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(tile, GUI_SCALE(12), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(GALLERY_COLOR_CARD_PRESSED), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    icon = lv_label_create(tile);
    lv_label_set_text(icon, is_video ? LV_SYMBOL_PLAY : LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(icon, lv_color_hex(GALLERY_COLOR_ACCENT), 0);
    lv_obj_center(icon);

    col = lv_obj_create(card);
    gallery_obj_strip_pad(col);
    lv_obj_set_size(col, GUI_SCALE(520), LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, GUI_SCALE_VER(52) + GUI_SCALE(14), 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, GUI_SCALE_VER(4), 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    name = lv_label_create(col);
    lv_label_set_text(name, item->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_color(name, lv_color_hex(GALLERY_COLOR_TEXT), 0);
    if (font)
        lv_obj_set_style_text_font(name, font, 0);

    date = lv_label_create(col);
    if (localtime_r(&item->mtime, &tmv))
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tmv);
    else
        tbuf[0] = '\0';
    lv_label_set_text(date, tbuf);
    lv_label_set_long_mode(date, LV_LABEL_LONG_DOT);
    lv_obj_set_width(date, lv_pct(100));
    lv_obj_set_style_text_color(date, lv_color_hex(GALLERY_COLOR_TEXT_DIM), 0);
    if (font_small)
        lv_obj_set_style_text_font(date, font_small, 0);

    chev = lv_label_create(card);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(GALLERY_COLOR_TEXT_DIM), 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -GUI_SCALE(4), 0);
    lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);

    return card;
}

static void gallery_fill_list(void)
{
    int i;
    int is_video = (s_gallery.tab == TABLET_GALLERY_VIDEO);

    if (!s_gallery.list)
        return;
    lv_obj_clean(s_gallery.list);

    if (!tablet_album_is_ready()) {
        gallery_add_empty_state(s_gallery.list, lan("Cannot save"));
        return;
    }

    gallery_scan(s_gallery.tab);
    if (s_gallery.item_count == 0) {
        gallery_add_empty_state(s_gallery.list, lan("No files"));
        return;
    }

    for (i = 0; i < s_gallery.item_count; i++)
        gallery_create_item_card(s_gallery.list, &s_gallery.items[i], i, is_video);
}

static void event_gallery_back(lv_event_t *e)
{
    (void)e;
    gallery_close_all();
}

static void event_tab_switch(lv_event_t *e)
{
    tablet_gallery_type_t tab = (tablet_gallery_type_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);

    gallery_close_preview();
    s_gallery.tab = tab;
    if (btn) {
        lv_obj_t *tabs = lv_obj_get_parent(btn);
        uint32_t i;
        uint32_t cnt = tabs ? lv_obj_get_child_count(tabs) : 0;

        for (i = 0; i < cnt; i++) {
            lv_obj_t *ch = lv_obj_get_child(tabs, (int32_t)i);
            lv_obj_t *lb;
            lv_obj_t *ind;
            int active = (ch == btn);

            /* Skip non-tab buttons (e.g. Back) in the top bar. */
            if (!ch || lv_obj_get_user_data(ch) == NULL)
                continue;
            lb = lv_obj_get_child(ch, 0);
            if (lb)
                lv_obj_set_style_text_color(lb, lv_color_hex(active ?
                                            GALLERY_COLOR_ACCENT : 0x888888), 0);
            ind = lv_obj_get_child(ch, 1);
            if (ind)
                lv_obj_set_style_bg_opa(ind, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
    gallery_fill_list();
}

static lv_obj_t *create_tab_button(lv_obj_t *parent, const char *text, int x,
                                   tablet_gallery_type_t tab, int active)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb;
    lv_obj_t *ind;
    lv_font_t *font = gallery_font();

    lv_obj_set_size(btn, GUI_SCALE(160), gallery_top_h());
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, event_tab_switch, LV_EVENT_CLICKED, (void *)(intptr_t)tab);
    /* Tag tab buttons so event_tab_switch can skip other top-bar buttons.
     * +1: TABLET_GALLERY_PHOTO is 0 and a NULL tag would be skipped. */
    lv_obj_set_user_data(btn, (void *)(intptr_t)(tab + 1));
    lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    if (font)
        lv_obj_set_style_text_font(lb, font, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(active ?
                                GALLERY_COLOR_ACCENT : 0x888888), 0);
    lv_obj_center(lb);
    /* Active-tab underline indicator, toggled in event_tab_switch. */
    ind = lv_obj_create(btn);
    gallery_obj_strip_pad(ind);
    lv_obj_set_size(ind, GUI_SCALE(56), GUI_SCALE_VER(4));
    lv_obj_align(ind, LV_ALIGN_BOTTOM_MID, 0, -GUI_SCALE_VER(6));
    lv_obj_set_style_radius(ind, GUI_SCALE_VER(2), 0);
    lv_obj_set_style_bg_color(ind, lv_color_hex(GALLERY_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(ind, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ind, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

int tablet_camera_gallery_show(lv_obj_t *parent_hide, tablet_gallery_type_t initial_tab,
                               tablet_gallery_close_cb_t on_close, void *user_data)
{
    lv_obj_t *screen;
    lv_obj_t *tabs;
    lv_obj_t *back;
    lv_display_t *disp;
    int scr_h;

    if (s_gallery.root)
        gallery_close_all();

    disp = lv_display_get_default();
    scr_h = disp ? (int)lv_display_get_vertical_resolution(disp) : LV_VER_RES;
    screen = lv_display_get_screen_active(disp);
    memset(&s_gallery, 0, sizeof(s_gallery));
    pthread_mutex_init(&s_gallery.play_lock, NULL);
    s_gallery.parent_hide = parent_hide;
    s_gallery.tab = initial_tab;
    s_gallery.on_close = on_close;
    s_gallery.user_data = user_data;
    s_gallery.preview_item_idx = -1;
    s_gallery.mm = (multimedia_t *)app_main_get_multimedia();
    s_list_block_until_tick = 0;

    if (parent_hide)
        lv_obj_add_flag(parent_hide, LV_OBJ_FLAG_HIDDEN);

    s_gallery.root = lv_obj_create(screen);
    gallery_obj_strip_pad(s_gallery.root);
    lv_obj_set_size(s_gallery.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_gallery.root, lv_color_hex(GALLERY_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_gallery.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gallery.root, LV_OBJ_FLAG_SCROLLABLE);

    tabs = lv_obj_create(s_gallery.root);
    gallery_obj_strip_pad(tabs);
    lv_obj_set_size(tabs, lv_pct(100), gallery_top_h());
    lv_obj_align(tabs, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_hex(GALLERY_COLOR_TOPBAR), 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(GALLERY_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);

    back = gallery_make_solid_btn(tabs, lan("Back"), GUI_SCALE(120), GUI_SCALE_VER(48),
                                  0x3a3a3c, LV_OPA_60, GUI_SCALE_VER(24), event_gallery_back);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, GUI_SCALE(12), 0);

    create_tab_button(tabs, lan("Photos"), GUI_SCALE(150), TABLET_GALLERY_PHOTO,
                      initial_tab == TABLET_GALLERY_PHOTO);
    create_tab_button(tabs, lan("Videos"), GUI_SCALE(330), TABLET_GALLERY_VIDEO,
                      initial_tab == TABLET_GALLERY_VIDEO);

    s_gallery.list = lv_list_create(s_gallery.root);
    lv_obj_set_style_pad_all(s_gallery.list, GUI_SCALE(12), 0);
    lv_obj_set_style_pad_row(s_gallery.list, GUI_SCALE(10), 0);
    lv_obj_set_style_border_width(s_gallery.list, 0, 0);
    lv_obj_set_flex_flow(s_gallery.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_gallery.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_gallery.list, LV_DIR_VER);
    lv_obj_set_size(s_gallery.list, lv_pct(100), scr_h - gallery_top_h());
    lv_obj_align(s_gallery.list, LV_ALIGN_TOP_LEFT, 0, gallery_top_h());
    lv_obj_set_style_bg_color(s_gallery.list, lv_color_hex(GALLERY_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_gallery.list, LV_OPA_COVER, 0);

    gallery_fill_list();
    return 0;
}

void tablet_camera_gallery_close(void)
{
    if (s_gallery.root)
        gallery_close_all();
}
