#include "tablet_audio_algo.h"

#define LOG_TAG "TABLET_AUDIO_ALGO"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_main.h"
#include "config.h"
#include "log.h"
#include "multimedia.h"
#include "notification.h"
#include "system_thread.h"
#include "tablet_common.h"
#include "tablet_font.h"

/* Match settings / 副本 nova dark theme (same hex as tablet_setting.h SETTING_*) */
#define SSL_PAGE_BG      0x1c1c1eU
#define SSL_CONTENT_BG   0x242426U
#define SSL_TEXT         0xffffffU
#define SSL_TEXT_SEC     0x98989dU
#define SSL_BORDER       0x48484aU

#define PCM_SAMPLE_INTERVAL_MS     40
#define PCM_EVENT_MAX              24
#define PCM_MAX_CHANNELS           2
/* Target scale: 60s window with 10ms per point (6000 points). */
#define PCM_POINT_INTERVAL_MS      2
/* File-scope arrays require compile-time constant length. */
#ifdef LV_HOR_RES_MAX
#define PCM_POINT_COUNT            LV_HOR_RES_MAX
#else
#define PCM_POINT_COUNT            1024
#endif
#define PCM_HISTORY_MS             (PCM_POINT_COUNT * PCM_POINT_INTERVAL_MS)
#define PCM_CLEAR_PERCENT          10
#define PCM_CLEAR_POINTS           ((PCM_POINT_COUNT * PCM_CLEAR_PERCENT) / 100)
#define PCM_RECENT_POINTS          PCM_CLEAR_POINTS
#define PCM_FETCH_BATCH_MAX        64
#define UI_TIMER_PERIOD_MS         30
/* Wave vertical fill: peak maps to ~90% of the half-band (stretched view). */
#define PCM_VISUAL_GAIN_MIN        2
#define PCM_VISUAL_GAIN_MAX        64
#define PCM_VISUAL_TARGET_PCT      90
/* ActionCam (single ADC MIC): no SSL UI/enable. DMIC boards keep sound localization. */
#if defined(CONFIG_BOARD_SUPPORT_DMIC) && CONFIG_BOARD_SUPPORT_DMIC
#define AUDIO_ALGO_SSL_ENABLE      1
#else
#define AUDIO_ALGO_SSL_ENABLE      0
#endif
#define INIT_STATE_IDLE            0
#define INIT_STATE_RUNNING         1
#define INIT_STATE_READY           2
#define INIT_STATE_FAILED          3
#define INIT_STATE_PCM_READY       4
#define INIT_STATE_ENABLING_VAD    5
#define INIT_STATE_ENABLING_CRY    6
#define INIT_STATE_ENABLING_SSL    7
#define INIT_STATE_ENABLING_STREAM 8

typedef struct {
    lv_obj_t *label;
    int sample_index;
    long long timestamp_ms;
    char type;
    bool valid;
} pcm_event_marker_t;

typedef struct {
    lv_obj_t *main;
    lv_obj_t *window;
    lv_timer_t *timer;
    lv_timer_t *init_timer;
    multimedia_t *mm;
    lv_obj_t *label_ssl_angle;
    lv_obj_t *label_cry_time;
    lv_obj_t *label_vad_time;
    lv_obj_t *label_info;
    int pcm_last_gain;
    lv_obj_t *pcm_panel;
    lv_obj_t *pcm_mid_line[PCM_MAX_CHANNELS];
    lv_obj_t *pcm_wave_line[PCM_MAX_CHANNELS];
    lv_obj_t *pcm_recent_line[PCM_MAX_CHANNELS];
    lv_obj_t *pcm_recent_line_wrap[PCM_MAX_CHANNELS];
    lv_point_precise_t pcm_mid_points[PCM_MAX_CHANNELS][2];
    lv_point_precise_t pcm_points[PCM_MAX_CHANNELS][PCM_POINT_COUNT];
    lv_point_precise_t pcm_recent_points[PCM_MAX_CHANNELS][PCM_RECENT_POINTS];
    lv_point_precise_t pcm_recent_points_wrap[PCM_MAX_CHANNELS][PCM_RECENT_POINTS];
    int pcm_samples[PCM_MAX_CHANNELS][PCM_POINT_COUNT];
    int pcm_head;
    int pcm_filled_points;
    int pcm_channels;
    int pcm_client_handle;
    bool pcm_stream_ready;
    int pcm_last_samples[PCM_MAX_CHANNELS];
    long long pcm_last_tick_us;
    long long pcm_accum_us;
    long long pcm_last_fetch_us;
    int pcm_last_due_points;
    int pcm_last_push_points;
    int pcm_fetch_fail_count;
    int init_thread_handle;
    int init_state;
    int init_ret_open;
    int init_ret_ssl;
    int init_ret_cry;
    int init_ret_vad;
    bool init_thread_reaped;
    pcm_event_marker_t pcm_events[PCM_EVENT_MAX];
    int pcm_event_head;
    long long cry_ts_ms;
    long long vad_ts_ms;
    bool cry_updated;
    bool vad_updated;
    bool closing;
    bool deinit_done;
    bool alarm_mutex_ready;
    bool slider_mutex_ready;
    pthread_mutex_t alarm_mutex;
} context_t;

static context_t context;

typedef struct {
    lv_obj_t *obj;
    int32_t value;
    bool value_valid;
    pthread_mutex_t mutex;
} sound_slider_t;

static sound_slider_t sound_slider;

static void slider_event_callback(lv_event_t *e);
static int sound_msg_callback(message_t *msg, void *pUser);
static int pcm_stream_callback(int handle, media_frame_info_t *frame, void *user_data);
static int alarm_cry_callback(alarm_t *alarm, int msg_len, void *user_data);
static int alarm_vad_callback(alarm_t *alarm, int msg_len, void *user_data);
static void slider_task_timer(lv_timer_t *timer);
static void init_audio_algorithms_timer_cb(lv_timer_t *timer);
static int init_audio_algorithms_thread(void *param);
static int audio_algo_set_enable(media_audio_algorithm_t algorithm, int enable);
static int audio_algo_set_default_param(media_audio_algorithm_t algorithm, int value);
static int audio_algo_prepare_pcm_stream(int stream);
static void format_event_time(long long timestamp_ms, char *buf, size_t buf_size);
static const char *i18n_text(const char *en, const char *zh);
static lv_font_t *i18n_font(int en_size, int zh_size);
static lv_obj_t *create_algo_card(lv_obj_t *parent, const char *title, const char *desc, lv_obj_t **value_label);
static lv_obj_t *create_section_title(lv_obj_t *parent, const char *txt);
static lv_obj_t *create_pcm_card(lv_obj_t *parent);
static void pcm_push_sample(int amplitude);
static void pcm_push_sample_multi(const int *samples, int channels);
static void pcm_clear_oldest_window(void);
static int pcm_fetch_stream_sample(int *samples, int *channels);
static int pcm_fetch_stream_sample_latest(int *samples, int *channels, int max_reads);
static int pcm_fetch_stream_sample_batch(int samples[][PCM_MAX_CHANNELS], int *out_points, int *channels, int max_points);
static int audio_algo_rebind_pcm_client(void);
static void pcm_add_event(char type, long long timestamp_ms);
static void pcm_refresh_view(void);
static void update_info_label(void);
static int pcm_calc_visual_gain(int band_h);
static long long pcm_now_us(void);
static int init_audio_algorithms(void);
static void deinit_audio_algorithms(void);
static void deinit_audio_algorithms_on_init_fail(void);
static void audio_algo_cleanup_once(void);
static void audio_algo_window_delete_event(lv_event_t *e);
static void audio_algo_start_ui_timer(lv_obj_t *optional_slider);
#if AUDIO_ALGO_SSL_ENABLE
static lv_obj_t *ssl_sound_slider_init(lv_obj_t *parent);
#endif

/* Cry/VAD + PCM UI timer; SSL slider is optional (DMIC only). */
static void audio_algo_start_ui_timer(lv_obj_t *optional_slider)
{
    sound_slider_t *pslider = (sound_slider_t *)(&sound_slider);

    if (!context.slider_mutex_ready) {
        memset((void *)pslider, 0, sizeof(sound_slider_t));
        pthread_mutex_init(&(pslider->mutex), NULL);
        context.slider_mutex_ready = true;
    }
    pslider->obj = optional_slider;

    notification_t *notif = (notification_t *)app_main_get_notification();
#if AUDIO_ALGO_SSL_ENABLE
    if (optional_slider && notif && notif->message) {
        notif->message->reg_client(MESSAGE_MANAGER_TYPE_LSSL, sound_msg_callback, (void *)pslider);
    }
#else
    (void)optional_slider;
#endif
    if (notif && notif->alarm) {
        notif->alarm->reg_client(ALARM_TYPE_CRY, alarm_cry_callback, NULL, 0);
        notif->alarm->reg_client(ALARM_TYPE_VOICE, alarm_vad_callback, NULL, 0);
    }
    if (!context.timer) {
        context.timer = lv_timer_create(slider_task_timer, 30, (void *)pslider);
        lv_timer_ready(context.timer);
    }
}

#if AUDIO_ALGO_SSL_ENABLE
static lv_obj_t *ssl_sound_slider_init(lv_obj_t *parent)
{
    const int min = 0;
    const int max = 180;
    const int middle = 90;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_width(slider, LV_PCT(92));
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, middle, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_event_callback, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    audio_algo_start_ui_timer(slider);
    return slider;
}
#endif

static void slider_event_callback(lv_event_t *e)
{
    (void)e;
}

static int sound_msg_callback(message_t *msg, void *pUser)
{
    sound_slider_t *pslider = (sound_slider_t *)pUser;
    if (context.closing || !context.slider_mutex_ready)
        return 0;
    if (!msg || !msg->data || msg->data_len < (int)sizeof(int32_t))
        return -1;
    int32_t value = *((int32_t *)(msg->data));
    pthread_mutex_lock(&(pslider->mutex));
    pslider->value = value;
    pslider->value_valid = true;
    pthread_mutex_unlock(&(pslider->mutex));
    return 0;
}

static int pcm_stream_callback(int handle, media_frame_info_t *frame, void *user_data)
{
    (void)handle;
    (void)frame;
    (void)user_data;
    return 0;
}

static int alarm_cry_callback(alarm_t *alarm, int msg_len, void *user_data)
{
    (void)msg_len;
    (void)user_data;
    if (context.closing || !context.alarm_mutex_ready)
        return 0;
    if (!alarm)
        return -1;

    pthread_mutex_lock(&context.alarm_mutex);
    context.cry_ts_ms = alarm->timestamp_ms;
    context.cry_updated = true;
    pthread_mutex_unlock(&context.alarm_mutex);
    return 0;
}

static int alarm_vad_callback(alarm_t *alarm, int msg_len, void *user_data)
{
    (void)msg_len;
    (void)user_data;
    if (context.closing || !context.alarm_mutex_ready)
        return 0;
    if (!alarm)
        return -1;

    pthread_mutex_lock(&context.alarm_mutex);
    context.vad_ts_ms = alarm->timestamp_ms;
    context.vad_updated = true;
    pthread_mutex_unlock(&context.alarm_mutex);
    return 0;
}

static void slider_task_timer(lv_timer_t *timer)
{
    if (context.closing) {
        return;
    }
    sound_slider_t *pslider = (sound_slider_t *)lv_timer_get_user_data(timer);
    lv_obj_t *slider = pslider ? pslider->obj : NULL;
    const int32_t min = 0;
    const int32_t max = 180;
    int32_t value = 0;
    bool valid = false;
    bool cry_updated = false;
    bool vad_updated = false;
    long long cry_ts_ms = 0;
    long long vad_ts_ms = 0;
#if AUDIO_ALGO_SSL_ENABLE
    if (slider && pslider) {
        bool hidden = lv_obj_has_flag(slider, LV_OBJ_FLAG_HIDDEN);
        pthread_mutex_lock(&(pslider->mutex));
        value = pslider->value;
        valid = pslider->value_valid;
        pslider->value_valid = false;
        pthread_mutex_unlock(&(pslider->mutex));
        if (valid && (!hidden)) {
            value = (value < min) ? min : value;
            value = (value > max) ? max : value;
            lv_slider_set_value(slider, value, LV_ANIM_ON);
            if (context.label_ssl_angle) {
                lv_label_set_text_fmt(context.label_ssl_angle, "%s: %d",
                                      i18n_text("Direction", "方向"), value);
            }
            lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
#else
    (void)slider;
    (void)min;
    (void)max;
    (void)value;
    (void)valid;
#endif

    pthread_mutex_lock(&context.alarm_mutex);
    cry_updated = context.cry_updated;
    vad_updated = context.vad_updated;
    cry_ts_ms = context.cry_ts_ms;
    vad_ts_ms = context.vad_ts_ms;
    context.cry_updated = false;
    context.vad_updated = false;
    pthread_mutex_unlock(&context.alarm_mutex);

    if (cry_updated) {
        char time_buf[64] = {0};
        format_event_time(cry_ts_ms, time_buf, sizeof(time_buf));
        pcm_add_event('C', cry_ts_ms);
        if (context.label_cry_time) {
            lv_label_set_text_fmt(context.label_cry_time, "%s: %s",
                                  i18n_text("Last event", "最近事件"), time_buf);
        }
    }
    if (vad_updated) {
        char time_buf[64] = {0};
        format_event_time(vad_ts_ms, time_buf, sizeof(time_buf));
        pcm_add_event('V', vad_ts_ms);
        if (context.label_vad_time) {
            lv_label_set_text_fmt(context.label_vad_time, "%s: %s",
                                  i18n_text("Last event", "最近事件"), time_buf);
        }
    }

    if (context.init_state == INIT_STATE_READY || context.init_state == INIT_STATE_FAILED) {
        (void)thread_destroy(context.init_thread_handle);
        context.init_thread_reaped = true;
        context.init_thread_handle = 0;
    }

    long long now_us = pcm_now_us();
    if (context.pcm_last_tick_us <= 0) {
        context.pcm_last_tick_us = now_us;
    }
    long long delta_us = now_us - context.pcm_last_tick_us;
    if (delta_us < 0) delta_us = 0;
    context.pcm_last_tick_us = now_us;
    context.pcm_accum_us += delta_us;

    const long long point_us = (long long)PCM_POINT_INTERVAL_MS * 1000LL;
    int due_points = (int)(context.pcm_accum_us / point_us);
    if (due_points > 120) due_points = 120; /* avoid long catch-up stalls */
    if (due_points < 0) due_points = 0;
    context.pcm_last_due_points = due_points;
    context.pcm_last_push_points = 0;
    if (due_points > 0) {
        context.pcm_accum_us -= (long long)due_points * point_us;
    }

    int pushed = 0;
    while (pushed < due_points) {
        int need = due_points - pushed;
        if (need > PCM_FETCH_BATCH_MAX) need = PCM_FETCH_BATCH_MAX;

        int batch[PCM_FETCH_BATCH_MAX][PCM_MAX_CHANNELS] = {{0}};
        int batch_points = 0;
        int channels = 1;
        long long t0 = pcm_now_us();
        int ok = pcm_fetch_stream_sample_batch(batch, &batch_points, &channels, need);
        context.pcm_last_fetch_us = pcm_now_us() - t0;
        if (ok == 0 && batch_points > 0) {
            context.pcm_fetch_fail_count = 0;
            context.pcm_channels = channels;
            pcm_clear_oldest_window();
            for (int i = 0; i < batch_points; i++) {
                for (int ch = 0; ch < PCM_MAX_CHANNELS; ch++) {
                    context.pcm_last_samples[ch] = batch[i][ch];
                }
                pcm_push_sample_multi(batch[i], channels);
            }
            pushed += batch_points;
            context.pcm_last_push_points += batch_points;
        } else {
            context.pcm_fetch_fail_count++;
            /* Strict stream-source rule: no fallback points when fetch fails. */
            break;
        }
    }

    pcm_refresh_view();
    update_info_label();
}

static void init_audio_algorithms_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (context.init_timer) {
        lv_timer_delete(context.init_timer);
        context.init_timer = NULL;
    }
    context.init_state = INIT_STATE_RUNNING;
    context.init_thread_handle = 0;
    context.init_thread_reaped = false;
    if (thread_create(&context.init_thread_handle, init_audio_algorithms_thread, NULL,
                      "audio_algo_init", 0, 0) != 0) {
        context.init_state = INIT_STATE_FAILED;
        context.init_ret_open = -1;
        context.init_ret_ssl = -1;
        context.init_ret_cry = -1;
        context.init_ret_vad = -1;
    }
}

static int init_audio_algorithms_thread(void *param)
{
    (void)param;
    (void)init_audio_algorithms();
    return 0;
}

static void event_handler_btn_back(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_user_data(e);

    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(obj);
    }
    if (context.window)
        lv_obj_add_flag(context.window, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(lv_display_get_default());
    audio_algo_cleanup_once();
    if (context.window) {
        lv_obj_delete(context.window);
    }
    context.window = NULL;
}

static void audio_algo_cleanup_once(void)
{
    if (context.deinit_done) {
        return;
    }
    context.deinit_done = true;
    context.closing = true;

    if (context.init_timer) {
        lv_timer_delete(context.init_timer);
        context.init_timer = NULL;
    }
    if (context.timer) {
        lv_timer_delete(context.timer);
        context.timer = NULL;
    }

    /* Join init thread before tearing down audio, so enable/open cannot race close. */
    if (!context.init_thread_reaped && context.init_thread_handle > 0) {
        (void)thread_destroy(context.init_thread_handle);
        context.init_thread_reaped = true;
        context.init_thread_handle = 0;
    }

    notification_t *notif = (notification_t *)app_main_get_notification();
#if AUDIO_ALGO_SSL_ENABLE
    if (notif && notif->message) {
        notif->message->unreg_client(MESSAGE_MANAGER_TYPE_LSSL, sound_msg_callback);
    }
#endif
    if (notif && notif->alarm) {
        notif->alarm->unreg_client(ALARM_TYPE_CRY, alarm_cry_callback);
        notif->alarm->unreg_client(ALARM_TYPE_VOICE, alarm_vad_callback);
    }

    deinit_audio_algorithms();
}

static void audio_algo_window_delete_event(lv_event_t *e)
{
    (void)e;
    audio_algo_cleanup_once();
    if (context.main) {
        lv_obj_remove_flag(context.main, LV_OBJ_FLAG_HIDDEN);
    }
}

static int audio_algo_set_enable(media_audio_algorithm_t algorithm, int enable)
{
    if (!context.mm) {
        context.mm = (multimedia_t *)app_main_get_multimedia();
    }
    if (!context.mm || !context.mm->audio || !context.mm->audio->set_audio_algorithm_enable) {
        return -1;
    }
    return context.mm->audio->set_audio_algorithm_enable(0, algorithm, enable);
}

static int audio_algo_set_default_param(media_audio_algorithm_t algorithm, int value)
{
    if (!context.mm) {
        context.mm = (multimedia_t *)app_main_get_multimedia();
    }
    if (!context.mm || !context.mm->audio || !context.mm->audio->set_audio_algorithm_param) {
        return -1;
    }
    return context.mm->audio->set_audio_algorithm_param(0, algorithm, "sensitivity", &value, sizeof(value));
}

static int audio_algo_prepare_pcm_stream(int stream)
{
    if (!context.mm || !context.mm->audio ||
        !context.mm->audio->stream_get_enc_param ||
        !context.mm->audio->stream_set_enc_param) {
        return -1;
    }

    media_audio_enc_param_t enc = {0};
    if (context.mm->audio->stream_get_enc_param(stream, &enc) != 0) {
        return -1;
    }

    enc.enable = 1;
    /* Talk leaves enc at 8k G711; algo path must force PCM@16k. */
    enc.sample_rate = 16000;
    if (enc.sample_bits <= 0) enc.sample_bits = 16;
    if (enc.channels <= 0) enc.channels = 1;
    enc.codec = MEDIA_CODEC_AUDIO_PCM;
    return context.mm->audio->stream_set_enc_param(stream, &enc);
}

static void format_event_time(long long timestamp_ms, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }

    time_t raw = 0;
    if (timestamp_ms > 0) {
        raw = (time_t)(timestamp_ms / 1000);
    } else {
        time(&raw);
    }
    /* Wall clock via TZ (e.g. CST-8); do not add +8 here or it doubles with localtime. */
    struct tm *timeinfo = localtime(&raw);
    if (!timeinfo) {
        (void)snprintf(buf, buf_size, "N/A");
        return;
    }

    (void)snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d",
                   timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                   timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
}

static const char *i18n_text(const char *en, const char *zh)
{
    return get_lan_index() == 1 ? zh : en;
}

static lv_font_t *i18n_font(int en_size, int zh_size)
{
    int size = get_lan_index() == 1 ? zh_size : en_size;
    return tablet_font_get_freetype_font(size);
}

static lv_obj_t *create_section_title(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, txt);
    lv_obj_set_style_text_color(title, lv_color_hex(SSL_TEXT), 0);
    lv_font_t *f = i18n_font(20, 22);
    if (f)
        lv_obj_set_style_text_font(title, f, 0);
    return title;
}

static lv_obj_t *create_algo_card(lv_obj_t *parent, const char *title, const char *desc, lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 72);
    lv_obj_set_style_bg_color(card, lv_color_hex(SSL_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(SSL_BORDER), 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 8, 0);

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(SSL_TEXT), 0);
    lv_font_t *f = i18n_font(18, 20);
    if (f)
        lv_obj_set_style_text_font(title_label, f, 0);
    lv_obj_set_width(title_label, LV_PCT(34));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = lv_label_create(header);
    lv_label_set_text(desc_label, desc);
    lv_obj_set_style_text_color(desc_label, lv_color_hex(SSL_TEXT_SEC), 0);
    lv_obj_set_width(desc_label, LV_PCT(32));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    *value_label = lv_label_create(header);
    lv_label_set_text(*value_label, "-");
    lv_obj_set_style_text_color(*value_label, lv_color_hex(SSL_TEXT), 0);
    lv_obj_set_width(*value_label, LV_PCT(34));
    lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(*value_label, LV_LABEL_LONG_CLIP);

    return card;
}

static int pcm_calc_visual_gain(int band_h)
{
    int peak = 1;
    int n = context.pcm_filled_points;
    int start;
    int i;
    int ch;
    int target;
    int gain;

    (void)band_h;
    if (n <= 0)
        return PCM_VISUAL_GAIN_MIN;
    if (n > PCM_POINT_COUNT)
        n = PCM_POINT_COUNT;
    start = (context.pcm_head - n + PCM_POINT_COUNT) % PCM_POINT_COUNT;
    for (i = 0; i < n; i++) {
        int idx = (start + i) % PCM_POINT_COUNT;
        int use_ch = context.pcm_channels > 0 ? context.pcm_channels : 1;
        if (use_ch > PCM_MAX_CHANNELS)
            use_ch = PCM_MAX_CHANNELS;
        for (ch = 0; ch < use_ch; ch++) {
            int a = context.pcm_samples[ch][idx];
            if (a < 0)
                a = -a;
            if (a > peak)
                peak = a;
        }
    }
    /* Map peak -> ~TARGET_PCT of full-scale so wave fills most of the band. */
    target = (32768 * PCM_VISUAL_TARGET_PCT) / 100;
    gain = target / peak;
    if (gain < PCM_VISUAL_GAIN_MIN)
        gain = PCM_VISUAL_GAIN_MIN;
    if (gain > PCM_VISUAL_GAIN_MAX)
        gain = PCM_VISUAL_GAIN_MAX;
    return gain;
}

static lv_obj_t *create_pcm_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
#if AUDIO_ALGO_SSL_ENABLE
    lv_obj_set_height(card, 260);
#else
    /*
     * No SSL strip: take remaining body space via flex_grow.
     * Do NOT set a large fixed height (e.g. 360): body is ~90% of 480 with
     * titles/labels; overflow makes LVGL shrink the card to ~0 and
     * pcm_refresh_view() bails out (width/height <= 4) — no waveform.
     */
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_min_height(card, 200, 0);
#endif
    lv_obj_set_style_bg_color(card, lv_color_hex(SSL_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(SSL_BORDER), 0);

    context.pcm_panel = lv_obj_create(card);
    lv_obj_remove_style_all(context.pcm_panel);
    lv_obj_set_size(context.pcm_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(context.pcm_panel, lv_color_hex(SSL_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(context.pcm_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(context.pcm_panel, 8, 0);
    lv_obj_set_style_pad_all(context.pcm_panel, 0, 0);

    for (int ch = 0; ch < PCM_MAX_CHANNELS; ch++) {
        context.pcm_mid_line[ch] = lv_line_create(context.pcm_panel);
        lv_obj_set_style_line_width(context.pcm_mid_line[ch], 1, 0);
        lv_obj_set_style_line_color(context.pcm_mid_line[ch], lv_color_hex(SSL_BORDER), 0);
        lv_obj_set_size(context.pcm_mid_line[ch], LV_PCT(100), LV_PCT(100));
        lv_obj_align(context.pcm_mid_line[ch], LV_ALIGN_CENTER, 0, 0);

        context.pcm_wave_line[ch] = lv_line_create(context.pcm_panel);
        lv_obj_set_style_line_width(context.pcm_wave_line[ch], 1, 0);
        lv_obj_set_style_line_color(context.pcm_wave_line[ch],
                                    ch == 0 ? lv_palette_main(LV_PALETTE_CYAN) : lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
        lv_obj_set_size(context.pcm_wave_line[ch], LV_PCT(100), LV_PCT(100));
        lv_obj_align(context.pcm_wave_line[ch], LV_ALIGN_CENTER, 0, 0);

        context.pcm_recent_line[ch] = lv_line_create(context.pcm_panel);
        lv_obj_set_style_line_width(context.pcm_recent_line[ch], 2, 0);
        lv_obj_set_style_line_color(context.pcm_recent_line[ch],
                                    ch == 0 ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_LIME), 0);
        lv_obj_set_size(context.pcm_recent_line[ch], LV_PCT(100), LV_PCT(100));
        lv_obj_align(context.pcm_recent_line[ch], LV_ALIGN_CENTER, 0, 0);

        context.pcm_recent_line_wrap[ch] = lv_line_create(context.pcm_panel);
        lv_obj_set_style_line_width(context.pcm_recent_line_wrap[ch], 2, 0);
        lv_obj_set_style_line_color(context.pcm_recent_line_wrap[ch],
                                    ch == 0 ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_LIME), 0);
        lv_obj_set_size(context.pcm_recent_line_wrap[ch], LV_PCT(100), LV_PCT(100));
        lv_obj_align(context.pcm_recent_line_wrap[ch], LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PCM_EVENT_MAX; i++) {
        pcm_event_marker_t *ev = &context.pcm_events[i];
        ev->label = lv_label_create(context.pcm_panel);
        lv_label_set_text(ev->label, "");
        lv_obj_add_flag(ev->label, LV_OBJ_FLAG_HIDDEN);
        ev->valid = false;
    }

    return card;
}

static void pcm_push_sample(int amplitude)
{
    if (amplitude < -32768) amplitude = -32768;
    if (amplitude > 32767) amplitude = 32767;
    context.pcm_samples[0][context.pcm_head] = amplitude;
    context.pcm_samples[1][context.pcm_head] = amplitude;
    context.pcm_head = (context.pcm_head + 1) % PCM_POINT_COUNT;
}

static void pcm_push_sample_multi(const int *samples, int channels)
{
    int use_channels = channels;
    if (use_channels < 1) use_channels = 1;
    if (use_channels > PCM_MAX_CHANNELS) use_channels = PCM_MAX_CHANNELS;

    for (int ch = 0; ch < PCM_MAX_CHANNELS; ch++) {
        int v = (ch < use_channels) ? samples[ch] : samples[0];
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        context.pcm_samples[ch][context.pcm_head] = v;
    }
    context.pcm_head = (context.pcm_head + 1) % PCM_POINT_COUNT;
    if (context.pcm_filled_points < PCM_POINT_COUNT) {
        context.pcm_filled_points++;
    }
}

static void pcm_clear_oldest_window(void)
{
    /* Clear oldest 10% points once before a new data batch arrives. */
    int clear_n = PCM_CLEAR_POINTS;
    if (clear_n < 1) clear_n = 1;
    int start = context.pcm_head;
    for (int i = 0; i < clear_n; i++) {
        int idx = (start + i) % PCM_POINT_COUNT;
        for (int ch = 0; ch < PCM_MAX_CHANNELS; ch++) {
            context.pcm_samples[ch][idx] = 0;
        }
    }

    /* Clear C/V markers that fall in the same cleared range. */
    for (int i = 0; i < PCM_EVENT_MAX; i++) {
        pcm_event_marker_t *ev = &context.pcm_events[i];
        if (!ev->valid) {
            continue;
        }
        int rel = (ev->sample_index - start + PCM_POINT_COUNT) % PCM_POINT_COUNT;
        if (rel >= 0 && rel < clear_n) {
            ev->valid = false;
            if (ev->label) {
                lv_obj_add_flag(ev->label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static int pcm_fetch_stream_sample(int *samples, int *channels)
{
    if (!context.mm || !context.mm->audio || !context.pcm_stream_ready || context.pcm_client_handle < 0 ||
        !context.mm->audio->stream_get_frame) {
        return -1;
    }

    media_frame_info_t frame = {0};
    if (context.mm->audio->stream_get_frame(context.pcm_client_handle, &frame) != 0 ||
        !frame.data || frame.data_len == 0) {
        return -1;
    }

    int ch = frame.channel_num > 0 ? frame.channel_num : 1;
    if (ch > PCM_MAX_CHANNELS) ch = PCM_MAX_CHANNELS;
    *channels = ch;

    if (frame.code_type != MEDIA_CODEC_AUDIO_PCM) {
        return -1;
    }

    if (frame.sample_bits == 16) {
        int total_samples = (int)(frame.data_len / sizeof(short));
        int frames = total_samples / ch;
        if (frames <= 0) return -1;
        const short *pcm = (const short *)frame.data;
        for (int c = 0; c < ch; c++) {
            int peak = 0;
            int peak_signed = 0;
            for (int i = c; i < total_samples; i += ch) {
                int s = pcm[i];
                int a = s < 0 ? -s : s;
                if (a > peak) {
                    peak = a;
                    peak_signed = s;
                }
            }
            samples[c] = peak_signed;
        }
        return 0;
    }

    if (frame.sample_bits == 8) {
        int total_samples = (int)frame.data_len;
        int frames = total_samples / ch;
        if (frames <= 0) return -1;
        const unsigned char *pcm = frame.data;
        for (int c = 0; c < ch; c++) {
            int peak = 0;
            int peak_signed = 0;
            for (int i = c; i < total_samples; i += ch) {
                int s = (((int)pcm[i] - 128) << 8);
                int a = s < 0 ? -s : s;
                if (a > peak) {
                    peak = a;
                    peak_signed = s;
                }
            }
            samples[c] = peak_signed;
        }
        return 0;
    }

    return -1;
}

static int pcm_fetch_stream_sample_latest(int *samples, int *channels, int max_reads)
{
    if (max_reads <= 1) {
        return pcm_fetch_stream_sample(samples, channels);
    }

    int tmp_samples[PCM_MAX_CHANNELS] = {0};
    int tmp_channels = 1;
    int got = -1;
    for (int i = 0; i < max_reads; i++) {
        if (pcm_fetch_stream_sample(tmp_samples, &tmp_channels) == 0) {
            samples[0] = tmp_samples[0];
            samples[1] = tmp_samples[1];
            *channels = tmp_channels;
            got = 0;
        } else {
            break;
        }
    }
    return got;
}

static int pcm_fetch_stream_sample_batch(int samples[][PCM_MAX_CHANNELS], int *out_points, int *channels, int max_points)
{
    if (!samples || !out_points || !channels || max_points <= 0) {
        return -1;
    }
    if (max_points > PCM_FETCH_BATCH_MAX) {
        max_points = PCM_FETCH_BATCH_MAX;
    }

    if (!context.mm || !context.mm->audio || !context.pcm_stream_ready || context.pcm_client_handle < 0 ||
        !context.mm->audio->stream_get_frame) {
        return -1;
    }

    media_frame_info_t frame = {0};
    if (context.mm->audio->stream_get_frame(context.pcm_client_handle, &frame) != 0 ||
        !frame.data || frame.data_len == 0 || frame.code_type != MEDIA_CODEC_AUDIO_PCM) {
        return -1;
    }

    int ch = frame.channel_num > 0 ? frame.channel_num : 1;
    if (ch > PCM_MAX_CHANNELS) ch = PCM_MAX_CHANNELS;
    *channels = ch;

    int sample_rate = frame.sample_rate > 0 ? frame.sample_rate : 16000;
    int point_ms = PCM_POINT_INTERVAL_MS;
    if (point_ms <= 0) point_ms = 1;
    int samples_per_point = (sample_rate * point_ms) / 1000;
    if (samples_per_point <= 0) samples_per_point = 1;

    if (frame.sample_bits == 16) {
        int total_samples = (int)(frame.data_len / (int)sizeof(short));
        int total_frames = total_samples / ch;
        if (total_frames <= 0) return -1;

        int points = (total_frames + samples_per_point - 1) / samples_per_point;
        if (points > max_points) points = max_points;
        const short *pcm = (const short *)frame.data;

        for (int p = 0; p < points; p++) {
            int start = p * samples_per_point;
            int end = start + samples_per_point;
            if (start >= total_frames) {
                points = p;
                break;
            }
            if (end > total_frames) end = total_frames;
            for (int c = 0; c < ch; c++) {
                int peak = 0;
                int peak_signed = 0;
                for (int i = start; i < end; i++) {
                    int s = pcm[i * ch + c];
                    int a = s < 0 ? -s : s;
                    if (a > peak) {
                        peak = a;
                        peak_signed = s;
                    }
                }
                samples[p][c] = peak_signed;
            }
            for (int c = ch; c < PCM_MAX_CHANNELS; c++) {
                samples[p][c] = samples[p][0];
            }
        }
        *out_points = points;
        return points > 0 ? 0 : -1;
    }

    if (frame.sample_bits == 8) {
        int total_samples = (int)frame.data_len;
        int total_frames = total_samples / ch;
        if (total_frames <= 0) return -1;

        int points = (total_frames + samples_per_point - 1) / samples_per_point;
        if (points > max_points) points = max_points;
        const unsigned char *pcm = (const unsigned char *)frame.data;

        for (int p = 0; p < points; p++) {
            int start = p * samples_per_point;
            int end = start + samples_per_point;
            if (start >= total_frames) {
                points = p;
                break;
            }
            if (end > total_frames) end = total_frames;
            for (int c = 0; c < ch; c++) {
                int peak = 0;
                int peak_signed = 0;
                for (int i = start; i < end; i++) {
                    int s = (((int)pcm[i * ch + c] - 128) << 8);
                    int a = s < 0 ? -s : s;
                    if (a > peak) {
                        peak = a;
                        peak_signed = s;
                    }
                }
                samples[p][c] = peak_signed;
            }
            for (int c = ch; c < PCM_MAX_CHANNELS; c++) {
                samples[p][c] = samples[p][0];
            }
        }
        *out_points = points;
        return points > 0 ? 0 : -1;
    }

    return -1;
}

static int audio_algo_rebind_pcm_client(void)
{
    if (!context.mm || !context.mm->audio) {
        return -1;
    }

    if (context.pcm_client_handle >= 0 && context.mm->audio->stream_unreg_client) {
        (void)context.mm->audio->stream_unreg_client(context.pcm_client_handle);
    }
    context.pcm_client_handle = -1;
    context.pcm_stream_ready = false;

    if (!context.mm->audio->stream_reg_client) {
        return -1;
    }
    if (context.mm->audio->stream_reg_client(0, &context.pcm_client_handle, pcm_stream_callback, NULL) == 0 &&
        context.pcm_client_handle >= 0) {
        context.pcm_stream_ready = true;
        context.pcm_fetch_fail_count = 0;
        return 0;
    }
    return -1;
}

static long long pcm_now_us(void)
{
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
}

static void pcm_add_event(char type, long long timestamp_ms)
{
    pcm_event_marker_t *ev = &context.pcm_events[context.pcm_event_head];
    ev->type = type;
    ev->timestamp_ms = timestamp_ms;
    ev->sample_index = (context.pcm_head + PCM_POINT_COUNT - 1) % PCM_POINT_COUNT;
    ev->valid = true;
    context.pcm_event_head = (context.pcm_event_head + 1) % PCM_EVENT_MAX;
}

static void pcm_refresh_view(void)
{
    if (!context.pcm_panel || !context.pcm_wave_line[0]) {
        return;
    }

    int width = (int)lv_obj_get_content_width(context.pcm_panel);
    int height = (int)lv_obj_get_content_height(context.pcm_panel);
    if (width <= 4 || height <= 4) {
        return;
    }

    int use_channels = context.pcm_channels > 1 ? 2 : 1;
    for (int ch = 0; ch < PCM_MAX_CHANNELS; ch++) {
        bool show = ch < use_channels;
        if (show) {
            lv_obj_clear_flag(context.pcm_mid_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(context.pcm_wave_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(context.pcm_mid_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(context.pcm_wave_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        int top = (height * ch) / use_channels;
        int bottom = (height * (ch + 1)) / use_channels;
        int band_h = bottom - top;
        int mid_y = top + band_h / 2;
        int visual_gain = pcm_calc_visual_gain(band_h);
        int half = band_h / 2 - 2;
        if (half < 1)
            half = 1;
        context.pcm_mid_points[ch][0].x = 0;
        context.pcm_mid_points[ch][0].y = mid_y;
        context.pcm_mid_points[ch][1].x = width - 1;
        context.pcm_mid_points[ch][1].y = mid_y;
        lv_line_set_points(context.pcm_mid_line[ch], context.pcm_mid_points[ch], 2);

        int draw_points = PCM_POINT_COUNT;
        for (int i = 0; i < draw_points; i++) {
            /* Keep waveform fixed on screen; overwrite oldest sample in-place. */
            int idx = i;
            int x = (i * (width - 1)) / (draw_points - 1);
            int scaled = context.pcm_samples[ch][idx] * visual_gain;
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            int y = mid_y - (scaled * half) / 32768;
            context.pcm_points[ch][i].x = x;
            context.pcm_points[ch][i].y = y;
        }
        if (ch == 0) {
            context.pcm_last_gain = visual_gain;
        }
        lv_line_set_points(context.pcm_wave_line[ch], context.pcm_points[ch], draw_points);
        lv_obj_clear_flag(context.pcm_wave_line[ch], LV_OBJ_FLAG_HIDDEN);

        int recent_n = PCM_RECENT_POINTS;
        if (context.pcm_filled_points < recent_n) recent_n = context.pcm_filled_points;
        if (recent_n > draw_points) recent_n = draw_points;
        if (recent_n >= 2) {
            int start_idx = context.pcm_head - recent_n;
            if (start_idx < 0) start_idx += PCM_POINT_COUNT;
            if (start_idx + recent_n <= PCM_POINT_COUNT) {
                for (int i = 0; i < recent_n; i++) {
                    int src_i = start_idx + i;
                    context.pcm_recent_points[ch][i] = context.pcm_points[ch][src_i];
                }
                lv_line_set_points(context.pcm_recent_line[ch], context.pcm_recent_points[ch], recent_n);
                lv_obj_clear_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
            } else {
                int n1 = PCM_POINT_COUNT - start_idx;
                int n2 = recent_n - n1;
                for (int i = 0; i < n1; i++) {
                    context.pcm_recent_points[ch][i] = context.pcm_points[ch][start_idx + i];
                }
                for (int i = 0; i < n2; i++) {
                    context.pcm_recent_points_wrap[ch][i] = context.pcm_points[ch][i];
                }
                if (n1 >= 2) {
                    lv_line_set_points(context.pcm_recent_line[ch], context.pcm_recent_points[ch], n1);
                    lv_obj_clear_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
                }
                if (n2 >= 2) {
                    lv_line_set_points(context.pcm_recent_line_wrap[ch], context.pcm_recent_points_wrap[ch], n2);
                    lv_obj_clear_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            lv_obj_add_flag(context.pcm_recent_line[ch], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(context.pcm_recent_line_wrap[ch], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < PCM_EVENT_MAX; i++) {
        pcm_event_marker_t *ev = &context.pcm_events[i];
        if (!ev->valid || !ev->label) {
            continue;
        }

        int newest = (context.pcm_head + PCM_POINT_COUNT - 1) % PCM_POINT_COUNT;
        int age = (newest - ev->sample_index + PCM_POINT_COUNT) % PCM_POINT_COUNT;
        if (age >= PCM_POINT_COUNT - 1) {
            lv_obj_add_flag(ev->label, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        int x = (ev->sample_index * (width - 1)) / (PCM_POINT_COUNT - 1);
        lv_label_set_text_fmt(ev->label, "%c", ev->type);
        lv_obj_set_style_text_color(ev->label,
                                    ev->type == 'C' ? lv_palette_main(LV_PALETTE_RED) : lv_palette_main(LV_PALETTE_ORANGE),
                                    0);
        lv_obj_set_style_text_font(ev->label, tablet_font_get_freetype_font(14) ? tablet_font_get_freetype_font(14) : tablet_font_get_montserrat_font(14), 0);
        lv_obj_align(ev->label, LV_ALIGN_TOP_LEFT, x, ev->type == 'C' ? 2 : 16);
        lv_obj_clear_flag(ev->label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_info_label(void)
{
    if (!context.label_info) {
        return;
    }
    const char *status;
    switch (context.init_state) {
    case INIT_STATE_RUNNING:
        status = i18n_text("Init...", "初始化...");
        break;
    case INIT_STATE_PCM_READY:
        status = i18n_text("PCM ready", "PCM就绪");
        break;
    case INIT_STATE_ENABLING_VAD:
        status = i18n_text("VAD...", "开启VAD...");
        break;
    case INIT_STATE_ENABLING_CRY:
        status = i18n_text("Cry...", "开启哭声检测...");
        break;
#if AUDIO_ALGO_SSL_ENABLE
    case INIT_STATE_ENABLING_SSL:
        status = i18n_text("SSL...", "开启声源定位...");
        break;
#endif
    case INIT_STATE_ENABLING_STREAM:
        status = i18n_text("Stream...", "开启音频流...");
        break;
    case INIT_STATE_READY:
        status = i18n_text("Running", "运行中");
        break;
    case INIT_STATE_FAILED:
        status = i18n_text("Failed", "启动失败");
        break;
    default:
        status = i18n_text("Loading...", "加载中...");
        break;
    }
    if (context.init_state == INIT_STATE_FAILED) {
        lv_label_set_text_fmt(context.label_info, "%s open=%d ssl=%d cry=%d vad=%d | due=%d push=%d %lldus",
                              status, context.init_ret_open, context.init_ret_ssl,
                              context.init_ret_cry, context.init_ret_vad,
                              context.pcm_last_due_points, context.pcm_last_push_points,
                              context.pcm_last_fetch_us);
    } else {
        lv_label_set_text_fmt(context.label_info, "%s | gain x%d | due=%d push=%d %lldus",
                              status, context.pcm_last_gain,
                              context.pcm_last_due_points, context.pcm_last_push_points,
                              context.pcm_last_fetch_us);
    }
}

static int init_audio_algorithms(void)
{
    long long init_begin_ms = pcm_now_us() / 1000LL;
    long long stage_begin_ms = init_begin_ms;
    log_info("audio_algo init begin (ssl_enable=%d)", AUDIO_ALGO_SSL_ENABLE);

    if (!context.mm) {
        context.mm = (multimedia_t *)app_main_get_multimedia();
    }
    (void)audio_algo_prepare_pcm_stream(0);

#if AUDIO_ALGO_SSL_ENABLE
    (void)audio_algo_set_default_param(MEDIA_AUDIO_ALGORITHM_SSL, 50);
#endif
    (void)audio_algo_set_default_param(MEDIA_AUDIO_ALGORITHM_CRY, 50);
    (void)audio_algo_set_default_param(MEDIA_AUDIO_ALGORITHM_VAD, 50);

    /*
     * Align with hardware audio test flow:
     * configure algorithm params -> enable algorithms -> open stream once (effects applied on open).
     * Avoid close/open "reopen" choreography in init path.
     */
    int ret_ssl = 0;
    int ret_cry = 0;
    int ret_vad = 0;
    int ret_open = -1;

    context.pcm_client_handle = -1;
    context.pcm_stream_ready = false;
    context.pcm_fetch_fail_count = 0;

    if (context.closing) {
        return -1;
    }

    /* Stage-1: enable VAD */
    if (context.closing) return -1;
    context.init_state = INIT_STATE_ENABLING_VAD;
    stage_begin_ms = pcm_now_us() / 1000LL;
    ret_vad = audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_VAD, 1);
    log_info("audio_algo init stage1 VAD enable elapsed=%lldms ret=%d",
             (pcm_now_us() / 1000LL) - stage_begin_ms, ret_vad);
    context.init_ret_vad = ret_vad;
    if (ret_vad != 0) {
        deinit_audio_algorithms_on_init_fail();
        context.init_state = INIT_STATE_FAILED;
        return -1;
    }

    /* Stage-2: enable NPU cry */
    if (context.closing) return -1;
    context.init_state = INIT_STATE_ENABLING_CRY;
    stage_begin_ms = pcm_now_us() / 1000LL;
    ret_cry = audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_CRY, 1);
    log_info("audio_algo init stage2 CRY enable elapsed=%lldms ret=%d",
             (pcm_now_us() / 1000LL) - stage_begin_ms, ret_cry);
    context.init_ret_cry = ret_cry;
    if (ret_cry != 0) {
        deinit_audio_algorithms_on_init_fail();
        context.init_state = INIT_STATE_FAILED;
        return -1;
    }

#if AUDIO_ALGO_SSL_ENABLE
    /* Stage-3: enable SSL (DMIC / multi-mic boards only) */
    if (context.closing) return -1;
    context.init_state = INIT_STATE_ENABLING_SSL;
    stage_begin_ms = pcm_now_us() / 1000LL;
    ret_ssl = audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_SSL, 1);
    log_info("audio_algo init stage3 SSL enable elapsed=%lldms ret=%d",
             (pcm_now_us() / 1000LL) - stage_begin_ms, ret_ssl);
    context.init_ret_ssl = ret_ssl;
    if (ret_ssl != 0) {
        deinit_audio_algorithms_on_init_fail();
        context.init_state = INIT_STATE_FAILED;
        return -1;
    }
#else
    ret_ssl = 0;
    context.init_ret_ssl = 0;
    log_info("audio_algo init: SSL skipped (CONFIG_BOARD_SUPPORT_DMIC off)");
#endif

    /* Stage-0 (final): open stream once (effects applied on open) */
    if (context.closing) return -1;
    context.init_state = INIT_STATE_ENABLING_STREAM;
    if (context.mm && context.mm->audio && context.mm->audio->stream_enable) {
        stage_begin_ms = pcm_now_us() / 1000LL;
        ret_open = context.mm->audio->stream_enable(0, 1);
        log_info("audio_algo init stage0 stream_enable elapsed=%lldms ret=%d",
                 (pcm_now_us() / 1000LL) - stage_begin_ms, ret_open);
        if (ret_open == 0) {
            stage_begin_ms = pcm_now_us() / 1000LL;
            (void)audio_algo_rebind_pcm_client();
            log_info("audio_algo init stage0 pcm rebind elapsed=%lldms",
                     (pcm_now_us() / 1000LL) - stage_begin_ms);
        }
    } else {
        ret_open = -1;
    }
    context.init_ret_open = ret_open;
    context.init_ret_ssl = ret_ssl;
    context.init_ret_cry = ret_cry;
    context.init_ret_vad = ret_vad;
    if (ret_open != 0) {
        deinit_audio_algorithms_on_init_fail();
        context.init_state = INIT_STATE_FAILED;
        return -1;
    }
    context.init_state = INIT_STATE_PCM_READY;

    context.init_state = INIT_STATE_READY;
    log_info("audio_algo init done total_elapsed=%lldms", (pcm_now_us() / 1000LL) - init_begin_ms);
    return 0;
}

static void deinit_audio_algorithms(void)
{
    if (context.mm && context.mm->audio && context.mm->audio->stream_unreg_client &&
        context.pcm_client_handle >= 0) {
        (void)context.mm->audio->stream_unreg_client(context.pcm_client_handle);
    }
    context.pcm_client_handle = -1;
    context.pcm_stream_ready = false;

#if AUDIO_ALGO_SSL_ENABLE
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_SSL, 0);
#endif
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_CRY, 0);
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_VAD, 0);
    if (context.mm && context.mm->audio && context.mm->audio->stream_enable) {
        (void)context.mm->audio->stream_enable(0, 0);
    }
}

static void deinit_audio_algorithms_on_init_fail(void)
{
    if (!context.mm || !context.mm->audio) {
        return;
    }
    if (context.mm->audio->stream_unreg_client && context.pcm_client_handle >= 0) {
        (void)context.mm->audio->stream_unreg_client(context.pcm_client_handle);
    }
    context.pcm_client_handle = -1;
    context.pcm_stream_ready = false;

#if AUDIO_ALGO_SSL_ENABLE
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_SSL, 0);
#endif
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_CRY, 0);
    (void)audio_algo_set_enable(MEDIA_AUDIO_ALGORITHM_VAD, 0);
    if (context.mm->audio->stream_enable) {
        (void)context.mm->audio->stream_enable(0, 0);
    }
}

lv_result_t tablet_audio_algo_create(lv_obj_t *main)
{
    if (NULL != context.window)
        return LV_RESULT_INVALID;

    /* Preserve alarm mutex across create/destroy cycles (memset would leak/reinit). */
    pthread_mutex_t saved_alarm_mutex;
    bool saved_alarm_mutex_ready = context.alarm_mutex_ready;
    if (saved_alarm_mutex_ready) {
        saved_alarm_mutex = context.alarm_mutex;
    }

    memset(&context, 0, sizeof(context));
    context.main = main;
    context.closing = false;
    context.deinit_done = false;
    if (saved_alarm_mutex_ready) {
        context.alarm_mutex = saved_alarm_mutex;
        context.alarm_mutex_ready = true;
    } else {
        pthread_mutex_init(&context.alarm_mutex, NULL);
        context.alarm_mutex_ready = true;
    }

    lv_obj_t *window = lv_obj_create(lv_display_get_screen_active(lv_display_get_default()));
    lv_obj_add_event_cb(window, audio_algo_window_delete_event, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(window);
    lv_obj_set_size(window, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(window, lv_color_hex(SSL_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, 0);

    tablet_create_top(window, i18n_text("Audio Algorithm", "音频算法"), event_handler_btn_back, main);

    /* Content area below title bar — same structure as settings (副本) */
    lv_obj_t *body = lv_obj_create(window);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(90));
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(SSL_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_radius(body, 0, 0);

    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

#if AUDIO_ALGO_SSL_ENABLE
    (void)create_section_title(body, i18n_text("Sound source localization", "音源定位"));

    lv_obj_t *ssl_card = lv_obj_create(body);
    lv_obj_remove_style_all(ssl_card);
    lv_obj_set_width(ssl_card, LV_PCT(100));
    lv_obj_set_height(ssl_card, 72);
    lv_obj_set_style_bg_color(ssl_card, lv_color_hex(SSL_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(ssl_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ssl_card, 8, 0);
    lv_obj_set_style_radius(ssl_card, 12, 0);
    lv_obj_set_style_border_width(ssl_card, 1, 0);
    lv_obj_set_style_border_color(ssl_card, lv_color_hex(SSL_BORDER), 0);
    context.label_ssl_angle = lv_label_create(ssl_card);
    lv_obj_set_style_text_color(context.label_ssl_angle, lv_color_hex(SSL_TEXT_SEC), 0);
    lv_obj_set_style_bg_color(context.label_ssl_angle, lv_color_hex(SSL_CONTENT_BG), 0);
    lv_obj_set_style_bg_opa(context.label_ssl_angle, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(context.label_ssl_angle, 6, 0);
    lv_obj_set_style_pad_ver(context.label_ssl_angle, 2, 0);
    lv_obj_set_style_radius(context.label_ssl_angle, 6, 0);
    lv_obj_align(context.label_ssl_angle, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_label_set_text(context.label_ssl_angle, i18n_text("Direction: 90", "方向: 90"));

    lv_obj_t *indicator = ssl_sound_slider_init(ssl_card);
    lv_obj_set_size(indicator, LV_PCT(100), 16);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -8);
#else
    /* Single-MIC boards: no SSL UI; still start cry/VAD + PCM timer. */
    audio_algo_start_ui_timer(NULL);
#endif

    lv_obj_t *pcm_title = create_section_title(body, i18n_text("PCM Wave (C=Cry, V=VAD)", "PCM波形（C=哭声，V=异响）"));
    (void)pcm_title;
    (void)create_pcm_card(body);
    pcm_refresh_view();
    /* Single info line below the PCM card: status + gain + timing. */
    context.label_info = lv_label_create(body);
    lv_obj_set_width(context.label_info, LV_PCT(100));
    lv_obj_set_style_text_color(context.label_info, lv_color_hex(SSL_TEXT_SEC), 0);
    lv_font_t *info_font = i18n_font(14, 14);
    if (info_font) {
        lv_obj_set_style_text_font(context.label_info, info_font, 0);
    }
    lv_label_set_long_mode(context.label_info, LV_LABEL_LONG_CLIP);
    update_info_label();

    context.window = window;
    lv_refr_now(lv_display_get_default());
    context.init_state = INIT_STATE_IDLE;
    context.init_ret_open = 0;
    context.init_ret_ssl = 0;
    context.init_ret_cry = 0;
    context.init_ret_vad = 0;
    context.init_thread_handle = 0;
    context.init_thread_reaped = true;
    context.init_timer = lv_timer_create(init_audio_algorithms_timer_cb, 120, NULL);
    return LV_RESULT_OK;
}
