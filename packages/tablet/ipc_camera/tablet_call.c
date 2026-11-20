/*
 * App-local call entry implementation (no src include wrapper).
 *
 * Nova screen camera, "ambient standby + tap-to-reveal" UI (see
 * docs/design/nova/anypi_nova_screencam_ui.md):
 *  - IDLE: full-screen background carousel (digital clock page, analog clock
 *          page, photo-album slideshow page), swipe left/right to switch;
 *          pipeline keeps only Nebula discovery.
 *  - Tap reveals a translucent bottom action bar; hides after 3 s idle.
 *  - Incoming call (PC CMD_ReqMediaTalk, Ve posted on the "net2ui" queue):
 *          Answer / Hang-up overlay; reject stops talk decode + audio session.
 *  - CALL: remote 800x480 video full screen; tap reveals the Stop/Hang-up bar.
 *  - Long-press (IDLE only) exits to home. No back button anywhere.
 */
#define LOG_TAG "TABLET_CALL"

#include "tablet_call.h"
#include "config.h"
#include "tablet_common.h"
#include "tablet_font.h"
#include "app_main.h"
#include "log.h"
#include "multimedia.h"
#include "network.h"
#include "nebula_protocol.h"
#include "notification.h"
#include "rtsp_protocol.h"
#include "bitmap.h"
#include "system_memory.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int media_player_set_video_show_callback(int play_handle, media_player_video_show_callback_t callback, void *user);

#ifdef CONFIG_APP_TABLET_CAMERA
#include "tablet_video_ctrl.h"
#endif

#ifdef CONFIG_APP_TABLET_IPC_CAMERA

/* ---- theme (docs/design/nova/anypi_nova_screencam_ui.md §7) ---- */
#define IPC_COLOR_BG        0x0B0E14
#define IPC_COLOR_TEXT      0xF4F7FB
#define IPC_COLOR_TEXT_DIM  0xC5D0DC
#define IPC_COLOR_PRIMARY   0x18A058
#define IPC_COLOR_DANGER    0xD64545
#define IPC_COLOR_BTN_IDLE  0x2E2E2E

/* ---- layout (800x480 reference, GUI_SCALE for 1024x600) ---- */
#define IPC_STATUS_H        GUI_SCALE_VER(40)
#define IPC_BAR_W_PCT       92
#define IPC_BAR_H           GUI_SCALE_VER(96)
/* Dots sit at the very bottom; the bar floats just above them. */
#define IPC_BAR_Y_OFF       (-GUI_SCALE_VER(44))
#define IPC_BTN_W           GUI_SCALE(210)
#define IPC_BTN_H           GUI_SCALE_VER(68)
#define IPC_RING_BTN_SIZE   GUI_SCALE_VER(96)
#define IPC_DOTS_Y_OFF      (-GUI_SCALE_VER(14))
#define IPC_DOT_SIZE        GUI_SCALE(10)
#define IPC_DOTS_GAP        GUI_SCALE(12)

#define IPC_REVEAL_TIMEOUT_MS 3000u
#define IPC_FADE_MS           300u
/* Swipe feel: dragging ~15% of a page width turns the page (LVGL snap alone
 * needed ~50%); SCROLL_END snaps manually from the live drag position. */
#define IPC_SWIPE_TURN_PCT    15

/* Standby carousel: page 0 digital clock, page 1 analog clock, page 2 album slideshow. */
#define IPC_PAGE_DIGITAL    0
#define IPC_PAGE_ANALOG     1
#define IPC_PAGE_ALBUM      2
#define IPC_BG_PAGE_COUNT   3
#define IPC_BG_REL_FMT      "image/screencam/bg_%d.qoi"
#define IPC_SLIDE_MAX       6
#define IPC_SLIDE_INTERVAL_MS 8000u
#define IPC_DIGIT_FONT_SIZE 96
#define IPC_SEC_FONT_SIZE   40
#define IPC_SWIPE_LIMIT_PX  4   /* indev scroll limit (default 10): swipe turns earlier */

#define IPC_CALL_PERF_TRACE 1
#define IPC_CALL_ENABLE_FASTPATH 0
#ifndef IPC_CALL_DISABLE_VIDEO_DECODE
#define IPC_CALL_DISABLE_VIDEO_DECODE 0
#endif

typedef enum {
    IPC_MODE_IDLE = 0,
    IPC_MODE_CALL = 1
} ipc_mode_t;

typedef struct {
    lv_obj_t *main;
    lv_obj_t *window;
    /* standby carousel */
    lv_obj_t *carousel;
    lv_obj_t *page_clock;
    /* analog clock face (restored from the clock app) */
    lv_obj_t *page_analog;
    lv_obj_t *analog_scale;
    lv_obj_t *needle_hour;
    lv_obj_t *needle_min;
    lv_obj_t *needle_sec;
    /* album slideshow page */
    lv_obj_t *page_album;
    lv_obj_t *slide_img;
    char slide_paths[IPC_SLIDE_MAX][384];
    int slide_cnt;
    int slide_idx;
    uint32_t last_slide_ms;
    int cur_page;
    lv_obj_t *dots_box;
    lv_obj_t *dots[IPC_BG_PAGE_COUNT];
    lv_obj_t *time_label;
    lv_obj_t *sec_label;
    lv_obj_t *date_label;
    /* fullscreen surface for CALL (RGB24 remote video) */
    lv_obj_t *preview_area;
    lv_obj_t *preview_img;
    /* top status bar (CALL only) */
    lv_obj_t *status_bar;
    lv_obj_t *status_left;
    lv_obj_t *status_right;
    /* bottom translucent action bar */
    lv_obj_t *op_bar;
    lv_obj_t *btn_call;
    lv_obj_t *btn_hangup;
    /* incoming-call overlay (Answer / Hang-up, visible while ringing) */
    lv_obj_t *ring_box;
    lv_obj_t *ring_halo;
    lv_obj_t *btn_accept;
    lv_obj_t *btn_reject;
    int ringing;
    /* timers */
    lv_timer_t *ui_tick_timer;   /* 500 ms: clock, call duration, bar auto-hide */
    lv_timer_t *photo_timer;     /* one-shot deferred QOI load */
    /* call soft-decode */
    lv_image_dsc_t call_dsc;
    void *call_buf;
    uint64_t call_buf_paddr;
    void *call_src_buf[2];
    uint64_t call_src_paddr[2];
    uint32_t call_src_buf_size;
    uint32_t call_src_w;
    uint32_t call_src_h;
    volatile int call_src_ready_idx;
    int call_src_write_idx;
    lv_timer_t *call_timer;
    uint32_t call_wait_t0_ms;
    int call_wait_hint_shown;
    /* mode / state */
    ipc_mode_t mode;
    int call_on;
    int ai_on;
    nova_af_focus_ui_t af_focus_ui;
    /* ui_tick state */
    int op_bar_visible;
    uint32_t op_bar_reveal_ms;
    uint32_t last_clock_update_ms;
    int call_connected;
    uint32_t call_connect_ms;
} ipc_call_ctx_t;

static ipc_call_ctx_t s_ctx;
static int s_call_enable;
static int s_server_connect_state;
static int s_talk_out_posted;
static multimedia_t *s_mm;
static volatile int s_call_has_frame;
static nebula_protocol_t *s_nebula;
static int s_rtsp_inited;
static rtsp_protocol_t *s_rtsp;
static volatile int s_rtsp_playing;
static volatile int s_nebula_linked;
static lv_timer_t *s_transport_timer;
static volatile int s_in_window_delete_cb;
/* Set during LV_EVENT_DELETE teardown so capture/player/LVGL callbacks bail before s_ctx is cleared. */
static volatile int s_ipc_teardown;
typedef struct {
    rtsp_protocol_ops_t *ops;
} rtsp_protocol_public_view_t;

static void call_stop(void);
static void switch_mode(ipc_mode_t new_mode);
static void call_config_enable_nebula_main(void);
static void ipc_ensure_nebula_started(void);
static void ipc_nebula_stop(void);
static void ipc_rtsp_stop(void);
static int call_render_start(void);
static void call_render_stop(void);
static void ipc_surface_click_cb(lv_event_t *e);
static void ipc_ring_show(int on);
static void ipc_update_analog_clock(void);
static void ipc_album_advance(void);
static void event_handler_btn_accept(lv_event_t *e);
static void event_handler_btn_reject(lv_event_t *e);
/* Implemented in nebula_common.c (gateway/nebula_protocol). */
int nebula_talk_audio_session_ensure_started(void);
void nebula_talk_audio_session_release(void);
void nebula_talk_video_decode_set_enabled(int enable);

/* ============================ pipeline (kept) ============================ */

static int ipc_call_prepare_src_buffers(ipc_call_ctx_t *ctx, uint32_t need)
{
    if (!ctx)
        return -1;
    if (ctx->call_src_buf_size >= need &&
        ctx->call_src_buf[0] && ctx->call_src_buf[1] &&
        ctx->call_src_paddr[0] != 0 && ctx->call_src_paddr[1] != 0) {
        return 0;
    }

    for (int i = 0; i < 2; i++) {
        if (ctx->call_src_buf[i]) {
            bitmap_buffer_free(ctx->call_src_buf[i]);
            ctx->call_src_buf[i] = NULL;
            ctx->call_src_paddr[i] = 0;
        }
        ctx->call_src_buf[i] = bitmap_buffer_alloc(need);
        if (!ctx->call_src_buf[i] || bitmap_buffer_paddr(ctx->call_src_buf[i], &ctx->call_src_paddr[i]) != 0) {
            if (ctx->call_src_buf[i]) {
                bitmap_buffer_free(ctx->call_src_buf[i]);
                ctx->call_src_buf[i] = NULL;
            }
            ctx->call_src_paddr[i] = 0;
            return -1;
        }
    }

    ctx->call_src_buf_size = need;
    ctx->call_src_ready_idx = -1;
    ctx->call_src_write_idx = 0;
    return 0;
}

static notification_t *ipc_get_notification(void)
{
    return (notification_t *)app_main_get_notification();
}

static int ipc_post_talk_out(int enable)
{
    notification_t *notification = ipc_get_notification();
    if (notification == NULL || notification->message == NULL || notification->message->post == NULL) {
        log_warn("ipc: post talk_out skipped (notification unavailable)");
        return -1;
    }

    int value = enable ? 1 : 0;
    (void)notification->message->post(MESSAGE_MANAGER_TYPE_TALK_OUT, (char *)&value, (int)sizeof(value));
    log_debug("ipc: post talk_out value=%d", value);
    return 0;
}

static void ipc_sync_talk_out(int enable)
{
    int target = enable ? 1 : 0;
    if (s_talk_out_posted == target)
        return;
    if (ipc_post_talk_out(target) == 0)
        s_talk_out_posted = target;
}

void framework_rtsp_on_play_state(int playing, const char *source_token)
{
    (void)source_token;
    s_rtsp_playing = playing ? 1 : 0;
}

void framework_nebula_on_link_state(int linked, const char *server_ip, int server_port)
{
    (void)server_ip;
    (void)server_port;
    s_nebula_linked = linked ? 1 : 0;
}

static void ipc_transport_arbitrate_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_ipc_teardown)
        return;
    /* Coexist policy (revised per screencam UI design §3):
     * - Nebula discovery stays up in every mode (LAN tools see the device).
     * - RTSP is owned by the PREVIEW mode (started/stopped on mode switches),
     *   never restarted here.
     * - This timer only performs lightweight keepalive/recovery. */
    if (!s_nebula) {
        ipc_ensure_nebula_started();
        call_config_enable_nebula_main();
    }
}

static void ipc_ensure_nebula_started(void)
{
    if (s_nebula)
        return;

    /* Nebula must NOT be started in app_main. Start it on-demand in IPC camera app. */
    nebula_protocol_t *nebula = NULL;
    if (nebula_protocol_open(&nebula) != NEBULA_PROTOCOL_OK || nebula == NULL) {
        log_warn("ipc: nebula protocol open failed");
        return;
    }

    /* Screen camera is a video-call product: drop file transfer, AI, alarm,
     * log subscription and face DB from the private protocol, and hide the
     * audio/video parameter HTTP config groups (stubs anyway). Must run
     * before network/http init. */
    if (nebula_protocol_set_capability(NEBULA_CAP_ALL
            & ~(NEBULA_CAP_FILE_TRANSFER | NEBULA_CAP_AI | NEBULA_CAP_ALARM
                | NEBULA_CAP_LOG | NEBULA_CAP_FACE_RECOGNIZE
                | NEBULA_CAP_HTTP_VIDEO_PARAM | NEBULA_CAP_HTTP_AUDIO)) != NEBULA_PROTOCOL_OK)
        log_warn("ipc: nebula set_capability failed, keep default (all on)");

    if (nebula->http) {
        if (nebula->http->init)
            nebula->http->init();
        if (nebula->http->start)
            nebula->http->start();
    }

    if (nebula->network) {
        if (nebula->network->init)
            nebula->network->init();
        if (nebula->network->start)
            nebula->network->start();
    }

    s_nebula = nebula;
    app_main_register_nebula(nebula);
    log_info("ipc: nebula started");
}

static void ipc_nebula_stop(void)
{
    nebula_protocol_t *nebula = s_nebula;
    if (!nebula)
        return;

    if (nebula->http) {
        if (nebula->http->stop)
            nebula->http->stop();
        if (nebula->http->deinit)
            nebula->http->deinit();
    }
    if (nebula->network) {
        if (nebula->network->stop)
            nebula->network->stop();
        if (nebula->network->deinit)
            nebula->network->deinit();
    }

    app_main_register_nebula(NULL);
    (void)nebula_protocol_close(nebula);
    s_nebula = NULL;
    log_info("ipc: nebula stopped");
}

static void ipc_rtsp_stop(void)
{
    if (!s_rtsp_inited)
        return;

    if (s_rtsp) {
        /* rtsp_protocol_close() will internally stop the server if initialized.
         * Do NOT call ops->server->stop() here, otherwise the server teardown
         * may run twice and crash (double stop on RtspLisRev thread). */
        (void)rtsp_protocol_close(s_rtsp);
        s_rtsp = NULL;
    }

    s_rtsp_inited = 0;
    log_info("ipc: rtsp deinit");
}

/* IPC camera "camera config" stage: enable Nebula main data stream early.
 * We keep this ON while toggling between PREVIEW/CALL inside the app.
 * Only disable it when leaving the app. */
static void call_config_enable_nebula_main(void)
{
#if CONFIG_NETWORK_SUPPORTED
    if (!ipc_call_network_ready()) {
        /* No hard UI message here; preview may still work, but main stream may not. */
        log_warn("ipc: config enable nebula main skipped (network not ready)");
        return;
    }
#endif
}

/* ============================ network / IP ============================ */

static int ipc_ipv4_usable(const char *ip)
{
    return ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

/**
 * Read IPv4 from kernel ifaddrs (shell udhcpc may own wlan0 without FW station CONNECTED).
 * Same idea as tablet_home.c tablet_iface_has_valid_ip.
 */
static int ipc_iface_ipv4(const char *iface, char *ip, size_t ip_len)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    int ok = -1;

    if (!iface || !ip || ip_len == 0)
        return -1;
    ip[0] = '\0';
    if (getifaddrs(&ifaddr) != 0)
        return -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, iface) != 0)
            continue;
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr == 0 || sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            continue;
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, (socklen_t)ip_len) != NULL &&
            ipc_ipv4_usable(ip)) {
            ok = 0;
        }
        break;
    }

    freeifaddrs(ifaddr);
    return ok;
}

/** Fill local IPv4 used for PC tools to connect. Returns 0 on success. */
static int ipc_get_local_ipv4(char *ip, size_t ip_len)
{
#if CONFIG_NETWORK_SUPPORTED
    network_t *net = (network_t *)app_main_get_network();
    static const char *ifaces[] = {
        "wlan0", "eth0", "eth1", "net0", "enp0s0",
    };
    unsigned i;

    if (!ip || ip_len == 0)
        return -1;
    ip[0] = '\0';

    if (net) {
        /* wifi->get_ip uses iface ioctl; does not require station CONNECTED. */
        if (net->wifi && net->wifi->get_ip &&
            net->wifi->get_ip(ip) == NETWORK_OK && ipc_ipv4_usable(ip))
            return 0;

        if (net->wifi && net->wifi->station) {
            network_wifi_station_state_e state;
            if (net->wifi->station->get_state(&state) == NETWORK_OK &&
                state == NETWORK_WIFI_STATION_STATE_CONNECTED) {
                if (net->wifi->station->get_ip_address(ip, ip_len) == NETWORK_OK &&
                    ipc_ipv4_usable(ip)) {
                    return 0;
                }
            }
        }

        if (net->ethernet) {
            if (net->ethernet->get_ip && net->ethernet->get_ip(ip) == NETWORK_OK &&
                ipc_ipv4_usable(ip))
                return 0;
            if (net->ethernet->get_ip_by_name) {
                for (i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
                    memset(ip, 0, ip_len);
                    if (net->ethernet->get_ip_by_name(ifaces[i], ip, ip_len) == NETWORK_OK &&
                        ipc_ipv4_usable(ip)) {
                        return 0;
                    }
                }
            }
        }
    }

    /* Kernel fallback when FW WiFi/Ethernet HAL has no IP yet. */
    for (i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        if (ipc_iface_ipv4(ifaces[i], ip, ip_len) == 0)
            return 0;
    }
    return -1;
#else
    (void)ip;
    (void)ip_len;
    return -1;
#endif
}

int ipc_call_network_ready(void)
{

#if CONFIG_NETWORK_SUPPORTED
    char ip[64] = {0};

    if (ipc_get_local_ipv4(ip, sizeof(ip)) == 0) {
        log_info("ipc: ipv4 usable: %s", ip);
        return 1;
    }
    log_error("ipc: ipv4 not usable");
    return 0;
#else
    return 1;
#endif
}

/* ============================ status bar ============================ */

static void set_status_text(const char *text)
{
    if (s_ctx.status_left) {
        lv_label_set_text(s_ctx.status_left, text ? text : "");
        if (tablet_font_get_freetype_font(20))
            lv_obj_set_style_text_font(s_ctx.status_left, tablet_font_get_freetype_font(20), 0);
    }
}

/** Right side of the status bar: local IPv4, or "Not connected". */
static void ipc_update_ip_label(void)
{
    char ip[64] = {0};

    if (!s_ctx.status_right)
        return;
    if (ipc_get_local_ipv4(ip, sizeof(ip)) == 0)
        lv_label_set_text(s_ctx.status_right, ip);
    else
        lv_label_set_text(s_ctx.status_right, lan("Not connected"));
    if (tablet_font_get_freetype_font(20))
        lv_obj_set_style_text_font(s_ctx.status_right, tablet_font_get_freetype_font(20), 0);
}

static void set_call_wait_status_with_ip(void)
{
    char ip[64] = {0};
    char buf[160];

    if (ipc_get_local_ipv4(ip, sizeof(ip)) == 0) {
        snprintf(buf, sizeof(buf), "%s · %s", lan("Waiting for peer"), ip);
    } else {
        snprintf(buf, sizeof(buf), "%s", lan("Waiting for network IP..."));
    }
    set_status_text(buf);
}

static void ipc_set_call_duration_status(void)
{
    char buf[96];
    uint32_t sec = (lv_tick_get() - s_ctx.call_connect_ms) / 1000u;

    snprintf(buf, sizeof(buf), "%s %02u:%02u", lan("In call"),
             (unsigned)(sec / 60u), (unsigned)(sec % 60u));
    set_status_text(buf);
}

/* ============================ op bar reveal / auto-hide ============================ */

static void ipc_opa_anim_exec_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;

    if (obj && lv_obj_is_valid(obj))
        lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void ipc_bar_fade_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;

    if (obj && lv_obj_is_valid(obj) && !s_ctx.op_bar_visible)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void ipc_animate_bar_opa(int32_t target, int hide_when_done)
{
    lv_obj_t *bar = s_ctx.op_bar;

    if (!bar || !lv_obj_is_valid(bar))
        return;
    lv_anim_delete(bar, ipc_opa_anim_exec_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_values(&a, (int32_t)lv_obj_get_style_opa(bar, 0), target);
    lv_anim_set_duration(&a, IPC_FADE_MS);
    lv_anim_set_exec_cb(&a, ipc_opa_anim_exec_cb);
    if (hide_when_done)
        lv_anim_set_ready_cb(&a, ipc_bar_fade_ready_cb);
    lv_anim_start(&a);
}

static void ipc_reveal_op_bar(void)
{
    if (!s_ctx.op_bar || s_ipc_teardown)
        return;
    s_ctx.op_bar_visible = 1;
    s_ctx.op_bar_reveal_ms = lv_tick_get();
    lv_obj_clear_flag(s_ctx.op_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ctx.op_bar);
    ipc_animate_bar_opa(LV_OPA_COVER, 0);
}

static void ipc_hide_op_bar(void)
{
    if (!s_ctx.op_bar || !s_ctx.op_bar_visible)
        return;
    s_ctx.op_bar_visible = 0;
    ipc_animate_bar_opa(0, 1);
}

/** Click on any non-button surface (standby pages, preview/call video). */
static void ipc_surface_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    if (s_ipc_teardown || !s_ctx.op_bar || !lv_obj_is_valid(s_ctx.op_bar))
        return;
    if (!s_ctx.op_bar_visible) {
        ipc_reveal_op_bar();
    } else {
        /* Already visible: restart the idle countdown. */
        s_ctx.op_bar_reveal_ms = lv_tick_get();
    }
}

/** Shared exit path: show Home, then tear the window down. */
static void ipc_exit_to_home(void)
{
    lv_display_t *disp;

    /* Show Home before teardown. */
    if (s_ctx.main && lv_obj_is_valid(s_ctx.main))
        lv_obj_remove_flag(s_ctx.main, LV_OBJ_FLAG_HIDDEN);
    if (s_ctx.window && lv_obj_is_valid(s_ctx.window))
        lv_obj_add_flag(s_ctx.window, LV_OBJ_FLAG_HIDDEN);
    disp = lv_display_get_default();
    if (disp)
        lv_refr_now(disp);
    if (s_ctx.window && lv_obj_is_valid(s_ctx.window)) {
        lv_obj_delete(s_ctx.window);
        s_ctx.window = NULL;
    }
}

/** Long-press on the standby carousel exits the app (no back button, design §2).
 * A hold-still in the middle of a swipe also raises LONG_PRESSED, so ignore the
 * event while the finger is dragging the carousel or its throw is animating. */
static void ipc_longpress_cb(lv_event_t *e)
{
    lv_indev_t *indev;

    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED)
        return;
    if (s_ipc_teardown || s_ctx.mode != IPC_MODE_IDLE)
        return;
    indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev))
        return; /* finger is on the carousel mid-drag (even holding still) */
    if (s_ctx.carousel && lv_obj_is_scrolling(s_ctx.carousel))
        return; /* a previous flick's scroll throw is still animating */

    log_info("ipc: long-press exit to home");
    ipc_exit_to_home();
}

/** Home indicator (tap / swipe up at the bottom) exits like long-press. */
static void ipc_home_bar_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_ipc_teardown || s_ctx.mode != IPC_MODE_IDLE)
        return;

    log_info("ipc: home bar exit to home");
    ipc_exit_to_home();
}

/* ============================ standby carousel ============================ */

/** Resolve an app-local resource (APPS_SD_BASE_DIR first, then APPS_BASE_DIR, then shared roots). */
static int ipc_resolve_res_path(const char *rel_name, char *out, size_t out_len)
{
    char try_path[384];

    if (!rel_name || !out || out_len == 0)
        return -1;
    out[0] = '\0';

#ifdef APPS_SD_BASE_DIR
    snprintf(try_path, sizeof(try_path), "%s/ipc_camera/res/%s", APPS_SD_BASE_DIR, rel_name);
    if (access(try_path, F_OK) == 0) {
        snprintf(out, out_len, "%s", try_path);
        return 0;
    }
#endif
#ifdef APPS_BASE_DIR
    snprintf(try_path, sizeof(try_path), "%s/ipc_camera/res/%s", APPS_BASE_DIR, rel_name);
    if (access(try_path, F_OK) == 0) {
        snprintf(out, out_len, "%s", try_path);
        return 0;
    }
#endif
    return tablet_resolve_image_path(rel_name, out, out_len);
}

/** One-shot deferred load after the first frame (design §8: nothing in the first
 * frame). Probes the bundled slideshow images (image/screencam/bg_1..N.qoi). */
static void ipc_photo_load_timer_cb(lv_timer_t *timer)
{
    char rel[64];
    char fs_path[384];
    char lv_path[420];

    (void)timer;
    if (s_ipc_teardown) {
        s_ctx.photo_timer = NULL;
        lv_timer_delete(timer);
        return;
    }
    for (int i = 0; i < IPC_SLIDE_MAX; i++) {
        snprintf(rel, sizeof(rel), IPC_BG_REL_FMT, i + 1);
        if (ipc_resolve_res_path(rel, fs_path, sizeof(fs_path)) != 0)
            continue;
        snprintf(s_ctx.slide_paths[s_ctx.slide_cnt], sizeof(s_ctx.slide_paths[0]),
                 "%s", fs_path);
        s_ctx.slide_cnt++;
    }
    if (s_ctx.slide_cnt > 0 && s_ctx.slide_img && lv_obj_is_valid(s_ctx.slide_img)) {
        snprintf(lv_path, sizeof(lv_path), "%c:%s", LV_FS_STDIO_LETTER, s_ctx.slide_paths[0]);
        lv_image_set_src(s_ctx.slide_img, lv_path);
        s_ctx.slide_idx = 0;
    } else {
        log_warn("ipc: album slideshow has no sources (image/screencam/bg_N.qoi)");
    }
    s_ctx.photo_timer = NULL;
    lv_timer_delete(timer);
}

static void ipc_update_clock_page(void)
{
    time_t rawtime;
    struct tm *timeinfo;
    static const char *wd_cn[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const char *wd_en[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char buf[64];

    time(&rawtime);
    /* Wall clock via TZ (e.g. CST-8); same reading as tablet_clock.c. */
    timeinfo = localtime(&rawtime);
    if (!timeinfo || !s_ctx.time_label)
        return;

    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    lv_label_set_text(s_ctx.time_label, buf);

    if (s_ctx.sec_label) {
        lv_label_set_text_fmt(s_ctx.sec_label, "%02d", timeinfo->tm_sec);
        /* Re-glue to the hero time: text width changes as digits change. */
        lv_obj_update_layout(s_ctx.time_label);
        lv_obj_align_to(s_ctx.sec_label, s_ctx.time_label,
                        LV_ALIGN_OUT_RIGHT_BOTTOM, GUI_SCALE(8), 0);
    }

    if (s_ctx.date_label) {
        int cn = (get_lan_index() != 0);
        snprintf(buf, sizeof(buf), "%02d/%02d %s",
                 timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 cn ? wd_cn[timeinfo->tm_wday] : wd_en[timeinfo->tm_wday]);
        lv_label_set_text(s_ctx.date_label, buf);
    }
}

/** Swipe handling: LVGL's built-in snap needs ~50% of a page and settles
 * before SCROLL_END, so with snap disabled we snap manually at SCROLL_END:
 * the page turns once the drag passes IPC_SWIPE_TURN_PCT of the page width. */
static void ipc_carousel_scroll_cb(lv_event_t *e)
{
    int32_t w = (int32_t)LV_HOR_RES;
    int32_t x;
    int target;

    if (lv_event_get_code(e) != LV_EVENT_SCROLL_END)
        return;
    if (!s_ctx.carousel || !lv_obj_is_valid(s_ctx.carousel))
        return;

    x = lv_obj_get_scroll_x(s_ctx.carousel);
    target = s_ctx.cur_page;
    if (x - (int32_t)target * w > w * IPC_SWIPE_TURN_PCT / 100)
        target++;
    else if ((int32_t)target * w - x > w * IPC_SWIPE_TURN_PCT / 100)
        target--;
    if (target < 0)
        target = 0;
    if (target > IPC_BG_PAGE_COUNT - 1)
        target = IPC_BG_PAGE_COUNT - 1;
    s_ctx.cur_page = target;
    s_ctx.last_slide_ms = lv_tick_get();
    if ((int32_t)target * w != x)
        lv_obj_scroll_to_x(s_ctx.carousel, (int32_t)target * w, LV_ANIM_ON);

    for (int i = 0; i < IPC_BG_PAGE_COUNT; i++) {
        if (s_ctx.dots[i] && lv_obj_is_valid(s_ctx.dots[i]))
            lv_obj_set_style_bg_opa(s_ctx.dots[i], (i == s_ctx.cur_page) ? LV_OPA_90 : LV_OPA_30, 0);
    }
}

/* ============================ analog clock / album slideshow ============================ */

/** Build an LVGL fs path for a clock asset (clock_bg/clock_h/clock_m/clock_s .qoi). */
static void ipc_clock_asset_path(char *out, size_t out_len, const char *name)
{
    char rel[96];
    char fs_path[384];

    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    snprintf(rel, sizeof(rel), "image/" GUI_IMAGE_RES_PREFIX "/clock/%s", name);
    if (ipc_resolve_res_path(rel, fs_path, sizeof(fs_path)) != 0) {
        log_warn("ipc: clock asset missing: %s", rel);
        return;
    }
    snprintf(out, out_len, "%c:%s", LV_FS_STDIO_LETTER, fs_path);
}

/** Move the analog hands (same mapping as the clock app: hour*5 + min/12). */
static void ipc_update_analog_clock(void)
{
    time_t rawtime;
    struct tm *timeinfo;
    int hour;

    if (!s_ctx.analog_scale || !lv_obj_is_valid(s_ctx.analog_scale))
        return;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (!timeinfo)
        return;
    hour = timeinfo->tm_hour >= 12 ? timeinfo->tm_hour - 12 : timeinfo->tm_hour;
    hour = hour * 5 + timeinfo->tm_min / 12;
    if (s_ctx.needle_sec)
        lv_scale_set_image_needle_value(s_ctx.analog_scale, s_ctx.needle_sec, timeinfo->tm_sec);
    if (s_ctx.needle_min)
        lv_scale_set_image_needle_value(s_ctx.analog_scale, s_ctx.needle_min, timeinfo->tm_min);
    if (s_ctx.needle_hour)
        lv_scale_set_image_needle_value(s_ctx.analog_scale, s_ctx.needle_hour, hour);
}

/** Advance the album slideshow by one picture. */
static void ipc_album_advance(void)
{
    char lv_path[420];

    if (s_ctx.slide_cnt <= 0 || !s_ctx.slide_img || !lv_obj_is_valid(s_ctx.slide_img))
        return;
    s_ctx.slide_idx = (s_ctx.slide_idx + 1) % s_ctx.slide_cnt;
    snprintf(lv_path, sizeof(lv_path), "%c:%s", LV_FS_STDIO_LETTER,
             s_ctx.slide_paths[s_ctx.slide_idx]);
    lv_image_set_src(s_ctx.slide_img, lv_path);
}

/* ============================ incoming call (PC ReqMediaTalk) ============================ */

/** Ring pulse on the Answer button: ripple ring growing out of the button edge.
 *  Deliberately NOT transform_scale — object-level scale creates an LVGL layer
 *  whose fb blend path faults on this BSP (invalid-rect log spam per frame). */
#define IPC_RING_PULSE_OPA 180

static void ipc_ring_pulse_exec_cb(void *var, int32_t v)
{
    lv_obj_t *halo = (lv_obj_t *)var;

    if (!halo || !lv_obj_is_valid(halo))
        return;
    int32_t base = IPC_RING_BTN_SIZE;
    int32_t size = base + (base / 3) * v / 100;
    lv_obj_set_size(halo, size, size);
    lv_obj_set_style_border_opa(halo, (lv_opa_t)(IPC_RING_PULSE_OPA * (100 - v) / 100), 0);
}

static void ipc_ring_pulse_start(lv_obj_t *halo)
{
    lv_anim_t a;

    if (!halo || !lv_obj_is_valid(halo))
        return;
    lv_anim_delete(halo, ipc_ring_pulse_exec_cb);
    lv_anim_init(&a);
    lv_anim_set_var(&a, halo);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, ipc_ring_pulse_exec_cb);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void ipc_ring_pulse_stop(lv_obj_t *halo)
{
    if (!halo)
        return;
    lv_anim_delete(halo, ipc_ring_pulse_exec_cb);
    if (lv_obj_is_valid(halo)) {
        ipc_ring_pulse_exec_cb(halo, 0);
        lv_obj_set_style_border_opa(halo, 0, 0);
    }
}

/** Show/hide the incoming-call overlay. While ringing the bar auto-hide pauses. */
static void ipc_ring_show(int on)
{
    s_ctx.ringing = on ? 1 : 0;
    if (s_ctx.ring_box && lv_obj_is_valid(s_ctx.ring_box)) {
        if (on) {
            lv_obj_clear_flag(s_ctx.ring_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ctx.ring_box);
            ipc_ring_pulse_start(s_ctx.ring_halo);
        } else {
            ipc_ring_pulse_stop(s_ctx.ring_halo);
            lv_obj_add_flag(s_ctx.ring_box, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Status prompt while ringing; IDLE has no status bar otherwise. */
    if (s_ctx.status_bar && lv_obj_is_valid(s_ctx.status_bar)) {
        if (on) {
            set_status_text(lan("Incoming call"));
            lv_obj_clear_flag(s_ctx.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ctx.status_bar);
        } else if (s_ctx.mode == IPC_MODE_IDLE) {
            lv_obj_add_flag(s_ctx.status_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (on && s_ctx.op_bar_visible)
        ipc_hide_op_bar();
}

/** Ve from the framework "net2ui" queue: 1 = PC requested talk (incoming),
 * 0 = peer cancelled (while ringing) or hung up (while in call). */
static void ipc_handle_incoming(int ve)
{
    if (s_ipc_teardown || !s_ctx.window || !lv_obj_is_valid(s_ctx.window))
        return;
    log_info("ipc: incoming call event Ve=%d (mode=%d ringing=%d)",
             ve, (int)s_ctx.mode, s_ctx.ringing);
    if (ve) {
        if (s_ctx.mode == IPC_MODE_CALL)
            return; /* Already in a call. */
        ipc_ring_show(1);
    } else {
        if (s_ctx.ringing)
            ipc_ring_show(0);
        else if (s_ctx.mode == IPC_MODE_CALL)
            switch_mode(IPC_MODE_IDLE); /* Peer hung up. */
    }
}

/** Drain the framework "net2ui" queue. (tablet_home's reader is dead code —
 * no lv_timer_create references it — so the app polls the queue itself.) */
static void ipc_read_net2ui(void)
{
    notification_t *notification = ipc_get_notification();
    int enable = 0;
    int size = (int)sizeof(enable);
    long long msg_time;
    int handle;

    if (!notification || !notification->queue)
        return;
    handle = notification->queue->create("net2ui");
    if (handle <= 0)
        return;
    if (notification->queue->read(handle, (char *)&enable, &size, &msg_time) == 0)
        ipc_handle_incoming(enable);
}

/* ============================ call frames (kept) ============================ */

static void ipc_call_video_show_cb(int play_handle, media_frame_pixel_t *frame, void *user)
{
    (void)play_handle;
    if (s_ipc_teardown)
        return;
    ipc_call_ctx_t *ctx = (ipc_call_ctx_t *)user;
    static unsigned s_cb_seen;
    static unsigned s_cb_drop;
    static unsigned s_copy_path_seen;
#if IPC_CALL_PERF_TRACE
    static unsigned s_perf_fallback_cnt;
    static uint32_t s_perf_fallback_cb_cost_sum;
    uint32_t cb_t0 = lv_tick_get();
#endif
    if (!ctx || !ctx->call_on || !frame || !ctx->call_buf)
        return;
    if (s_cb_seen < 8u)
        s_cb_seen++;
    if (frame->width <= 0 || frame->height <= 0 || frame->data_count < 3)
    {
        if (s_cb_drop < 8u)
            s_cb_drop++;
        return;
    }
    if (!frame->data[0] || !frame->data[1] || !frame->data[2])
    {
        if (s_cb_drop < 8u)
            s_cb_drop++;
        return;
    }

    uint8_t *src_base = NULL;
    uint32_t src_w = (uint32_t)frame->width;
    uint32_t src_h = (uint32_t)frame->height;
    uint32_t src_y_size = src_w * src_h;
    uint32_t src_uv_h = src_h / 2u;
    uint32_t src_uv_w = src_w / 2u;
    uint32_t src_uv_size = src_uv_w * src_uv_h;
    uint32_t src_need = src_y_size + src_uv_size * 2u;

    if (src_w == 0u || src_h == 0u || (src_w & 1u) != 0u || (src_h & 1u) != 0u) {
        if (s_cb_drop < 8u)
            s_cb_drop++;
        return;
    }

    if (s_copy_path_seen < 6u)
        s_copy_path_seen++;

    if (ipc_call_prepare_src_buffers(ctx, src_need) != 0) {
        if (s_cb_drop < 8u)
            s_cb_drop++;
        return;
    }
    src_base = (uint8_t *)ctx->call_src_buf[ctx->call_src_write_idx];
    {
        int ls0 = (frame->linesize[0] > 0) ? frame->linesize[0] : (int)src_w;
        int ls1 = (frame->linesize[1] > 0) ? frame->linesize[1] : (int)src_uv_w;
        int ls2 = (frame->linesize[2] > 0) ? frame->linesize[2] : (int)src_uv_w;
        uint8_t *dst_y = src_base;
        uint8_t *dst_u = dst_y + src_y_size;
        uint8_t *dst_v = dst_u + src_uv_size;
        uint8_t *src_y = frame->data[0];
        uint8_t *src_u = frame->data[1];
        uint8_t *src_v = frame->data[2];

        if (!src_y || !src_u || !src_v) {
            if (s_cb_drop < 8u)
                s_cb_drop++;
            return;
        }

        if (ls0 == (int)src_w) {
            memcpy(dst_y, src_y, src_y_size);
        } else {
            for (uint32_t y = 0; y < src_h; y++) {
                memcpy(dst_y + y * src_w, src_y + (uint32_t)ls0 * y, src_w);
            }
        }

        if (ls1 == (int)src_uv_w) {
            memcpy(dst_u, src_u, src_uv_size);
        } else {
            for (uint32_t y = 0; y < src_uv_h; y++) {
                memcpy(dst_u + y * src_uv_w, src_u + (uint32_t)ls1 * y, src_uv_w);
            }
        }

        if (ls2 == (int)src_uv_w) {
            memcpy(dst_v, src_v, src_uv_size);
        } else {
            for (uint32_t y = 0; y < src_uv_h; y++) {
                memcpy(dst_v + y * src_uv_w, src_v + (uint32_t)ls2 * y, src_uv_w);
            }
        }
    }
    ctx->call_src_w = src_w;
    ctx->call_src_h = src_h;
    ctx->call_src_ready_idx = ctx->call_src_write_idx;
    ctx->call_src_write_idx ^= 1;
    s_call_has_frame = 1;
#if IPC_CALL_PERF_TRACE
    {
        uint32_t cb_cost = (uint32_t)(lv_tick_get() - cb_t0);
        s_perf_fallback_cnt++;
        s_perf_fallback_cb_cost_sum += cb_cost;
    }
#endif
}

static void ipc_call_timer_cb(lv_timer_t *timer)
{
    if (s_ipc_teardown)
        return;
    ipc_call_ctx_t *ctx = (ipc_call_ctx_t *)lv_timer_get_user_data(timer);
    static unsigned s_scale_fail_cnt;
    if (!ctx || !ctx->call_on || !ctx->preview_img || !ctx->call_buf)
        return;
    if (!s_call_has_frame) {
        if (!ctx->call_wait_hint_shown) {
            uint32_t now_ms = lv_tick_get();
            if ((uint32_t)(now_ms - ctx->call_wait_t0_ms) >= 3000u) {
                set_call_wait_status_with_ip();
                ctx->call_wait_hint_shown = 1;
            }
        }
        return;
    }
    int ready_idx = ctx->call_src_ready_idx;
    uint32_t src_w = ctx->call_src_w;
    uint32_t src_h = ctx->call_src_h;
    if (ready_idx >= 0 && ready_idx <= 1 &&
        ctx->call_src_buf[ready_idx] && ctx->call_src_paddr[ready_idx] != 0 &&
        src_w > 0u && src_h > 0u) {
        bitmap_info_t src = {0};
        bitmap_info_t dst = {0};
        uint32_t src_y_size = src_w * src_h;
        uint32_t src_uv_size = (src_w / 2u) * (src_h / 2u);

        src.u32Width = src_w;
        src.u32Height = src_h;
        src.rect.x = 0;
        src.rect.y = 0;
        src.rect.width = src.u32Width;
        src.rect.height = src.u32Height;
        src.vaddr = (uint8_t *)ctx->call_src_buf[ready_idx];
        src.paddr = ctx->call_src_paddr[ready_idx];
        src.format = SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12;
        src.u8GlobalOpacity = 0xFF;
        src.addrs_cnt = 3;
        src.vaddrs[0] = src.vaddr;
        src.vaddrs[1] = src.vaddr + src_y_size;
        src.vaddrs[2] = src.vaddrs[1] + src_uv_size;
        src.paddrs[0] = src.paddr;
        src.paddrs[1] = src.paddr + src_y_size;
        src.paddrs[2] = src.paddrs[1] + src_uv_size;

        dst.u32Width = (uint32_t)LV_HOR_RES;
        dst.u32Height = (uint32_t)LV_VER_RES;
        dst.rect.x = 0;
        dst.rect.y = 0;
        dst.rect.width = dst.u32Width;
        dst.rect.height = dst.u32Height;
        dst.vaddr = (uint8_t *)ctx->call_buf;
        dst.paddr = ctx->call_buf_paddr;
        dst.format = SYSTEM_PIXEL_FORMAT_RGB24;
        dst.u8GlobalOpacity = 0xFF;

        if (bitmap_scale(&src, &dst) != 0) {
            if (s_scale_fail_cnt < 8u)
                s_scale_fail_cnt++;
            return;
        }
    }
    s_call_has_frame = 0;
    if (!ctx->call_connected) {
        ctx->call_connected = 1;
        ctx->call_connect_ms = lv_tick_get();
        ipc_set_call_duration_status();
    }
    ctx->call_wait_hint_shown = 0;
    ctx->call_dsc.data = (const uint8_t *)ctx->call_buf;
    lv_image_set_src(ctx->preview_img, &ctx->call_dsc);
    lv_obj_invalidate(ctx->preview_area);
}

/* ============================ call render lifecycle ============================ */

static int call_render_start(void)
{
    uint32_t w = (uint32_t)LV_HOR_RES;
    uint32_t h = (uint32_t)LV_VER_RES;
    uint32_t size = w * h * 3u;

    if (!s_ctx.preview_area)
        return -1;

    if (!s_ctx.preview_img) {
        s_ctx.preview_img = lv_image_create(s_ctx.preview_area);
        lv_obj_set_size(s_ctx.preview_img, LV_PCT(100), LV_PCT(100));
        lv_obj_center(s_ctx.preview_img);
        lv_obj_add_flag(s_ctx.preview_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_ctx.preview_img, ipc_surface_click_cb, LV_EVENT_CLICKED, NULL);
    }

    s_ctx.call_buf = bitmap_buffer_alloc(size);
    if (!s_ctx.call_buf)
        return -1;
    if (bitmap_buffer_paddr(s_ctx.call_buf, &s_ctx.call_buf_paddr) != 0) {
        bitmap_buffer_free(s_ctx.call_buf);
        s_ctx.call_buf = NULL;
        s_ctx.call_buf_paddr = 0;
        return -1;
    }
    memset(s_ctx.call_buf, 0, size);
    s_ctx.call_src_buf[0] = NULL;
    s_ctx.call_src_buf[1] = NULL;
    s_ctx.call_src_paddr[0] = 0;
    s_ctx.call_src_paddr[1] = 0;
    s_ctx.call_src_buf_size = 0;
    s_ctx.call_src_w = 0;
    s_ctx.call_src_h = 0;
    s_ctx.call_src_ready_idx = -1;
    s_ctx.call_src_write_idx = 0;

    s_ctx.call_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ctx.call_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_ctx.call_dsc.header.w = (uint16_t)w;
    s_ctx.call_dsc.header.h = (uint16_t)h;
    s_ctx.call_dsc.header.stride = (uint16_t)(w * 3u);
    s_ctx.call_dsc.data = (const uint8_t *)s_ctx.call_buf;
    s_ctx.call_dsc.data_size = size;

    s_ctx.call_timer = lv_timer_create(ipc_call_timer_cb, 1000 / 15, (void *)&s_ctx);
    if (!s_ctx.call_timer) {
        bitmap_buffer_free(s_ctx.call_buf);
        s_ctx.call_buf = NULL;
        return -1;
    }

    s_call_has_frame = 0;
    s_ctx.call_on = 1;
    s_ctx.call_connected = 0;
    s_ctx.call_connect_ms = 0;
    s_ctx.call_wait_t0_ms = lv_tick_get();
    s_ctx.call_wait_hint_shown = 0;
    return 0;
}

static void call_render_stop(void)
{
    if (!s_ctx.call_on && !s_ctx.call_timer && !s_ctx.call_buf)
        return;
    s_ctx.call_on = 0;
    s_call_has_frame = 0;
    s_ctx.call_connected = 0;
    s_ctx.call_connect_ms = 0;
    s_ctx.call_wait_t0_ms = 0;
    s_ctx.call_wait_hint_shown = 0;
    if (s_ctx.call_timer) {
        lv_timer_delete(s_ctx.call_timer);
        s_ctx.call_timer = NULL;
    }
    if (s_ctx.preview_img && lv_obj_is_valid(s_ctx.preview_img)) {
        lv_image_set_src(s_ctx.preview_img, NULL);
    }
    if (s_ctx.call_buf) {
        bitmap_buffer_free(s_ctx.call_buf);
        s_ctx.call_buf = NULL;
        s_ctx.call_buf_paddr = 0;
    }
    for (int i = 0; i < 2; i++) {
        if (s_ctx.call_src_buf[i]) {
            bitmap_buffer_free(s_ctx.call_src_buf[i]);
            s_ctx.call_src_buf[i] = NULL;
        }
        s_ctx.call_src_paddr[i] = 0;
    }
    s_ctx.call_src_buf_size = 0;
    s_ctx.call_src_w = 0;
    s_ctx.call_src_h = 0;
    s_ctx.call_src_ready_idx = -1;
    s_ctx.call_src_write_idx = 0;
    if (!s_in_window_delete_cb && s_ctx.preview_img) {
        if (lv_obj_is_valid(s_ctx.preview_img))
            lv_obj_delete(s_ctx.preview_img);
        s_ctx.preview_img = NULL;
    } else if (s_in_window_delete_cb) {
        s_ctx.preview_img = NULL;
    }
}

static void ipc_window_delete_event_cb(lv_event_t *e)
{
    (void)e;
    /* 先停流与回调，再关 RTSP/相机 HAL，最后清零上下文 — 与备份分支一致。 */
    s_ipc_teardown = 1;
    s_in_window_delete_cb = 1;
    log_info("[IPC-TEARDOWN] window delete: begin");
    call_stop();
    /* Stop arbitration first so this timer cannot restart Nebula while we tear down. */
    if (s_transport_timer) {
        lv_timer_delete(s_transport_timer);
        s_transport_timer = NULL;
    }
    /* Delete UI timers before the object tree goes away. */
    if (s_ctx.ui_tick_timer) {
        lv_timer_delete(s_ctx.ui_tick_timer);
        s_ctx.ui_tick_timer = NULL;
    }
    if (s_ctx.photo_timer) {
        lv_timer_delete(s_ctx.photo_timer);
        s_ctx.photo_timer = NULL;
    }
    /* 1) Nebula off: link thread uses multimedia_open(), avoid racing camera unreg. */
    ipc_nebula_stop();
    if (!s_mm)
        s_mm = (multimedia_t *)app_main_get_multimedia();
    /* 2) Local preview mode was removed; only CALL can be active here.
     * Do NOT call teardown_ai_hw / remove_camera_ai here while AI capture threads are
     * still feeding frames — that races HAL ops and historically faulted at
     * disable_camera_ai_tile_overlap (NULL+0xb8). Backup: rtsp → OSD → release.
     */
    s_ctx.mode = IPC_MODE_IDLE;
    if (s_ctx.main && lv_obj_is_valid(s_ctx.main)) {
        lv_obj_remove_flag(s_ctx.main, LV_OBJ_FLAG_HIDDEN);
    }
    /* 3) RTSP next (may disable HAL channels for live sessions). */
    ipc_rtsp_stop();
    /*
     * 3b) Audio back to boot-idle: close leftover talk AO + capture AI.
     * Must run AFTER Nebula/RTSP unreg clients; otherwise next app (audio_algo
     * PCM@16k) hits SDK AEC fail against orphan AO@8k.
     */
    if (!s_mm)
        s_mm = (multimedia_t *)app_main_get_multimedia();
    if (s_mm && s_mm->audio && s_mm->audio->reset_idle) {
        log_info("[IPC-TEARDOWN] audio reset_idle begin");
        (void)s_mm->audio->reset_idle();
        log_info("[IPC-TEARDOWN] audio reset_idle done");
    }
    /* 4) OSD after streams idle — avoids ShowCanvas(vaddr NULL) spam. */
    if (s_mm && s_mm->service && s_mm->service->teardown_osd_for_camera_close) {
        log_info("[IPC-TEARDOWN] osd teardown begin");
        s_mm->service->teardown_osd_for_camera_close();
        log_info("[IPC-TEARDOWN] osd teardown done");
    }
    /* 5) Drop manager ref (last holder may close device + join AI threads).
     * Do NOT set_ai_enabled(0) here: ai_disable → tile_overlap races HAL after streams
     * stop (fault_addr=0xb8). Backup leaves AI teardown to camera_manager_close. */
    if (s_mm && s_mm->service && s_mm->service->release_camera_manager) {
        log_info("[IPC-TEARDOWN] release_camera_manager begin");
        s_mm->service->release_camera_manager();
        log_info("[IPC-TEARDOWN] release_camera_manager done");
    }
    s_mm = NULL;
    memset(&s_ctx, 0, sizeof(s_ctx));
    /* 保持 s_ipc_teardown=1 直至下次 tablet_call_create：避免 LVGL/广播晚到回调在 memset 之后
     * 仍见 teardown=0 而访问已清零的 s_ctx 或已释放的 pipeline 相关状态（曾导致 GuiThread SIGSEGV）。 */
    s_in_window_delete_cb = 0;
    log_info("[IPC-TEARDOWN] window delete: done");
}

static void call_stop(void)
{
#if !IPC_CALL_DISABLE_VIDEO_DECODE
    (void)media_player_set_video_show_callback(-1, NULL, NULL);
    call_render_stop();
#endif
    s_call_enable = 0;
    /* Stop talk video decode until the next call (accept or dial); the peer
     * may keep pushing frames after a hang-up and the player would burn CPU. */
    nebula_talk_video_decode_set_enabled(0);
    ipc_sync_talk_out(0);
    /* Fully release talk AO/AEC so leaving IPC (or CALL→PREVIEW) returns audio
     * HW to idle; otherwise audio_algo reopen can hit leftover HAL/SDK state. */
    nebula_talk_audio_session_release();
}

static int call_start(void)
{
    if (!ipc_call_network_ready()) {
        show_msgbox_note(lan("do not connect to server"));
        return -1;
    }
    /* AI runs on the dedicated ai stream (fw_video_config role=ai); must be on before camera_manager_init(). */
    if (!s_mm)
        s_mm = (multimedia_t *)app_main_get_multimedia();
    if (s_mm && s_mm->camera && s_mm->camera->set_ai_enabled) {
        s_mm->camera->set_ai_enabled(1);
    }

    s_call_enable = 1;
    /* (Re-)enable talk video decode: a reject disabled it; an incoming call
     * may already have the framework player open at the peer's resolution. */
    nebula_talk_video_decode_set_enabled(!IPC_CALL_DISABLE_VIDEO_DECODE);
    ipc_ensure_nebula_started();
    if (nebula_talk_audio_session_ensure_started() != 0) {
        log_warn("ipc: talk audio init failed on call_start");
    }
    call_config_enable_nebula_main();
#if !IPC_CALL_DISABLE_VIDEO_DECODE
    (void)media_player_set_video_show_callback(-1, ipc_call_video_show_cb, (void *)&s_ctx);
    if (call_render_start() != 0) {
        (void)media_player_set_video_show_callback(-1, NULL, NULL);
        s_call_enable = 0;
        show_msgbox_note(lan("Start video device fail"));
        return -1;
    }
#endif
    ipc_sync_talk_out(1);
    /* Show local IP early so PC tools can connect while waiting for remote video. */
    set_call_wait_status_with_ip();
    return 0;
}

/* ============================ mode switch ============================ */

static void ipc_apply_mode_ui(ipc_mode_t mode)
{
    if (!s_ctx.window || !lv_obj_is_valid(s_ctx.window))
        return;

    int idle = (mode == IPC_MODE_IDLE);
    int call = (mode == IPC_MODE_CALL);

    /* Layers */
    if (s_ctx.carousel) {
        if (idle)
            lv_obj_clear_flag(s_ctx.carousel, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_ctx.carousel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ctx.preview_area) {
        if (idle)
            lv_obj_add_flag(s_ctx.preview_area, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_ctx.preview_area, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ctx.dots_box) {
        if (idle)
            lv_obj_clear_flag(s_ctx.dots_box, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_ctx.dots_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ctx.status_bar) {
        if (idle)
            lv_obj_add_flag(s_ctx.status_bar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_ctx.status_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* Action bar buttons per mode (hidden widgets drop out of the flex row). */
    if (s_ctx.btn_call)
        lv_obj_add_flag(s_ctx.btn_call, LV_OBJ_FLAG_HIDDEN);
    if (s_ctx.btn_hangup)
        lv_obj_add_flag(s_ctx.btn_hangup, LV_OBJ_FLAG_HIDDEN);
    if (idle) {
        if (s_ctx.btn_call)
            lv_obj_clear_flag(s_ctx.btn_call, LV_OBJ_FLAG_HIDDEN);
    } else if (call) {
        /* In-call: Hang-up only. Status bar right shows the local IP. */
        if (s_ctx.btn_hangup)
            lv_obj_clear_flag(s_ctx.btn_hangup, LV_OBJ_FLAG_HIDDEN);
        ipc_update_ip_label();
    }

    /* Entering a mode reveals its bar once; it auto-hides (design §2). */
    ipc_reveal_op_bar();
}

static void switch_mode(ipc_mode_t new_mode)
{
    if (s_ipc_teardown)
        return;

    if (new_mode == IPC_MODE_IDLE) {
        if (s_ctx.mode == IPC_MODE_IDLE)
            return;
        call_stop();
        /* AI teardown is left to release_camera_manager in the LV_EVENT_DELETE
         * path — set_ai_enabled(0) here races camera_service_reset after
         * talk_stop dropped the last client (fault_addr=0xb8). */
        ipc_rtsp_stop();
        s_ctx.mode = IPC_MODE_IDLE;
        ipc_apply_mode_ui(IPC_MODE_IDLE);
        return;
    }

    if (new_mode == IPC_MODE_CALL) {
        if (s_ctx.mode == IPC_MODE_CALL) {
            /* Ignore repeated call clicks to avoid accidental stop/start flapping. */
            return;
        }
        ipc_rtsp_stop();
        /* Entering a call answers any pending ring. */
        ipc_ring_show(0);
        if (call_start() != 0)
            return;
        s_ctx.mode = IPC_MODE_CALL;
        /* Status already set to local IP in call_start; "In call" when remote frame arrives. */
        ipc_apply_mode_ui(IPC_MODE_CALL);
        return;
    }
}

static void event_handler_btn_call(lv_event_t *e)
{
    (void)e;
    switch_mode(IPC_MODE_CALL);
}

static void event_handler_btn_hangup(lv_event_t *e)
{
    (void)e;
    switch_mode(IPC_MODE_IDLE);
}

static void event_handler_btn_accept(lv_event_t *e)
{
    (void)e;
    if (s_ipc_teardown)
        return;
    log_info("ipc: incoming call accepted");
    ipc_ring_show(0);
    switch_mode(IPC_MODE_CALL);
}

static void event_handler_btn_reject(lv_event_t *e)
{
    (void)e;
    if (s_ipc_teardown)
        return;
    log_info("ipc: incoming call rejected");
    ipc_ring_show(0);
    /* Release what the framework opened on Ve=1: talk video player + AO/AEC. */
    nebula_talk_video_decode_set_enabled(0);
    nebula_talk_audio_session_release();
    if (s_ctx.mode != IPC_MODE_IDLE)
        switch_mode(IPC_MODE_IDLE);
}

/* ============================ ui tick (500 ms) ============================ */

static void ipc_ui_tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();

    if (s_ipc_teardown)
        return;

    /* Drain incoming-call events (PC CMD_ReqMediaTalk via "net2ui"). */
    ipc_read_net2ui();

    /* Standby clock pages + album slideshow, 1 Hz. */
    if (s_ctx.mode == IPC_MODE_IDLE &&
        (now - s_ctx.last_clock_update_ms) >= 1000u) {
        s_ctx.last_clock_update_ms = now;
        ipc_update_clock_page();
        ipc_update_analog_clock();
        if (s_ctx.cur_page == IPC_PAGE_ALBUM &&
            (uint32_t)(now - s_ctx.last_slide_ms) >= IPC_SLIDE_INTERVAL_MS) {
            s_ctx.last_slide_ms = now;
            ipc_album_advance();
        }
    }

    /* Connected call duration, 1 Hz (design §4). */
    if (s_ctx.mode == IPC_MODE_CALL && s_ctx.call_connected &&
        (now - s_ctx.last_clock_update_ms) >= 1000u) {
        s_ctx.last_clock_update_ms = now;
        ipc_set_call_duration_status();
    }

    /* Action bar auto-hide after 3 s idle (design §2); paused while ringing. */
    if (!s_ctx.ringing && s_ctx.op_bar_visible &&
        (uint32_t)(now - s_ctx.op_bar_reveal_ms) >= IPC_REVEAL_TIMEOUT_MS) {
        ipc_hide_op_bar();
    }
}

/* ============================ services entry (N5: discovery only) ============================ */

static void ipc_start_services_after_ui(void)
{
    /* Standby keeps only Nebula discovery so LAN tools can see the device.
     * AI / RTSP / preview client start on demand in PREVIEW/CALL modes. */
    ipc_ensure_nebula_started();
    nebula_talk_video_decode_set_enabled(!IPC_CALL_DISABLE_VIDEO_DECODE);
    call_config_enable_nebula_main();
    if (!s_transport_timer) {
        /* Periodically keep Nebula alive in LVGL context. */
        s_transport_timer = lv_timer_create(ipc_transport_arbitrate_timer_cb, 200, NULL);
    }
}

/* ============================ widgets ============================ */

/** Pill (h) or circle (w==h) button: soft shadow, subtle border, press feedback. */
static lv_obj_t *ipc_create_button(lv_obj_t *parent, const char *text, uint32_t bg,
                                   int32_t w, int32_t h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, (w == h) ? LV_RADIUS_CIRCLE : h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, (lv_opa_t)217, 0);   /* ~85% */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(IPC_COLOR_TEXT), 0);
    lv_obj_set_style_border_opa(btn, (lv_opa_t)38, 0); /* ~15% */
    lv_obj_set_style_shadow_width(btn, GUI_SCALE_VER(24), 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_darken(lv_color_hex(bg), LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    if (tablet_font_get_freetype_font(22))
        lv_obj_set_style_text_font(label, tablet_font_get_freetype_font(22), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(IPC_COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *ipc_create_page(lv_obj_t *carousel)
{
    lv_obj_t *page = lv_obj_create(carousel);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(IPC_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page, ipc_surface_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(page, ipc_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    return page;
}

static void ipc_build_standby(lv_obj_t *window)
{
    /* Horizontal carousel: swipe left/right, one page per swipe (design §3). */
    s_ctx.carousel = lv_obj_create(window);
    lv_obj_remove_style_all(s_ctx.carousel);
    lv_obj_set_size(s_ctx.carousel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_pad_all(s_ctx.carousel, 0, 0);
    lv_obj_set_layout(s_ctx.carousel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ctx.carousel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ctx.carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_ctx.carousel, LV_DIR_HOR);
    /* No scroll_snap: SCROLL_END snaps manually (see ipc_carousel_scroll_cb) so
     * a short drag (~15% of a page) already turns the page. */
    lv_obj_set_scrollbar_mode(s_ctx.carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_ctx.carousel, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_add_flag(s_ctx.carousel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ctx.carousel, ipc_surface_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ctx.carousel, ipc_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_ctx.carousel, ipc_carousel_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    /* Page 0: digital clock (pure drawing, zero image decode in the first frame). */
    s_ctx.page_clock = ipc_create_page(s_ctx.carousel);
    s_ctx.time_label = lv_label_create(s_ctx.page_clock);
    lv_label_set_text(s_ctx.time_label, "--:--");
    lv_obj_set_style_text_color(s_ctx.time_label, lv_color_hex(IPC_COLOR_TEXT), 0);
    {
        /* Hero digits (96 px, raised FreeType cap); Montserrat 48 is the fallback. */
        lv_font_t *digit_font = tablet_font_get_freetype_font(IPC_DIGIT_FONT_SIZE);
        if (!digit_font)
            digit_font = tablet_font_get_montserrat_font(48);
        if (digit_font)
            lv_obj_set_style_text_font(s_ctx.time_label, digit_font, 0);
    }
    lv_obj_set_style_text_letter_space(s_ctx.time_label, 2, 0);
    lv_obj_align(s_ctx.time_label, LV_ALIGN_CENTER, 0, -GUI_SCALE_VER(56));
    /* Seconds ride at the bottom-right of the hero time, dim and small. */
    s_ctx.sec_label = lv_label_create(s_ctx.page_clock);
    lv_label_set_text(s_ctx.sec_label, "--");
    lv_obj_set_style_text_color(s_ctx.sec_label, lv_color_hex(IPC_COLOR_TEXT_DIM), 0);
    if (tablet_font_get_freetype_font(IPC_SEC_FONT_SIZE))
        lv_obj_set_style_text_font(s_ctx.sec_label, tablet_font_get_freetype_font(IPC_SEC_FONT_SIZE), 0);
    lv_obj_align_to(s_ctx.sec_label, s_ctx.time_label,
                    LV_ALIGN_OUT_RIGHT_BOTTOM, GUI_SCALE(8), 0);
    s_ctx.date_label = lv_label_create(s_ctx.page_clock);
    lv_obj_set_style_text_color(s_ctx.date_label, lv_color_hex(IPC_COLOR_TEXT_DIM), 0);
    if (tablet_font_get_freetype_font(28))
        lv_obj_set_style_text_font(s_ctx.date_label, tablet_font_get_freetype_font(28), 0);
    lv_obj_align(s_ctx.date_label, LV_ALIGN_CENTER, 0, GUI_SCALE_VER(56));
    /* Accent divider between time and date. */
    {
        lv_obj_t *div = lv_obj_create(s_ctx.page_clock);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, GUI_SCALE(56), GUI_SCALE_VER(3));
        lv_obj_set_style_radius(div, GUI_SCALE_VER(3), 0);
        lv_obj_set_style_bg_color(div, lv_color_hex(IPC_COLOR_PRIMARY), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(div, LV_ALIGN_CENTER, 0, GUI_SCALE_VER(14));
    }
    ipc_update_clock_page();
    s_ctx.last_clock_update_ms = lv_tick_get();

    /* Page 1: analog clock face restored from the clock app (bg + needle images). */
    s_ctx.page_analog = ipc_create_page(s_ctx.carousel);
    {
        char path_bg[420];
        char path_s[420];
        char path_m[420];
        char path_h[420];
        lv_obj_t *bg = lv_image_create(s_ctx.page_analog);

        ipc_clock_asset_path(path_bg, sizeof(path_bg), "clock_bg.qoi");
        ipc_clock_asset_path(path_h, sizeof(path_h), "clock_h.qoi");
        ipc_clock_asset_path(path_m, sizeof(path_m), "clock_m.qoi");
        ipc_clock_asset_path(path_s, sizeof(path_s), "clock_s.qoi");
        if (path_bg[0])
            lv_image_set_src(bg, path_bg);
        lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(bg);
        lv_obj_update_layout(bg);
        if (path_bg[0] && lv_obj_get_width(bg) > 0) {
            s_ctx.analog_scale = lv_scale_create(s_ctx.page_analog);
            lv_obj_remove_style_all(s_ctx.analog_scale);
            lv_obj_remove_flag(s_ctx.analog_scale, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(s_ctx.analog_scale, lv_obj_get_width(bg), lv_obj_get_height(bg));
            lv_obj_align_to(s_ctx.analog_scale, bg, LV_ALIGN_CENTER, 0, 0);
            lv_scale_set_mode(s_ctx.analog_scale, LV_SCALE_MODE_ROUND_INNER);
            lv_scale_set_label_show(s_ctx.analog_scale, false);
            lv_scale_set_total_tick_count(s_ctx.analog_scale, 60);
            lv_scale_set_major_tick_every(s_ctx.analog_scale, 5);
            lv_scale_set_range(s_ctx.analog_scale, 0, 60);
            lv_scale_set_angle_range(s_ctx.analog_scale, 360);
            lv_scale_set_rotation(s_ctx.analog_scale, 270);

            s_ctx.needle_sec = lv_image_create(s_ctx.analog_scale);
            lv_image_set_src(s_ctx.needle_sec, path_s);
            lv_obj_align(s_ctx.needle_sec, LV_ALIGN_CENTER, 0, 0);
            lv_image_set_pivot(s_ctx.needle_sec, 155, 13);
            lv_obj_remove_flag(s_ctx.needle_sec, LV_OBJ_FLAG_CLICKABLE);

            s_ctx.needle_min = lv_image_create(s_ctx.analog_scale);
            lv_image_set_src(s_ctx.needle_min, path_m);
            lv_obj_align(s_ctx.needle_min, LV_ALIGN_CENTER, 0, 0);
            lv_image_set_pivot(s_ctx.needle_min, 138, 15);
            lv_obj_remove_flag(s_ctx.needle_min, LV_OBJ_FLAG_CLICKABLE);

            s_ctx.needle_hour = lv_image_create(s_ctx.analog_scale);
            lv_image_set_src(s_ctx.needle_hour, path_h);
            lv_obj_align(s_ctx.needle_hour, LV_ALIGN_CENTER, 0, 0);
            lv_image_set_pivot(s_ctx.needle_hour, 94, 19);
            lv_obj_remove_flag(s_ctx.needle_hour, LV_OBJ_FLAG_CLICKABLE);

            ipc_update_analog_clock();
        } else {
            log_warn("ipc: analog clock bg missing, page stays dark");
        }
    }

    /* Page 2: photo album; slides auto-advance, sources probed in the photo timer. */
    s_ctx.page_album = ipc_create_page(s_ctx.carousel);
    s_ctx.slide_img = lv_image_create(s_ctx.page_album);
    lv_obj_set_size(s_ctx.slide_img, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(s_ctx.slide_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_clear_flag(s_ctx.slide_img, LV_OBJ_FLAG_CLICKABLE);
    s_ctx.last_slide_ms = lv_tick_get();

    /* Page dots (non-clickable) at the very bottom, under the action bar. */
    s_ctx.dots_box = lv_obj_create(window);
    lv_obj_remove_style_all(s_ctx.dots_box);
    lv_obj_set_size(s_ctx.dots_box, LV_HOR_RES, IPC_DOT_SIZE);
    lv_obj_align(s_ctx.dots_box, LV_ALIGN_BOTTOM_MID, 0, IPC_DOTS_Y_OFF);
    lv_obj_set_layout(s_ctx.dots_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ctx.dots_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ctx.dots_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ctx.dots_box, IPC_DOTS_GAP, 0);
    lv_obj_clear_flag(s_ctx.dots_box, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < IPC_BG_PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(s_ctx.dots_box);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, IPC_DOT_SIZE, IPC_DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(IPC_COLOR_TEXT), 0);
        lv_obj_set_style_bg_opa(dot, (i == 0) ? LV_OPA_90 : LV_OPA_30, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s_ctx.dots[i] = dot;
    }
}

static void ipc_build_status_bar(lv_obj_t *window)
{
    s_ctx.status_bar = lv_obj_create(window);
    lv_obj_remove_style_all(s_ctx.status_bar);
    lv_obj_set_size(s_ctx.status_bar, LV_HOR_RES, IPC_STATUS_H);
    lv_obj_align(s_ctx.status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ctx.status_bar, lv_color_hex(IPC_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ctx.status_bar, LV_OPA_50, 0);
    lv_obj_clear_flag(s_ctx.status_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ctx.status_bar, LV_OBJ_FLAG_HIDDEN);

    s_ctx.status_left = lv_label_create(s_ctx.status_bar);
    lv_obj_set_style_text_color(s_ctx.status_left, lv_color_hex(IPC_COLOR_TEXT), 0);
    if (tablet_font_get_freetype_font(20))
        lv_obj_set_style_text_font(s_ctx.status_left, tablet_font_get_freetype_font(20), 0);
    lv_obj_align(s_ctx.status_left, LV_ALIGN_LEFT_MID, GUI_SCALE(16), 0);

    s_ctx.status_right = lv_label_create(s_ctx.status_bar);
    lv_obj_set_style_text_color(s_ctx.status_right, lv_color_hex(IPC_COLOR_TEXT_DIM), 0);
    if (tablet_font_get_freetype_font(20))
        lv_obj_set_style_text_font(s_ctx.status_right, tablet_font_get_freetype_font(20), 0);
    lv_obj_align(s_ctx.status_right, LV_ALIGN_RIGHT_MID, -GUI_SCALE(16), 0);
}

static void ipc_build_op_bar(lv_obj_t *window)
{
    s_ctx.op_bar = lv_obj_create(window);
    lv_obj_remove_style_all(s_ctx.op_bar);
    lv_obj_set_size(s_ctx.op_bar, LV_PCT(IPC_BAR_W_PCT), IPC_BAR_H);
    lv_obj_align(s_ctx.op_bar, LV_ALIGN_BOTTOM_MID, 0, IPC_BAR_Y_OFF);
    /* Transparent container: only the button itself is drawn, no dark strip. */
    lv_obj_set_style_opa(s_ctx.op_bar, 0, 0);
    lv_obj_set_layout(s_ctx.op_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ctx.op_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ctx.op_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ctx.op_bar, LV_OBJ_FLAG_HIDDEN);

    s_ctx.btn_call = ipc_create_button(s_ctx.op_bar, lan("Video call"),
                                       IPC_COLOR_PRIMARY, IPC_BTN_W, IPC_BTN_H,
                                       event_handler_btn_call);
    s_ctx.btn_hangup = ipc_create_button(s_ctx.op_bar, lan("Hang up"),
                                         IPC_COLOR_DANGER, IPC_BTN_W, IPC_BTN_H,
                                         event_handler_btn_hangup);
}

/** Incoming-call overlay: Answer (green) / Hang-up (red) circles, always visible. */
static void ipc_build_ring_box(lv_obj_t *window)
{
    s_ctx.ring_box = lv_obj_create(window);
    lv_obj_remove_style_all(s_ctx.ring_box);
    lv_obj_set_size(s_ctx.ring_box, LV_HOR_RES, GUI_SCALE_VER(150));
    lv_obj_align(s_ctx.ring_box, LV_ALIGN_BOTTOM_MID, 0, IPC_BAR_Y_OFF);
    lv_obj_set_layout(s_ctx.ring_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ctx.ring_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ctx.ring_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ctx.ring_box, LV_OBJ_FLAG_HIDDEN);

    s_ctx.btn_accept = ipc_create_button(s_ctx.ring_box, lan("Answer"),
                                         IPC_COLOR_PRIMARY, IPC_RING_BTN_SIZE, IPC_RING_BTN_SIZE,
                                         event_handler_btn_accept);
    /* Ripple halo: child of the Answer button so the flex row is untouched; it
     * grows past the button edge (overflow is visible, no clipping). */
    s_ctx.ring_halo = lv_obj_create(s_ctx.btn_accept);
    lv_obj_remove_style_all(s_ctx.ring_halo);
    lv_obj_set_size(s_ctx.ring_halo, IPC_RING_BTN_SIZE, IPC_RING_BTN_SIZE);
    lv_obj_align(s_ctx.ring_halo, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_ctx.ring_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ctx.ring_halo, 0, 0);
    lv_obj_set_style_border_width(s_ctx.ring_halo, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_border_color(s_ctx.ring_halo, lv_color_hex(IPC_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_opa(s_ctx.ring_halo, 0, 0);
    lv_obj_move_to_index(s_ctx.ring_halo, 0); /* below the button label */
    s_ctx.btn_reject = ipc_create_button(s_ctx.ring_box, lan("Hang up"),
                                         IPC_COLOR_DANGER, IPC_RING_BTN_SIZE, IPC_RING_BTN_SIZE,
                                         event_handler_btn_reject);
}

/* ============================ create / entry ============================ */

lv_result_t tablet_call_create(lv_obj_t *main, int call_mode)
{
    (void)call_mode;
    s_ipc_teardown = 0;
    lv_display_t *disp = lv_display_get_default();
    if (s_ctx.window) {
        if (lv_obj_is_valid(s_ctx.window)) {
            if (main)
                lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ctx.window);
            lv_obj_invalidate(s_ctx.window);
            if (disp)
                lv_refr_now(disp);
            log_debug("ipc create reuse existing window");
            ipc_start_services_after_ui();
            return LV_RESULT_OK;
        }
        log_warn("ipc stale window pointer detected, reset context");
        memset(&s_ctx, 0, sizeof(s_ctx));
    }

    lv_obj_t *scr = disp ? lv_display_get_screen_active(disp) : NULL;
    if (!scr)
        return LV_RESULT_INVALID;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.main = main;
    s_ctx.mode = IPC_MODE_IDLE;

    s_ctx.window = lv_obj_create(scr);
    if (!s_ctx.window)
        return LV_RESULT_INVALID;
    lv_obj_add_event_cb(s_ctx.window, ipc_window_delete_event_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(s_ctx.window);
    lv_obj_set_size(s_ctx.window, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_ctx.window, lv_color_hex(IPC_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ctx.window, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ctx.window, LV_OBJ_FLAG_SCROLLABLE);

    /* Phone-style home indicator: tap / swipe up at the bottom returns to
     * Home (IDLE only, same guard as long-press). */
    tablet_create_home_bar(s_ctx.window, ipc_home_bar_back_cb, NULL);

    ipc_build_standby(s_ctx.window);

    /* Fullscreen surface for PREVIEW / CALL (above carousel, below bars). */
    s_ctx.preview_area = lv_obj_create(s_ctx.window);
    lv_obj_remove_style_all(s_ctx.preview_area);
    lv_obj_set_size(s_ctx.preview_area, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_ctx.preview_area, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_ctx.preview_area, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ctx.preview_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ctx.preview_area, ipc_surface_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_ctx.preview_area, LV_OBJ_FLAG_HIDDEN);

    ipc_build_status_bar(s_ctx.window);
    ipc_build_op_bar(s_ctx.window);
    ipc_build_ring_box(s_ctx.window);

    /* Trim bar buttons to the IDLE set (preview/call only). Stop/Hang-up stay
     * hidden until an event (incoming ring or an actual call) shows them. */
    ipc_apply_mode_ui(IPC_MODE_IDLE);
    s_ctx.op_bar_visible = 0;
    if (s_ctx.op_bar && lv_obj_is_valid(s_ctx.op_bar))
        lv_obj_add_flag(s_ctx.op_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_foreground(s_ctx.status_bar);
    lv_obj_move_foreground(s_ctx.op_bar);
    lv_obj_move_foreground(s_ctx.ring_box);
    lv_obj_invalidate(s_ctx.window);
    if (disp)
        lv_refr_now(disp);

    /* Swipe sensitivity: start page turns after a short drag (default is 10 px). */
    {
        lv_indev_t *indev = lv_indev_get_next(NULL);
        while (indev) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
                lv_indev_set_scroll_limit(indev, IPC_SWIPE_LIMIT_PX);
            indev = lv_indev_get_next(indev);
        }
    }

    /* Deferred QOI decode (after first frame, same approach as home wallpaper). */
    s_ctx.photo_timer = lv_timer_create(ipc_photo_load_timer_cb, 100, NULL);
    s_ctx.ui_tick_timer = lv_timer_create(ipc_ui_tick_timer_cb, 500, NULL);

    log_info("ipc create done");
    ipc_start_services_after_ui();

    return LV_RESULT_OK;
}

int tablet_call_msg_callback(int enable, void *pUser)
{
    (void)pUser;
    s_call_enable = enable;
    return 0;
}

int tablet_call_connect_state_callback(message_t *pMsg, void *pUser)
{
    (void)pUser;
    if (pMsg && pMsg->data && pMsg->data_len >= (int)sizeof(int32_t))
        s_server_connect_state = *((int32_t *)pMsg->data);
    return 0;
}

int tablet_call_get_state(void) { return s_call_enable; }
int tablet_call_get_net_connect_state(void) { return s_server_connect_state; }
#endif
