// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "VOLC_ENGINE_APP"

#include "volc_engine_rtclite.h"
#include "file_parser.h"
#include <stdio.h>
#include <arpa/inet.h> /* ntohl for byte_rtc_on_message_received */
//#include <iconv.h>
#include <ctype.h>

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "app_main.h"
#include "multimedia.h"
#include "volc_engine_app.h"

/* Opens AI (mic) capture; without this, audio broadcast has no data and stream_reg_client callbacks never run. */
extern int audio_service_open_input_streams(void);
extern int audio_service_discard_stream_backlog(int stream);
#include "system_config.h"
#include "log.h"

#include <curl/curl.h>
#include "cJSON.h"
#include "volc_engine_authorization.h"
#include "volc_engine_rtc_token.h"
#include "console.h"
#include "notification.h"
#include "system_time.h"
#if CONFIG_NETWORK_SUPPORTED
#include "network.h"
#endif

//#include "mbedtls/x509_crt.h"

#define DEFAULT_FPS_VALUE                   25
#define NUMBER_OF_OPUS_FRAME_FILES          618
#define SAMPLE_AUDIO_FRAME_DURATION         20

#define SIMPLE_CHANENL_NAME                 "xyzz"

#define MAX_APPID_LEN       64
#define MAX_ROOMID_LEN      64
#define MAX_USERID_LEN      64
#define MAX_video_file_LEN   256
#define MAX_audio_file_LEN   256
#define MAX_TOKEN_LEN       256
#define MAX_PATH_LEN        1024
#define MAX_COMMON_LEN      64

/* 下行与旧版一致：A-law/μ-law 解码成 PCM 后直接 talk_send_data。
 * 不再做软增益，也不调用 out_set_volume(100)（100% 会写成 +20 dB，盖掉
 * audio_config.ini [def_far] 的 vol_dB，喇叭削波后 AEC 消不干净）。 */

/* 只打开喇叭，不把麦克风切进对讲/AEC。上行仍是采集编码回调里的原始码流。 */
extern int audio_service_playback_start(int dev);

char*  volc_url = "https://rtc.volcengineapi.com?Action=StartVoiceChat&Version=2024-12-01";


// A律解码表 (ITU-T G.711标准)
static const int16_t alaw_table[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
    -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
    -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
    -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
    -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
    -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
    -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
    -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
    5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
    7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
    2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
    3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
    22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
    11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
    15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
    344,   328,   376,   360,   280,   264,   312,   296,
    472,   456,   504,   488,   408,   392,   440,   424,
    88,    72,    120,   104,    24,     8,    56,    40,
    216,   200,   248,   232,   152,   136,   184,   168,
    1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
    1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
    688,   656,   752,   720,   560,   528,   624,   592,
    944,   912,   1008,  976,   816,   784,   880,   848
};

typedef struct
{
    int video_handle;
    int audio_handle;
    int video_frame_rate;
    int video_bitrate_kbps;
    int video_width;
    int video_height;

    audio_data_type_e audio_codec;
    multimedia_t *multimedia;

}stream_mgr_t;

stream_mgr_t live_stream_mgr = {0};

/* Last non-empty H264 payload size for zero-length fallback when bitrate unknown. */
static size_t s_volc_last_nonempty_video_len = 1024;

/* ini token fallback when local generate fails */
static char s_rtc_token_fallback[MAX_TOKEN_LEN];

typedef struct {
    char app_id[MAX_APPID_LEN];
    char room_id[MAX_ROOMID_LEN];
    char user_id[MAX_USERID_LEN];
    char video_file[MAX_video_file_LEN];
    char audio_file[MAX_audio_file_LEN];
    char token[MAX_TOKEN_LEN];
    char end_point_id[128];

    bool channel_joined;
    bool is_send_video;
    bool is_send_audio;
//    pthread_t  send_video_thread_id;
//    pthread_t  send_audio_thread_id;
    int audio_codec;
    int fps;
    byte_rtc_engine_t engine;
    bool fini_notifyed;

    //http 返回数据
    char *data;
    int size;
} byte_rtc_data;

typedef struct
{
	char task_id[MAX_COMMON_LEN];
// 鉴权 AK/SK。前往 https://console.volcengine.com/iam/keymanage 获取
	char sk[MAX_COMMON_LEN];
	char ak[MAX_COMMON_LEN];

// 实时音视App ID。前往 https://console.volcengine.com/rtc/listRTC 获取或创
	char rtc_app_id[MAX_COMMON_LEN];

// 实时音视APP KEY。前往 https://console.volcengine.com/rtc/listRTC 获取
	char rtc_app_key[MAX_COMMON_LEN];

// 大模型推理接入点 EndPointId 前往 https://console.volcengine.com/ark/region:ark+cn-beijing/endpoint?config=%7B%7D 创建
	char default_end_point_id[MAX_COMMON_LEN];

// 音频生成-语音合成 Voice_type，前往 https://console.volcengine.com/speech/service/8 获取
	char default_voice_type[MAX_COMMON_LEN];

// 语音识别-流式语音识别 APPID 前往 https://console.volcengine.com/speech/service/16 获取
	char asr_app_id[MAX_COMMON_LEN];

// 语音识别-流式语音识别 ACCESS TOKEN 前往 https://console.volcengine.com/speech/service/16 获取
	char asr_access_token[MAX_COMMON_LEN];

// 音频生成-语音合成 APPID，前往 https://console.volcengine.com/speech/service/8 获取
	char tts_app_id[MAX_COMMON_LEN];

// 音频生成-语音合成 ACCESS TOKEN，前往 https://console.volcengine.com/speech/service/8 获取
	char tts_access_token[MAX_COMMON_LEN];

// volc engine account id
	char account_id[MAX_COMMON_LEN];

	/* AgentConfig.DuplexMode: "full" | "half" */
	char duplex_mode[16];
	int enable_vad_interrupt;
	/* ASR VAD 静默判停(ms)，用户停说后触发整句识别送 LLM */
	int asr_silence_time_ms;
	/* LLM VisionConfig.Enable：0 关闭截图多模态（排障用） */
	int vision_enable;

}byte_rtc_config;

byte_rtc_config g_byte_rtc_config = {0};

byte_rtc_data g_byte_rtc_data = {0};

/* Same lifecycle as celestia_old: full init once; re-entry only restarts media send. */
static int g_volc_engine_inited = 0;
static int g_volc_engine_play = 1;
/* 按键说话（PTT）：0=上行静音帧（默认），1=正常采样。见 volc_engine_app.h。 */
static volatile int s_volc_voice_enable = 0;
/* 上行画面门控：流注册保持，仅 send_video 回调内丢弃。默认关（UI 默认文本模式）。 */
static volatile int s_volc_video_enable = 0;
/* Half-duplex gate: suppress uplink only while bot TTS has real speech (not comfort-noise RTP). */
static volatile int s_volc_bot_audio_active = 0;
static volatile uint64_t s_volc_bot_audio_last_ms = 0;
#define VOLC_BOT_UPLINK_HOLD_MS       350
#define VOLC_BOT_SPEECH_PEAK_THRESH   800
#define VOLC_BOT_SILENCE_PEAK_THRESH  64
/* Soft syllables (e.g. 小凯 / 到了) often sit ~400-900 after NR; used for diag only.
 * Exp6: match 05_orig_volc — do NOT gate uplink with near_sil (orig just sent). */
#define VOLC_VOICE_PCM_PEAK_THRESH    600

static int volc_agent_full_duplex(void)
{
    return (g_byte_rtc_config.duplex_mode[0] == '\0' ||
            strcmp(g_byte_rtc_config.duplex_mode, "full") == 0);
}

static int volc_uplink_bot_gate_blocks(void)
{
    if (volc_agent_full_duplex())
        return 0;
    if (!s_volc_bot_audio_active)
        return 0;
    uint64_t now_ms = (uint64_t)system_get_sys_run_time64();
    if (now_ms > s_volc_bot_audio_last_ms &&
        (now_ms - s_volc_bot_audio_last_ms) > VOLC_BOT_UPLINK_HOLD_MS) {
        s_volc_bot_audio_active = 0;
        return 0;
    }
    return 1;
}

/* deinit 首行置 0，init 成功末行置 1：避免退出后 RTC 仍回调导致打 log / 塞满通知队列 */
static volatile int s_volc_rtc_callbacks_ok = 0;
static volatile int s_volc_deinit_in_progress = 0;

char *volc_work_path = "/etc/config/volc/";

/* volc config path built at load via config_build_file_path("app_volc_config.ini", ...) */

int debug_print = 0;

volc_engine_recv_text_callback_t volc_recv_text_cb = NULL;

void *byte_rtc_send_text(void *param);

/*
 * Do not gate RTC on TCP to 8.8.8.8:443: ICMP ping can succeed while this fails (RST/refused,
 * firewall, or 2s too short on first connect). Volc traffic goes to rtc.volcengineapi.com, not 8.8.8.8.
 */

// PCMA转PCM函数
void pcma_to_pcm(const uint8_t* input, int16_t* output, size_t length) {
    for (size_t i = 0; i < length; i++) {
        output[i] = alaw_table[input[i]];
    }
}

/*
 * Echo: rely on SDK libakaudiofilter AEC ([volc_mic] aecEna + [def_far] limit).
 * App mute is disabled (unreliable). Far ring + corr_* logs stay for board diag:
 * after Exp1 (limit=0.15FS), watch uplink pcm_peak — should stay below clip.
 */
#define VOLC_FAR_RING               8000  /* 1.0 s @ 8 kHz; delay search needs ~100 ms, not full TTS */
#define VOLC_FAR_SPEECH_HOLD_MS     450
#define VOLC_FAR_SPEECH_PEAK_MIN    600
#define VOLC_ECHO_CLIP_NEAR_PEAK    14000 /* diag only; mute path off */
#define VOLC_FAR_SILENCE_ABS        64
/* 0 = SDK AEC only (Exp1+). Set 1 only for temporary mute_clip fallback. */
#define VOLC_APP_ECHO_MUTE_ENA      0

static int16_t s_volc_far_ring[VOLC_FAR_RING];
static unsigned s_volc_far_w;
static int s_volc_far_filled;
static volatile int s_volc_far_speech_peak;
static volatile uint64_t s_volc_far_speech_last_ms;

/*
 * Downlink pending ring: keep ~1.2 s of 8 kHz PCM while AO is still opening.
 * Welcome TTS often arrives in the join→AO-ready window; without this the head
 * ("你好呀…") is dropped and only the tail ("小凯") may play.
 */
#define VOLC_DL_PEND_CAP     9600
#define VOLC_DL_FLUSH_CHUNK  320

static int16_t s_volc_dl_pend[VOLC_DL_PEND_CAP];
static unsigned s_volc_dl_pend_r;
static unsigned s_volc_dl_pend_n;
static pthread_mutex_t s_volc_dl_pend_mu = PTHREAD_MUTEX_INITIALIZER;

static void volc_dl_pend_clear(void)
{
    pthread_mutex_lock(&s_volc_dl_pend_mu);
    s_volc_dl_pend_r = 0;
    s_volc_dl_pend_n = 0;
    pthread_mutex_unlock(&s_volc_dl_pend_mu);
}

static void volc_dl_pend_push(const int16_t *pcm, size_t n)
{
    if (pcm == NULL || n == 0)
        return;

    pthread_mutex_lock(&s_volc_dl_pend_mu);
    for (size_t i = 0; i < n; i++) {
        if (s_volc_dl_pend_n >= VOLC_DL_PEND_CAP) {
            s_volc_dl_pend_r = (s_volc_dl_pend_r + 1u) % VOLC_DL_PEND_CAP;
            s_volc_dl_pend_n--;
        }
        unsigned w = (s_volc_dl_pend_r + s_volc_dl_pend_n) % VOLC_DL_PEND_CAP;
        s_volc_dl_pend[w] = pcm[i];
        s_volc_dl_pend_n++;
    }
    pthread_mutex_unlock(&s_volc_dl_pend_mu);
}

static void volc_dl_pend_flush(void)
{
    if (!live_stream_mgr.multimedia || !live_stream_mgr.multimedia->audio ||
        !live_stream_mgr.multimedia->audio->talk_send_data) {
        return;
    }

    int16_t chunk[VOLC_DL_FLUSH_CHUNK];
    unsigned flushed = 0;

    for (;;) {
        size_t n = 0;

        pthread_mutex_lock(&s_volc_dl_pend_mu);
        if (s_volc_dl_pend_n == 0) {
            pthread_mutex_unlock(&s_volc_dl_pend_mu);
            break;
        }
        n = s_volc_dl_pend_n;
        if (n > VOLC_DL_FLUSH_CHUNK)
            n = VOLC_DL_FLUSH_CHUNK;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = s_volc_dl_pend[s_volc_dl_pend_r];
            s_volc_dl_pend_r = (s_volc_dl_pend_r + 1u) % VOLC_DL_PEND_CAP;
            s_volc_dl_pend_n--;
        }
        pthread_mutex_unlock(&s_volc_dl_pend_mu);

        media_frame_info_t frame = {0};
        frame.data = (unsigned char *)chunk;
        frame.data_len = (unsigned int)(n * sizeof(int16_t));
        frame.code_type = MEDIA_CODEC_AUDIO_PCM;
        frame.sample_rate = 8000;
        frame.sample_bits = 16;
        frame.channel_num = 1;

        int ret = live_stream_mgr.multimedia->audio->talk_send_data(0, &frame);
        if (ret != MULTIMEDIA_OK) {
            /* Restore this chunk to the front so order stays FIFO. */
            pthread_mutex_lock(&s_volc_dl_pend_mu);
            for (size_t i = n; i > 0; i--) {
                if (s_volc_dl_pend_n >= VOLC_DL_PEND_CAP)
                    break;
                s_volc_dl_pend_r = (s_volc_dl_pend_r + VOLC_DL_PEND_CAP - 1u) % VOLC_DL_PEND_CAP;
                s_volc_dl_pend[s_volc_dl_pend_r] = chunk[i - 1];
                s_volc_dl_pend_n++;
            }
            unsigned left = s_volc_dl_pend_n;
            pthread_mutex_unlock(&s_volc_dl_pend_mu);
            log_warn("volc: dl_pend flush stop ret=%d flushed=%u left=%u",
                     ret, flushed, left);
            break;
        }
        flushed += (unsigned)n;
    }

    if (flushed > 0) {
        log_info("volc: dl_pend flushed %u samples (~%ums)",
                 flushed, flushed * 1000u / 8000u);
    }
}

static void volc_far_push(const int16_t *pcm, size_t n)
{
    size_t i;
    int peak = 0;

    for (i = 0; i < n; i++) {
        int v = pcm[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
        s_volc_far_ring[s_volc_far_w] = pcm[i];
        s_volc_far_w++;
        if (s_volc_far_w >= VOLC_FAR_RING)
            s_volc_far_w = 0;
        if (s_volc_far_filled < VOLC_FAR_RING)
            s_volc_far_filled++;
    }
    if (peak >= VOLC_FAR_SPEECH_PEAK_MIN) {
        s_volc_far_speech_peak = peak;
        s_volc_far_speech_last_ms = (uint64_t)system_get_sys_run_time64();
    }
}

static int16_t volc_far_ago(unsigned ago)
{
    unsigned idx = (s_volc_far_w + VOLC_FAR_RING - 1u - ago) % VOLC_FAR_RING;
    return s_volc_far_ring[idx];
}

static int volc_far_speech_recent(void)
{
    uint64_t now = (uint64_t)system_get_sys_run_time64();
    uint64_t last = s_volc_far_speech_last_ms;

    if (last == 0)
        return 0;
    if (now >= last && (now - last) <= VOLC_FAR_SPEECH_HOLD_MS)
        return 1;
    return 0;
}

/* Silence ratio in the whole far ring (0..100). Comfort-noise frames inflate this. */
static int volc_far_silence_pct(void)
{
    int i;
    int silent = 0;
    int n = s_volc_far_filled;

    if (n <= 0)
        return 100;
    for (i = 0; i < n; i++) {
        int v = s_volc_far_ring[i];
        if (v < 0)
            v = -v;
        if (v <= VOLC_FAR_SILENCE_ABS)
            silent++;
    }
    return (silent * 100) / n;
}

/*
 * Measure best delay + normalized correlation (diag only).
 * corr_pct ≈ 100 * (dot^2) / (En * Ef). Real echo often needs >20 to subtract safely;
 * clipped mic vs clean far typically stays near 0.
 */
static void volc_echo_corr_diag(const int16_t *near, int n, int *delay_out, int *corr_pct_out,
                                int *far_peak_out, int *near_clip_pct_out)
{
    int64_t energy_n = 0;
    int dmax;
    int d;
    int best_d = -1;
    int64_t best_dot = 0;
    int64_t best_ef = 1;
    int i;
    int clip = 0;
    int far_peak = 0;

    *delay_out = -1;
    *corr_pct_out = 0;
    *far_peak_out = 0;
    *near_clip_pct_out = 0;

    if (n <= 0 || n > 480 || s_volc_far_filled < n + 64)
        return;

    for (i = 0; i < n; i++) {
        int v = near[i];
        if (v < 0)
            v = -v;
        if (v >= 28000)
            clip++;
        energy_n += (int64_t)near[i] * (int64_t)near[i];
    }
    *near_clip_pct_out = (clip * 100) / n;
    if (energy_n < (int64_t)n * 200 * 200)
        return;

    dmax = s_volc_far_filled - n;
    if (dmax > 4000)
        dmax = 4000; /* search up to 500 ms */
    for (d = 16; d <= dmax; d += 4) {
        int64_t dot = 0;
        int64_t ef = 0;
        for (i = 0; i < n; i += 2) {
            int16_t f = volc_far_ago((unsigned)(d + (n - 1 - i)));
            dot += (int64_t)near[i] * (int64_t)f;
            ef += (int64_t)f * (int64_t)f;
        }
        if (dot <= 0 || ef < 2000)
            continue;
        if (best_d < 0 || dot * 256 / ef > best_dot * 256 / best_ef) {
            best_d = d;
            best_dot = dot;
            best_ef = ef;
        }
    }
    if (best_d < 0)
        return;

    {
        int64_t dot = 0;
        int64_t ef = 0;
        for (i = 0; i < n; i++) {
            int16_t f = volc_far_ago((unsigned)(best_d + (n - 1 - i)));
            int fv = f;
            if (fv < 0)
                fv = -fv;
            if (fv > far_peak)
                far_peak = fv;
            dot += (int64_t)near[i] * (int64_t)f;
            ef += (int64_t)f * (int64_t)f;
        }
        *delay_out = best_d;
        *far_peak_out = far_peak;
        if (energy_n > 0 && ef > 0)
            *corr_pct_out = (int)((100.0 * (double)dot * (double)dot) /
                                  ((double)energy_n * (double)ef));
    }
}

/* Returns 1 if uplink should be silenced. Disabled while evaluating SDK AEC. */
static int volc_echo_should_mute_uplink(int near_peak, const char **reason)
{
#if !VOLC_APP_ECHO_MUTE_ENA
    if (reason) {
        if (!volc_far_speech_recent())
            *reason = "idle";
        else if (near_peak >= VOLC_ECHO_CLIP_NEAR_PEAK)
            *reason = "sdk_aec_clip";
        else if (near_peak >= 1000)
            *reason = "sdk_aec_barge";
        else
            *reason = "sdk_aec_quiet";
    }
    return 0;
#else
    if (!volc_far_speech_recent()) {
        if (reason)
            *reason = "idle";
        return 0;
    }
    if (near_peak >= VOLC_ECHO_CLIP_NEAR_PEAK) {
        if (reason)
            *reason = "mute_clip";
        return 1;
    }
    if (reason)
        *reason = (near_peak >= 1000) ? "pass_barge" : "pass_quiet";
    return 0;
#endif
}

static void volc_fill_pcma_silence(uint8_t *dst, size_t n)
{
    memset(dst, 0xD5, n);
}

/* ITU G.711 μ-law 编码（与 pcmu_to_pcm_one 配对） */
static uint8_t pcm16_to_pcmu(int16_t pcm_val)
{
    int32_t sample = (int32_t)pcm_val;
    uint8_t sign = 0;
    if (sample < 0) {
        sign = 0x80;
        sample = (sample == -32768) ? 32767 : -sample;
    }
    if (sample > 32635) {
        sample = 32635;
    }
    sample += 132;
    int exponent = 7;
    for (int exp_mask = 0x4000; (sample & exp_mask) == 0 && exponent > 0; exponent--, exp_mask >>= 1) {
    }
    int mantissa = (int)((sample >> (exponent + 3)) & 0x0F);
    return (uint8_t)(~(sign | (uint8_t)(exponent << 4) | (uint8_t)mantissa));
}

/* μ-law (PCMU) to signed 16-bit PCM */
static int16_t pcmu_to_pcm_one(uint8_t u_val)
{
    u_val = (uint8_t)~u_val;
    int sign = (u_val & 0x80) ? -1 : 1;
    int exponent = (u_val >> 4) & 0x07;
    int mantissa = u_val & 0x0F;
    int t = ((mantissa << 3) + 0x84) << exponent;
    int pcm = sign * (t - 0x84);
    if (pcm > 32767) pcm = 32767;
    if (pcm < -32768) pcm = -32768;
    return (int16_t)pcm;
}

static void pcmu_to_pcm(const uint8_t* input, int16_t* output, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        output[i] = pcmu_to_pcm_one(input[i]);
    }
}
static int byte_cjson_get_string(cJSON *object, const char *pString, char *pDst,const int iDstSize)
{
	if (object == NULL || pString == NULL || pDst == NULL || iDstSize < 0)
	{
		return -1;
	}
    
	cJSON * pParam = cJSON_GetObjectItem(object, pString);
    if (pParam == NULL)
    {
        return -1;
    }
    
	if (NULL == pParam->valuestring)
    {   
		return -1;
    }
    
	if (NULL == strncpy(pDst, pParam->valuestring, iDstSize -1))
	{
		return -1;
	}
    
	return 0;
}

static int byte_rtc_parser_json_to_param()
{
    int ret = 0;   
    char *json = g_byte_rtc_data.data;
    cJSON *pRoot = NULL;
    pRoot = cJSON_Parse(json);
    if(pRoot == NULL)
    {
        printf("json=%s error.\n",json);
        return -1;
    }
    int code_value = 0;
    cJSON * code = cJSON_GetObjectItem(pRoot, "code");
    if(code)
    {
        code_value  = code->valueint;
    }
    else
    {
        
        printf("parser code error.");
        ret = -1;
        goto lable;
    }
    if(code_value != 200)
    {
         printf("json:%s", json);
         ret = -1;
         goto lable;
    }
    cJSON * data = cJSON_GetObjectItem(pRoot, "data");

    byte_cjson_get_string(data,"room_id",g_byte_rtc_data.room_id,sizeof(g_byte_rtc_data.room_id));

    cJSON * uid = cJSON_GetObjectItem(data, "uid");
    (void)uid;
    //byte_cjson_get_string(data,"uid",g_byte_rtc_data.user_id,sizeof(g_byte_rtc_data.user_id));
    //log_warn("g_byte_rtc_data.user_id:%s", g_byte_rtc_data.user_id);

    byte_cjson_get_string(data,"app_id",g_byte_rtc_data.app_id,sizeof(g_byte_rtc_data.app_id));

    byte_cjson_get_string(data,"token",g_byte_rtc_data.token,sizeof(g_byte_rtc_data.token));

lable:
    cJSON_Delete(pRoot);

    return ret;
}

int volc_engine_set_text_cb(volc_engine_recv_text_callback_t cb)
{
	volc_recv_text_cb = cb;

	return 0;
}

/*
 * PTT barge-in. Official Conversational AI control frame (RTC mode):
 *   "ctrl" + big-endian payload length + {"Command":"interrupt"}
 * Sent to AgentConfig.UserId so the cloud stops TTS at the source.
 * Local AO mute stays as a backstop for packets already in flight.
 */
static void volc_ptt_interrupt_bot(void)
{
	static const char payload[] = "{\"Command\":\"interrupt\"}";
	unsigned char msg[8 + sizeof(payload)];
	size_t plen = sizeof(payload) - 1;
	int64_t mid;

	if (!g_byte_rtc_data.channel_joined || g_byte_rtc_data.engine == NULL ||
	    g_byte_rtc_data.room_id[0] == '\0') {
		log_warn("volc ptt interrupt: not in room");
		return;
	}

	memcpy(msg, "ctrl", 4);
	msg[4] = (unsigned char)((plen >> 24) & 0xff);
	msg[5] = (unsigned char)((plen >> 16) & 0xff);
	msg[6] = (unsigned char)((plen >> 8) & 0xff);
	msg[7] = (unsigned char)(plen & 0xff);
	memcpy(msg + 8, payload, plen);

	mid = byte_rtc_rts_send_message(g_byte_rtc_data.engine,
	                                g_byte_rtc_data.room_id,
	                                "botuser",
	                                msg, 8 + plen,
	                                true, RTS_MESSAGE_RELIABLE);
	log_info("volc ptt interrupt: rts msgid=%lld room=%s target=botuser",
	         (long long)mid, g_byte_rtc_data.room_id);
}

int volc_engine_set_voice_enable(int enable)
{
	int old = s_volc_voice_enable;

	s_volc_voice_enable = enable ? 1 : 0;
	if (old != s_volc_voice_enable) {
		log_info("volc voice_enable %d -> %d", old, s_volc_voice_enable);
		/* PTT half-duplex: stop feeding AO while user talks so SPK does not
		 * re-enter the mic and poison volcano ASR (duplex=full still plays
		 * bot TTS into the room otherwise). Far-ring is cleared below in the
		 * audio callback path once mute is observed. */
		if (s_volc_voice_enable) {
			s_volc_bot_audio_active = 0;
			s_volc_far_speech_peak = 0;
			/* Tell the agent to stop the current utterance upstream. */
			volc_ptt_interrupt_bot();
		}
	}
	return 0;
}

int volc_engine_get_voice_enable(void)
{
	return s_volc_voice_enable;
}

int volc_engine_set_video_enable(int enable)
{
	int old = s_volc_video_enable;
	/* Cloud VisionConfig.Enable=1 needs H.264 uplink even in UI text mode;
	 * otherwise bot says "没收到画面" and may restart the agent. */
	int want = (enable || g_byte_rtc_config.vision_enable) ? 1 : 0;

	s_volc_video_enable = want;
	if (old != s_volc_video_enable)
		log_info("volc video_enable %d -> %d (ui=%d vision=%d)",
		         old, s_volc_video_enable, enable ? 1 : 0,
		         g_byte_rtc_config.vision_enable);
	return 0;
}

int volc_engine_bot_speaking(void)
{
	return s_volc_bot_audio_active ? 1 : 0;
}

static size_t byte_rtc_perform_req_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    byte_rtc_data *response = (byte_rtc_data *)userp;
    if(response->data == NULL)
    {
        response->data = malloc(1024);
    }

    char *ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        printf("Memory allocation failed\n");
        return 0;
    }

    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = '\0';

    return response->size;
}

static int byte_rtc_config_to_json(byte_rtc_config *config,byte_rtc_data *data, char *req_json)
{

	/* 1. Create root object */
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		fprintf(stderr, "cJSON_CreateObject failed\n");
		return -1;
	}


	/* 2. Add AppId, RoomId, TaskId */
	cJSON_AddStringToObject(root, "AppId", data->app_id);
	cJSON_AddStringToObject(root, "RoomId", data->room_id);
	{
		const char *task_id = (config->task_id[0] != '\0') ? config->task_id : "task1";
		cJSON_AddStringToObject(root, "TaskId", task_id);
	}

	/* 3. Create Config object */
	cJSON *config_obj = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "Config", config_obj);

	/* 4. Create ASRConfig */
	cJSON *asr_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(asr_obj, "Provider", "volcano");

	cJSON *asr_params = cJSON_CreateObject();
	cJSON_AddStringToObject(asr_params, "Mode", "smallmodel");
	cJSON_AddStringToObject(asr_params, "AppId", config->asr_app_id);
	cJSON_AddStringToObject(asr_params, "Cluster", "volcengine_streaming_common");
	{
		int silence_ms = (config->asr_silence_time_ms > 0) ? config->asr_silence_time_ms : 800;
		cJSON *vad_obj = cJSON_CreateObject();
		cJSON_AddNumberToObject(vad_obj, "SilenceTime", silence_ms);
		cJSON_AddItemToObject(asr_params, "VADConfig", vad_obj);
	}
	cJSON_AddItemToObject(asr_obj, "ProviderParams", asr_params);
	cJSON_AddItemToObject(config_obj, "ASRConfig", asr_obj);

	/* 5. Create TTSConfig */
	cJSON *tts_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(tts_obj, "Provider", "volcano");

	cJSON *tts_params = cJSON_CreateObject();

	// IgnoreBracketText: [1, 2]
	cJSON *ignore_array = cJSON_CreateArray();
	cJSON_AddItemToArray(ignore_array, cJSON_CreateNumber(1));
	cJSON_AddItemToArray(ignore_array, cJSON_CreateNumber(2));
	cJSON_AddItemToObject(tts_params, "IgnoreBracketText", ignore_array);

	/* app object */
	cJSON *app_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(app_obj, "appid", config->tts_app_id);
	cJSON_AddStringToObject(app_obj, "cluster", "volcano_tts");
	cJSON_AddStringToObject(app_obj, "token", config->tts_access_token);
	cJSON_AddItemToObject(tts_params, "app", app_obj);

	/* audio object */
	cJSON *audio_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(audio_obj, "voice_type", "BV002_streaming");
	cJSON_AddNumberToObject(audio_obj, "speech_ratio", 1.0);
	cJSON_AddNumberToObject(audio_obj, "volume_ratio", 1.0);
	cJSON_AddNumberToObject(audio_obj, "pitch_ratio", 1.0);
	cJSON_AddItemToObject(tts_params, "audio", audio_obj);

	cJSON_AddItemToObject(tts_obj, "ProviderParams", tts_params);
	cJSON_AddItemToObject(config_obj, "TTSConfig", tts_obj);

	/* 6. Create LLMConfig */
	cJSON *llm_obj = cJSON_CreateObject();
	/* 本端点 ep 使用 ArkV3；勿改为 seed（曾误判，实测 ArkV3 可正常对话） */
	cJSON_AddStringToObject(llm_obj, "Mode", "ArkV3");
	cJSON_AddStringToObject(llm_obj, "EndPointId", config->default_end_point_id);
	cJSON_AddNumberToObject(llm_obj, "MaxTokens", 1024);
	cJSON_AddNumberToObject(llm_obj, "Temperature", 0.1);
	cJSON_AddNumberToObject(llm_obj, "TopP", 0.3);

	/* SystemMessages array */
	cJSON *sys_msgs = cJSON_CreateArray();
//	cJSON_AddItemToArray(sys_msgs, cJSON_CreateString(
//		"你是小宁，性格幽默又善解人意。你在表达时需简明扼要，有自己的观点
//	));
	cJSON_AddItemToArray(sys_msgs, cJSON_CreateString(
			"# 角色\n"
			"你是一个智能硬件小助手，能为客户提供情感陪伴和助手服务\n"
			"# 信息获取\n"
			"用户的语音会被转成文本提供给你，同时用户摄像头拍摄的视频会被转成图片发送给你，你可以结合这些图片感知用户的场景，识别各种目标\n"
			"# 回复形式\n"
			"你的回复文本会被转成语音发送给用户\n"
			"# 任务要求\n"
			"## 情感陪伴\n"
			"- **态度友好**：对话语气要亲切、热情，让用户感受到温暖和关怀，展现出积极的陪伴态度\n"
			"- **理解共情**：认真倾听用户的话语，理解用户的情感状态，对用户表达的喜怒哀乐给予共情回应，让用户感到被理解和支持\n"
			"- **话题引导**：当对话陷入沉默或用户情绪低落时，主动发起有趣的话题，引导对话继续，帮助用户缓解负面情绪，提升积极情绪\n"
			"## 助手能力\n"
			"- **精准响应**：根据用户的需求和问题，结合图片信息，提供准确、有用的回答和解决方案\n"
			"- **高效服务**：快速处理用户的请求，不拖延，确保及时满足用户的需求\n"
			"- **持续学习**：不断积累知识和经验，提升自身的服务能力，以便更好地为用户提供多样化的帮助\n"
			"# 交流原则\n"
			"- **自然流畅**：回复内容要符合日常交流的习惯，语言表达自然、通顺，避免出现生硬、机械的表述\n"
			"- **简洁明了**：回答问题要简洁直接，避免冗长复杂的语句，让用户能够快速理解你的意图\n"
			"- **尊重隐私**：严格保护用户的个人信息和隐私，不泄露用户的任何敏感信息"
		));


	cJSON_AddItemToObject(llm_obj, "SystemMessages", sys_msgs);

	/* UserPrompts array */
	cJSON *user_prompts = cJSON_CreateArray();

	/* First prompt: user -> "hello" */
	cJSON *prompt1 = cJSON_CreateObject();
	cJSON_AddStringToObject(prompt1, "Role", "user");
	cJSON_AddStringToObject(prompt1, "Content", "你好");
	cJSON_AddItemToArray(user_prompts, prompt1);

	/* Second prompt: assistant reply */
	cJSON *prompt2 = cJSON_CreateObject();
	cJSON_AddStringToObject(prompt2, "Role", "assistant");
	cJSON_AddStringToObject(prompt2, "Content", "有什么可以帮到你的？");
	cJSON_AddItemToArray(user_prompts, prompt2);

	cJSON_AddItemToObject(llm_obj, "UserPrompts", user_prompts);

	// HistoryLength
	cJSON_AddNumberToObject(llm_obj, "HistoryLength", 3);

	cJSON_AddItemToObject(config_obj, "LLMConfig", llm_obj);


    /* 7. Create VisionConfig */
    cJSON *vision_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(llm_obj, "VisionConfig", vision_obj);

    /*
     * Vision 开关由 app_volc_config.ini 的 vision_enable 控制（1=开，0=关）。
     * 对照测试曾临时硬编码关闭，已恢复 ini 驱动。
     */
    cJSON_AddBoolToObject(vision_obj, "Enable", config->vision_enable ? 1 : 0);
    /* 对照测试临时关闭：cJSON_AddBoolToObject(vision_obj, "Enable", 0); */

    // VisionConfig.SnapshotConfig
    cJSON *snap_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(vision_obj, "SnapshotConfig", snap_obj);

    cJSON_AddNumberToObject(snap_obj, "StreamType", 0);
    cJSON_AddStringToObject(snap_obj, "ImageDetail", "low");
    cJSON_AddNumberToObject(snap_obj, "Height", 360);
    cJSON_AddNumberToObject(snap_obj, "Interval", 2000);
    cJSON_AddNumberToObject(snap_obj, "ImagesLimit", 2);

    // VisionConfig.StorageConfig
    cJSON *storage_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(vision_obj, "StorageConfig", storage_obj);

	//0:save to cashc;1:save to server
    cJSON_AddNumberToObject(storage_obj, "Type", 0);

    // StorageConfig.TosConfig
    cJSON *tos_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(storage_obj, "TosConfig", tos_obj);

    cJSON_AddStringToObject(tos_obj, "AccountId", config->account_id);
    cJSON_AddNumberToObject(tos_obj, "Region", 0);
    cJSON_AddStringToObject(tos_obj, "Bucket", "visionbucket");

	/* 8. Create AgentConfig */
	cJSON *agent_obj = cJSON_CreateObject();

	/* TargetUserId array */
	cJSON *target_ids = cJSON_CreateArray();
	cJSON_AddItemToArray(target_ids, cJSON_CreateString(data->user_id));
	cJSON_AddItemToObject(agent_obj, "TargetUserId", target_ids);

	cJSON_AddStringToObject(agent_obj, "WelcomeMessage", "你好呀，我是小凯");
	cJSON_AddStringToObject(agent_obj, "UserId", "botuser");
	{
		const char *duplex = (config->duplex_mode[0] != '\0') ? config->duplex_mode : "full";
		cJSON_AddStringToObject(agent_obj, "DuplexMode", duplex);
		if (config->enable_vad_interrupt) {
			cJSON_AddBoolToObject(agent_obj, "EnableVadInterrupt", 1);
		}
	}

	cJSON_AddItemToObject(root, "AgentConfig", agent_obj);

	/* 9. Print JSON string */
	char *json_string = cJSON_Print(root);
	if (!json_string) {
		fprintf(stderr, "cJSON_Print failed\n");
		cJSON_Delete(root);
		return -1;
	}

	/* 10. Free JSON object */
	cJSON_Delete(root);

	if (json_string)
	{
		strcpy(req_json,json_string);
		free(json_string);
	}

	return 0;
}

static int byte_rtc_init_default_data()
{
    char room_id[64] = {0};
    char volc_cfg_path[256];
    if (config_build_file_path("app_volc_config.ini", volc_cfg_path, sizeof(volc_cfg_path)) != 0) {
        return -1;
    }
    dictionary * handle = iniparser_load(volc_cfg_path);
    if (!handle) {
        return -1;
    }

    snprintf(g_byte_rtc_data.app_id, sizeof(g_byte_rtc_data.app_id), "%s",
             iniparser_dict_getstring(handle, "volc", "app_id", ""));

    snprintf(room_id, sizeof(room_id), "%s",
             iniparser_dict_getstring(handle, "volc", "room_id", ""));

    snprintf(g_byte_rtc_data.user_id, sizeof(g_byte_rtc_data.user_id), "%s",
             iniparser_dict_getstring(handle, "volc", "user_id", ""));

    snprintf(g_byte_rtc_data.token, sizeof(g_byte_rtc_data.token), "%s",
             iniparser_dict_getstring(handle, "volc", "token", ""));

    snprintf(g_byte_rtc_data.end_point_id, sizeof(g_byte_rtc_data.end_point_id), "%s",
             iniparser_dict_getstring(handle, "volc", "end_point_id", "ep-20250807104100-77f9z"));

    log_info("rtc config: app_id=%s room=%s uid=%s token_len=%zu",
             g_byte_rtc_data.app_id, g_byte_rtc_data.room_id, g_byte_rtc_data.user_id,
             strlen(g_byte_rtc_data.token));
    log_info("rtc config: end_point_id=%s", g_byte_rtc_data.end_point_id);
    snprintf(s_rtc_token_fallback, sizeof(s_rtc_token_fallback), "%s", g_byte_rtc_data.token);

    g_byte_rtc_data.channel_joined = false;

    strncpy(g_byte_rtc_data.video_file,"resource/send_video.h264",MAX_video_file_LEN);

    g_byte_rtc_data.audio_codec = AUDIO_CODEC_TYPE_G711A;

	//room_id :audio+id(G711AAnykaRoom or G711UAnykaRoom)
    snprintf(g_byte_rtc_data.room_id, sizeof(g_byte_rtc_data.room_id), "G711A%s", room_id);

    if(g_byte_rtc_data.audio_codec == AUDIO_CODEC_TYPE_G711A)
	{
        strncpy(g_byte_rtc_data.audio_file,"resource/send_audio.pcma",MAX_audio_file_LEN);    
    }
	else if(g_byte_rtc_data.audio_codec == AUDIO_CODEC_TYPE_AACLC)
	{
        strncpy(g_byte_rtc_data.audio_file,"resource/send_audio.aac",MAX_audio_file_LEN);    
    }

    g_byte_rtc_data.fps = DEFAULT_FPS_VALUE;
    g_byte_rtc_data.engine  =   NULL;
    g_byte_rtc_data.fini_notifyed = false;

    snprintf(g_byte_rtc_config.task_id, sizeof(g_byte_rtc_config.task_id), "%s",
             iniparser_dict_getstring(handle, "volc", "task_id", ""));

    snprintf(g_byte_rtc_config.sk, sizeof(g_byte_rtc_config.sk), "%s",
             iniparser_dict_getstring(handle, "volc", "sk", ""));

    snprintf(g_byte_rtc_config.ak, sizeof(g_byte_rtc_config.ak), "%s",
             iniparser_dict_getstring(handle, "volc", "ak", ""));

    snprintf(g_byte_rtc_config.rtc_app_id, sizeof(g_byte_rtc_config.rtc_app_id), "%s",
             iniparser_dict_getstring(handle, "volc", "rtc_app_id", ""));

    snprintf(g_byte_rtc_config.rtc_app_key, sizeof(g_byte_rtc_config.rtc_app_key), "%s",
             iniparser_dict_getstring(handle, "volc", "rtc_app_key", ""));

    snprintf(g_byte_rtc_config.default_end_point_id, sizeof(g_byte_rtc_config.default_end_point_id), "%s",
             iniparser_dict_getstring(handle, "volc", "default_end_point_id", ""));

    snprintf(g_byte_rtc_config.default_voice_type, sizeof(g_byte_rtc_config.default_voice_type), "%s",
             iniparser_dict_getstring(handle, "volc", "default_voice_type", ""));

    snprintf(g_byte_rtc_config.asr_app_id, sizeof(g_byte_rtc_config.asr_app_id), "%s",
             iniparser_dict_getstring(handle, "volc", "asr_app_id", ""));

    snprintf(g_byte_rtc_config.asr_access_token, sizeof(g_byte_rtc_config.asr_access_token), "%s",
             iniparser_dict_getstring(handle, "volc", "asr_access_token", ""));

    snprintf(g_byte_rtc_config.tts_app_id, sizeof(g_byte_rtc_config.tts_app_id), "%s",
             iniparser_dict_getstring(handle, "volc", "tts_app_id", ""));

    snprintf(g_byte_rtc_config.tts_access_token, sizeof(g_byte_rtc_config.tts_access_token), "%s",
             iniparser_dict_getstring(handle, "volc", "tts_access_token", ""));

    snprintf(g_byte_rtc_config.account_id, sizeof(g_byte_rtc_config.account_id), "%s",
             iniparser_dict_getstring(handle, "volc", "account_id", ""));

    snprintf(g_byte_rtc_config.duplex_mode, sizeof(g_byte_rtc_config.duplex_mode), "%s",
             iniparser_dict_getstring(handle, "volc", "duplex_mode", "full"));
    g_byte_rtc_config.enable_vad_interrupt =
        iniparser_dict_getint(handle, "volc", "enable_vad_interrupt", 1);
    g_byte_rtc_config.asr_silence_time_ms =
        iniparser_dict_getint(handle, "volc", "asr_silence_time_ms", 800);
    g_byte_rtc_config.vision_enable =
        iniparser_dict_getint(handle, "volc", "vision_enable", 1);

	iniparser_freedict(handle);

    log_info("rtc openapi: task_id=%s ak_len=%zu", g_byte_rtc_config.task_id,
             strlen(g_byte_rtc_config.ak));
    log_info("rtc openapi: rtc_app_id=%s rtc_app_key_len=%zu",
             g_byte_rtc_config.rtc_app_id, strlen(g_byte_rtc_config.rtc_app_key));
    log_info("rtc config: duplex=%s vad_interrupt=%d silence_ms=%d vision=%d",
             g_byte_rtc_config.duplex_mode, g_byte_rtc_config.enable_vad_interrupt,
             g_byte_rtc_config.asr_silence_time_ms, g_byte_rtc_config.vision_enable);
	return 0;
}

/*
 * Audio (PCM/G.711 packed): theoretical bytes per packet = samples_in_window * (bits/8) * channels.
 * Typical capture is 20ms (SAMPLE_AUDIO_FRAME_DURATION): e.g. 8000Hz mono 8bit -> 8000*20/1000 = 160 bytes.
 */
static size_t volc_audio_theory_pcm_frame_bytes(const media_frame_info_t *f, int duration_ms)
{
    int sr = (f && f->sample_rate > 0) ? f->sample_rate : 8000;
    int bits = (f && f->sample_bits > 0) ? f->sample_bits : 8;
    int ch = (f && f->channel_num > 0) ? f->channel_num : 1;
    if (duration_ms <= 0) {
        duration_ms = SAMPLE_AUDIO_FRAME_DURATION;
    }
    unsigned long long samples = (unsigned long long)sr * (unsigned)duration_ms / 1000ULL;
    if (samples == 0) {
        samples = 1;
    }
    size_t bytes = (size_t)(samples * (unsigned)bits / 8U * (unsigned)ch);
    if (bytes == 0) {
        bytes = 160;
    }
    if (bytes > 8192U) {
        bytes = 8192U;
    }
    return bytes;
}

static size_t volc_audio_theory_pcm_frame_bytes_for_rate(int sample_rate, int sample_bits, int channel_num,
                                                         int duration_ms)
{
    if (sample_rate <= 0) {
        sample_rate = 8000;
    }
    if (sample_bits <= 0) {
        sample_bits = 8;
    }
    if (channel_num <= 0) {
        channel_num = 1;
    }
    if (duration_ms <= 0) {
        duration_ms = SAMPLE_AUDIO_FRAME_DURATION;
    }
    unsigned long long samples = (unsigned long long)sample_rate * (unsigned)duration_ms / 1000ULL;
    if (samples == 0) {
        samples = 1;
    }
    size_t bytes = (size_t)(samples * (unsigned)sample_bits / 8U * (unsigned)channel_num);
    if (bytes == 0) {
        bytes = 160;
    }
    if (bytes > 8192U) {
        bytes = 8192U;
    }
    return bytes;
}

/*
 * Video H264: average upper bound per frame at CBR ~= (bitrate_kbps * 1000 / 8) / fps = bitrate_kbps * 125 / fps.
 * If bitrate unknown, estimate from resolution or last good payload size.
 */
static size_t volc_video_theory_payload_bytes(const stream_mgr_t *mgr, const media_frame_info_t *frame)
{
    int fps = (mgr && mgr->video_frame_rate > 0) ? mgr->video_frame_rate : DEFAULT_FPS_VALUE;
    if (fps <= 0) {
        fps = 20;
    }
    if (mgr && mgr->video_bitrate_kbps > 0) {
        size_t v = (size_t)((unsigned long)mgr->video_bitrate_kbps * 125UL / (unsigned)fps);
        if (v < 64) {
            v = 64;
        }
        if (v > 512U * 1024U) {
            v = 512U * 1024U;
        }
        return v;
    }
    int w = (frame && frame->width > 0) ? frame->width : (mgr ? mgr->video_width : 0);
    int h = (frame && frame->height > 0) ? frame->height : (mgr ? mgr->video_height : 0);
    if (w > 0 && h > 0) {
        size_t v = (size_t)((unsigned long)w * (unsigned long)h / 2000UL);
        if (v < 256) {
            v = 256;
        }
        if (v > 512U * 1024U) {
            v = 512U * 1024U;
        }
        return v;
    }
    size_t fb = s_volc_last_nonempty_video_len;
    if (fb < 256) {
        fb = 256;
    }
    if (fb > 512U * 1024U) {
        fb = 512U * 1024U;
    }
    return fb;
}

static int send_video_data_callback(int handle, media_frame_info_t *frame, void *user_data)
{
    (void)handle;
    stream_mgr_t *mgr = (stream_mgr_t *)user_data;
    if (!frame || !mgr) {
        return -1;
    }

    if (g_volc_engine_play == 0 || !s_volc_rtc_callbacks_ok) {
        return 0;
    }

    /* 上行画面开关：流注册保持，仅丢弃发送（不重开 VENC）。 */
    if (!s_volc_video_enable) {
        return 0;
    }

    size_t theory_len = volc_video_theory_payload_bytes(mgr, frame);
    unsigned int real_len = frame->data_len;
    const unsigned char *real_ptr = frame->data;

    if (!g_byte_rtc_data.channel_joined || g_byte_rtc_data.engine == NULL) {
        return 0;
    }

    video_frame_info_t video_frame_info = {
        .data_type      = VIDEO_DATA_TYPE_H264,
        .frame_rate     = 20,
        .frame_type     = VIDEO_FRAME_AUTO_DETECT,
        .stream_type    = VIDEO_STREAM_HIGH
    };
    video_frame_info.frame_rate = mgr->video_frame_rate > 0 ? mgr->video_frame_rate : 20;
    video_frame_info.frame_type = frame->frame_type == 1 ? 1 : 2;

    unsigned char *send_ptr = (unsigned char *)real_ptr;
    unsigned int send_len = real_len;
    void *pad = NULL;

    if (real_len == 0 || real_ptr == NULL) {
        size_t pad_len = theory_len;
        if (pad_len > 512U * 1024U) {
            pad_len = 512U * 1024U;
        }
        pad = calloc(1, pad_len);
        if (!pad) {
            log_error("volc send_video_cb zero-frame pad alloc failed pad_len=%zu (theory=%zu)", pad_len, theory_len);
            return -1;
        }
        send_ptr = (unsigned char *)pad;
        send_len = (unsigned int)pad_len;
        log_warn("volc send_video_cb: real_len=0, sending %u bytes zero padding (theory capped)", send_len);
    } else {
        if (real_len > s_volc_last_nonempty_video_len) {
            s_volc_last_nonempty_video_len = real_len;
        }
    }

    byte_rtc_send_video_data(g_byte_rtc_data.engine, g_byte_rtc_data.room_id,
                             send_ptr, send_len, &video_frame_info);

    free(pad);
    return 0;
}

int start_send_video()
{
    media_video_enc_param_t param = {0};
    int stream = 0;

    /* Avoid double registration: it can leak clients/handles and crash camera/VENC. */
    if (live_stream_mgr.video_handle >= 0) {
        return 0;
    }

    if (live_stream_mgr.multimedia && live_stream_mgr.multimedia->camera) {
        /*
         * Volc uses the preview stream from video_cfg.json: name "video_bg".
         * Resolve its stream index dynamically to avoid hardcoding numeric indices.
         */
        if (live_stream_mgr.multimedia->camera->stream_index_by_name) {
            int idx = -1;
            int rc = live_stream_mgr.multimedia->camera->stream_index_by_name("video_bg", &idx);
            if (rc == 0 && idx >= 0) {
                stream = idx;
            } else {
                log_warn("volc: stream_index_by_name(video_bg) failed rc=%d idx=%d, fallback stream=0",
                         rc, idx);
            }
        } else {
            log_warn("volc: camera ops has no stream_index_by_name, fallback stream=0");
        }

        live_stream_mgr.multimedia->camera->stream_get_enc_param(stream, &param);
        live_stream_mgr.video_frame_rate = param.framerate;
        live_stream_mgr.video_bitrate_kbps = param.bitrate;
        live_stream_mgr.video_width = param.width;
        live_stream_mgr.video_height = param.height;
        log_info("volc video uplink: stream=%d name=video_bg %dx%d fps=%d bitrate=%dkbps codec=H264",
                 stream, param.width, param.height, param.framerate, param.bitrate);
        int ret = live_stream_mgr.multimedia->camera->stream_reg_client(
            stream, &live_stream_mgr.video_handle, send_video_data_callback, (void *)&live_stream_mgr);
        (void)ret;
    }
//    if (g_byte_rtc_data.is_send_video) return -1;
//    g_byte_rtc_data.is_send_video = true;
//    pthread_create(&g_byte_rtc_data.send_video_thread_id, NULL, send_video_routine,0);
    return 0;
}

enum { VOLC_PCMA_PROC_MAX = 2048 };

static int volc_pcm16_peak(const int16_t *samples, size_t count)
{
    int peak = 0;

    if (samples == NULL || count == 0)
        return 0;

    for (size_t i = 0; i < count; i++) {
        int v = (int)samples[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    return peak;
}

static int volc_g711a_pcm16_peak(const uint8_t *g711, size_t nbytes)
{
    int16_t pcm[2048];
    size_t n = nbytes;

    if (g711 == NULL || nbytes == 0)
        return 0;
    if (n > sizeof(pcm))
        n = sizeof(pcm);
    pcma_to_pcm(g711, pcm, n);
    return volc_pcm16_peak(pcm, n);
}

static int volc_g711a_linear_peak(const uint8_t *g711, size_t nbytes)
{
    int peak = 0;

    if (g711 == NULL || nbytes == 0)
        return 0;
    for (size_t i = 0; i < nbytes; i++) {
        int v = (int)g711[i] ^ 0xD5;
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    return peak;
}

/**
 * celestia_old: 16kHz G.711 A-law 下采样为 8kHz——取偶数索引字节 0,2,4,...
 * （μ-law 采集时同样按字节隔一取一，与旧工程仅 PCMA 路径一致）
 */
static int downsample_g711a_16k_to_8k(const uint8_t *input, int input_len, uint8_t *output)
{
    if (input_len < 2 || (input_len % 2) != 0) {
        return -1;
    }
    if (input_len > VOLC_PCMA_PROC_MAX) {
        log_warn("volc: g711 16k frame len=%d > %d, truncated", input_len, VOLC_PCMA_PROC_MAX);
        input_len = VOLC_PCMA_PROC_MAX;
    }
    int output_len = input_len / 2;
    for (int i = 0; i < output_len; i++) {
        output[i] = input[i * 2];
    }
    return output_len;
}

/* 火山 RTC 官方要求默认 20 ms 发包。8 kHz G.711A 下 20 ms = 160 B，
 * 而硬件采集帧是 256 B（32 ms）。这里按 160 B 边界重切：字节顺序不变，
 * 只把打包节奏对齐到官方值，避免服务端按 20 ms 重打包时时间轴漂移。 */
#define VOLC_UPLINK_FRAME_BYTES 160
static uint8_t s_up20_q[VOLC_PCMA_PROC_MAX * 2];
static size_t s_up20_q_len;
static unsigned int s_up20_out_cnt;

static void volc_uplink_send_20ms(const uint8_t *data, size_t len, audio_frame_info_t *info)
{
    if (data && len > 0) {
        if (len > sizeof(s_up20_q)) {
            log_warn("volc uplink 20ms: frame len=%u > queue cap, truncated", (unsigned)len);
            len = sizeof(s_up20_q);
        }
        if (s_up20_q_len + len > sizeof(s_up20_q)) {
            log_warn("volc uplink 20ms: queue overflow (have=%u add=%u), reset",
                     (unsigned)s_up20_q_len, (unsigned)len);
            s_up20_q_len = 0;
        }
        memcpy(s_up20_q + s_up20_q_len, data, len);
        s_up20_q_len += len;
    }

    while (s_up20_q_len >= VOLC_UPLINK_FRAME_BYTES) {
        byte_rtc_send_audio_data(g_byte_rtc_data.engine, g_byte_rtc_data.room_id,
                                 s_up20_q, VOLC_UPLINK_FRAME_BYTES, info);
        memmove(s_up20_q, s_up20_q + VOLC_UPLINK_FRAME_BYTES,
                s_up20_q_len - VOLC_UPLINK_FRAME_BYTES);
        s_up20_q_len -= VOLC_UPLINK_FRAME_BYTES;
        s_up20_out_cnt++;
    }
}

static int send_audio_data_callback(int handle, media_frame_info_t *frame, void *user_data)
{
    static unsigned int s_uplink_cb_cnt;
    static unsigned int s_uplink_drop_play_cnt;
    static unsigned int s_uplink_drop_join_cnt;
    static unsigned int s_uplink_drop_bot_cnt;
    static unsigned int s_uplink_send_ok_cnt;
    static unsigned int s_uplink_empty_cnt;
    static unsigned int s_uplink_voice_frames;
    static int s_uplink_max_g711_peak;
    static int s_uplink_max_pcm_peak;

    (void)handle;
    stream_mgr_t *mgr = (stream_mgr_t *)user_data;
    if (!frame || !mgr) {
        return -1;
    }

    if (g_volc_engine_play == 0 || !s_volc_rtc_callbacks_ok) {
        if (s_uplink_drop_play_cnt < 2u) {
            log_warn("volc uplink drop: play=%d rtc_ok=%d (cb #%u)",
                     (int)g_volc_engine_play, (int)s_volc_rtc_callbacks_ok, s_uplink_cb_cnt);
            s_uplink_drop_play_cnt++;
        }
        return 0;
    }

    audio_frame_info_t audio_frame_info;
    audio_frame_info.data_type = mgr->audio_codec;

    if (!g_byte_rtc_data.channel_joined || g_byte_rtc_data.engine == NULL) {
        if (s_uplink_drop_join_cnt < 2u) {
            log_warn("volc uplink drop: joined=%d engine=%p (cb #%u)",
                     (int)g_byte_rtc_data.channel_joined,
                     (void *)g_byte_rtc_data.engine, s_uplink_cb_cnt);
            s_uplink_drop_join_cnt++;
        }
        return 0;
    }

    /* 按键说话（PTT）未按下：上行发 G.711 静音帧（0xD5），
     * 保持 RTP 节拍与服务端 VAD 静默，不反复开关采集。 */
    if (!s_volc_voice_enable) {
        static uint8_t s_silence_buf[VOLC_PCMA_PROC_MAX];
        size_t send_n = frame->data_len;

        if (send_n > sizeof(s_silence_buf))
            send_n = sizeof(s_silence_buf);
        memset(s_silence_buf, 0xD5, send_n);
        volc_uplink_send_20ms(s_silence_buf, send_n, &audio_frame_info);
        return 0;
    }

    {
        uint64_t now_ms = (uint64_t)(system_get_sys_run_time64());
        if (volc_uplink_bot_gate_blocks()) {
            if (s_uplink_drop_bot_cnt < 8u || (s_uplink_drop_bot_cnt % 100u) == 0u) {
                log_info("volc uplink drop: bot speech gate (half-duplex) cnt=%u", s_uplink_drop_bot_cnt);
            }
            s_uplink_drop_bot_cnt++;
            return 0;
        }
    }

    if (frame->data == NULL || frame->data_len == 0) {
        if (s_uplink_empty_cnt < 5u) {
            log_warn("volc uplink drop: empty mic frame (cb #%u)", s_uplink_cb_cnt);
            s_uplink_empty_cnt++;
        }
        return 0;
    }

    if (debug_print == 1) {
        printf("send audio len:%d \n", frame->data_len);
        if (frame->data && frame->data_len > 0) {
            FILE *fd = fopen("/tmp/audio_send.pcma", "a+");
            if (fd) {
                fwrite(frame->data, 1, frame->data_len, fd);
                fclose(fd);
            }
        }
    }

    /* celestia_old 逻辑：8k 直发；否则 downsample_g711a_16k_to_8k 再发 */
    if (frame->sample_rate == 8000) {
        static int16_t s_near_pcm[480];
        static uint8_t s_near_pcma[480];
        const uint8_t *send_ptr = (const uint8_t *)frame->data;
        size_t send_len = frame->data_len;
        int aec_applied = 0;

        if (frame->data_len > 0 && frame->data_len <= 480) {
            int i;
            int near_peak = 0;
            const char *aec_reason = "idle";
            static unsigned s_aec_diag_cnt;

            pcma_to_pcm((const uint8_t *)frame->data, s_near_pcm, frame->data_len);
            for (i = 0; i < (int)frame->data_len; i++) {
                int v = s_near_pcm[i];
                if (v < 0)
                    v = -v;
                if (v > near_peak)
                    near_peak = v;
            }
            /* Exp6 / 05_orig_volc: pass mic frames through (no near_sil gate).
             * Optional echo mute only when VOLC_APP_ECHO_MUTE_ENA=1. */
            if (volc_echo_should_mute_uplink(near_peak, &aec_reason)) {
                volc_fill_pcma_silence(s_near_pcma, frame->data_len);
                send_ptr = s_near_pcma;
                send_len = frame->data_len;
                aec_applied = 1;
            }
            /* Diagnose correlation even when we mute by energy — prove root cause. */
            if ((near_peak >= 2000 || aec_applied) &&
                (s_aec_diag_cnt < 20u || (s_aec_diag_cnt % 8u) == 0u)) {
                int delay = -1;
                int corr_pct = 0;
                int far_at_d = 0;
                int clip_pct = 0;
                int sil_pct = volc_far_silence_pct();

                volc_echo_corr_diag(s_near_pcm, (int)frame->data_len, &delay, &corr_pct,
                                    &far_at_d, &clip_pct);
                log_info("volc aec: %s near=%d clip%%=%d far_spk=%d far_at_d=%d delay=%d(~%dms) "
                         "corr%%=%d ring=%d(%dms) sil%%=%d mute=%d",
                         aec_reason ? aec_reason : "?", near_peak, clip_pct,
                         (int)s_volc_far_speech_peak, far_at_d, delay,
                         delay > 0 ? (delay * 1000 / 8000) : -1, corr_pct,
                         s_volc_far_filled, s_volc_far_filled * 1000 / 8000, sil_pct,
                         aec_applied);
            }
            if (near_peak >= 2000 || aec_applied)
                s_aec_diag_cnt++;
        }
        {
            const uint8_t *p = send_ptr;
            size_t n = send_len < 160 ? send_len : 160;
            int g711_peak = volc_g711a_linear_peak(p, n);
            int pcm_peak = volc_g711a_pcm16_peak(p, n);

            if (g711_peak > s_uplink_max_g711_peak)
                s_uplink_max_g711_peak = g711_peak;
            if (pcm_peak > s_uplink_max_pcm_peak)
                s_uplink_max_pcm_peak = pcm_peak;
            if (pcm_peak > VOLC_VOICE_PCM_PEAK_THRESH)
                s_uplink_voice_frames++;
            if (s_uplink_send_ok_cnt < 8u || (s_uplink_send_ok_cnt % 50u) == 0u) {
                unsigned int denom = s_uplink_send_ok_cnt > 0u ? s_uplink_send_ok_cnt : 1u;
                unsigned int voice_pct = (s_uplink_voice_frames * 100u) / denom;
                log_info("volc uplink send #%u: len=%u sr=%u codec=%d g711_peak=%d pcm_peak=%d max_g711=%d max_pcm=%d voice_pct=%u%% out20=%u q20=%u far_spk=%d aec=%d",
                         s_uplink_send_ok_cnt, frame->data_len, frame->sample_rate,
                         (int)mgr->audio_codec, g711_peak, pcm_peak,
                         s_uplink_max_g711_peak, s_uplink_max_pcm_peak, voice_pct,
                         s_up20_out_cnt, (unsigned)s_up20_q_len,
                         volc_far_speech_recent() ? (int)s_volc_far_speech_peak : 0,
                         aec_applied);
            }
        }
        volc_uplink_send_20ms(send_ptr, send_len, &audio_frame_info);
        (void)aec_applied;
        s_uplink_send_ok_cnt++;
        s_uplink_cb_cnt++;
        return 0;
    }

    uint8_t pcma8k[2048];
    int pcma8k_len = downsample_g711a_16k_to_8k((const uint8_t *)frame->data, (int)frame->data_len, pcma8k);
    if (pcma8k_len < 0) {
        log_warn("volc send_audio: downsample_g711a_16k_to_8k failed len=%u", frame->data_len);
        return 0;
    }

    {
        int g711_peak = volc_g711a_linear_peak(pcma8k, (size_t)pcma8k_len);
        int pcm_peak = volc_g711a_pcm16_peak(pcma8k, (size_t)pcma8k_len);

        if (g711_peak > s_uplink_max_g711_peak)
            s_uplink_max_g711_peak = g711_peak;
        if (pcm_peak > s_uplink_max_pcm_peak)
            s_uplink_max_pcm_peak = pcm_peak;
        if (pcm_peak > VOLC_VOICE_PCM_PEAK_THRESH)
            s_uplink_voice_frames++;
        if (s_uplink_send_ok_cnt < 8u || (s_uplink_send_ok_cnt % 50u) == 0u) {
            unsigned int denom = s_uplink_send_ok_cnt > 0u ? s_uplink_send_ok_cnt : 1u;
            unsigned int voice_pct = (s_uplink_voice_frames * 100u) / denom;
            log_info("volc uplink send #%u: len=%d sr=%u (downsampled from %u) codec=%d g711_peak=%d pcm_peak=%d max_g711=%d max_pcm=%d voice_pct=%u%% out20=%u",
                     s_uplink_send_ok_cnt, pcma8k_len, frame->sample_rate, frame->data_len,
                     (int)mgr->audio_codec, g711_peak, pcm_peak,
                     s_uplink_max_g711_peak, s_uplink_max_pcm_peak, voice_pct,
                     s_up20_out_cnt);
        }
    }
    volc_uplink_send_20ms(pcma8k, (size_t)pcma8k_len, &audio_frame_info);
    s_uplink_send_ok_cnt++;
    s_uplink_cb_cnt++;

    if (debug_print == 1) {
        FILE *fd = fopen("/tmp/audio_send8.pcma", "a+");
        if (fd) {
            fwrite(pcma8k, 1, (size_t)pcma8k_len, fd);
            fclose(fd);
        }
    }
    return 0;
}


/*
 * 与旧版一致：先打开直播采集，再 stream_reg_client。
 * 回调里的 frame 就是麦克风编码，原样 byte_rtc_send_audio_data。不调用 talk_start。
 */
int start_send_audio(void)
{
    if (live_stream_mgr.audio_handle >= 0) {
        log_info("volc start_send_audio: already registered handle=0x%x", live_stream_mgr.audio_handle);
        return 0;
    }

    if (!live_stream_mgr.multimedia || !live_stream_mgr.multimedia->audio ||
        !live_stream_mgr.multimedia->audio->stream_reg_client) {
        log_error("volc start_send_audio: multimedia/audio/stream_reg_client missing");
        return -1;
    }

    int ai_ret = MULTIMEDIA_ERROR_OPERATION_FAILED;

    if (live_stream_mgr.multimedia->audio->stream_enable)
        ai_ret = live_stream_mgr.multimedia->audio->stream_enable(0, 1);
    else
        ai_ret = audio_service_open_input_streams();

    /* 先开麦再开喇叭，AEC 才能在开 AO 时找到 AI。不调用 talk_start。 */
    (void)audio_service_playback_start(0);

    (void)audio_service_discard_stream_backlog(0);

    if (ai_ret != MULTIMEDIA_OK) {
        log_warn("volc start_send_audio: open capture ret=%d (mic may be silent)", ai_ret);
    }

    live_stream_mgr.audio_handle = -1;
    int ret = live_stream_mgr.multimedia->audio->stream_reg_client(
        0, &live_stream_mgr.audio_handle, send_audio_data_callback, (void *)&live_stream_mgr);
    if (ret != MULTIMEDIA_OK || live_stream_mgr.audio_handle < 0) {
        log_error("volc start_send_audio: stream_reg_client failed ret=%d handle=%d", ret,
                  live_stream_mgr.audio_handle);
        live_stream_mgr.audio_handle = -1;
        return -1;
    }
    {
        media_audio_enc_param_t enc = {0};
        if (live_stream_mgr.multimedia->audio->stream_get_enc_param) {
            live_stream_mgr.multimedia->audio->stream_get_enc_param(0, &enc);
        }
        log_info("volc start_send_audio: ok handle=0x%x open_ret=%d sr=%u codec=%d joined=%d rtc_ok=%d",
                 live_stream_mgr.audio_handle, ai_ret, enc.sample_rate,
                 (int)live_stream_mgr.audio_codec,
                 (int)g_byte_rtc_data.channel_joined, (int)s_volc_rtc_callbacks_ok);
    }
    /* AO ready: play any welcome/TTS frames queued while opening. */
    volc_dl_pend_flush();
    return 0;
}

static int volc_start_media(int start_audio)
{
    g_volc_engine_play = 1;
    (void)start_send_video();
    if (start_audio)
        (void)start_send_audio();
    return 0;
}

int stop_send_video()
{
    if(live_stream_mgr.video_handle >= 0 && live_stream_mgr.multimedia && live_stream_mgr.multimedia->camera)
    {
        int h = live_stream_mgr.video_handle;
        live_stream_mgr.multimedia->camera->stream_unreg_client(h);
        live_stream_mgr.video_handle = -1;
    }

    return 0;
}
int stop_send_audio()
{
    if(live_stream_mgr.audio_handle >= 0 && live_stream_mgr.multimedia && live_stream_mgr.multimedia->audio)
    {
        int h = live_stream_mgr.audio_handle;
        live_stream_mgr.multimedia->audio->stream_unreg_client(h);
        live_stream_mgr.audio_handle = -1;
    }

    return 0;
}

static void byte_rtc_on_join_channel_success(byte_rtc_engine_t engine,const char *channel, int elapsed_ms, bool rejoin) {
    (void)engine;
    (void)rejoin;
    g_byte_rtc_data.channel_joined = true;
    log_info("volc join ok: room=%s elapsed=%dms audio_handle=0x%x",
             channel ? channel : "(null)", elapsed_ms, live_stream_mgr.audio_handle);
    /*
     * Allow RTC uplink/downlink immediately. Welcome TTS often arrives within
     * tens of ms; do not wait for a late start_send_audio() to finish.
     * If AO is still opening, on_audio_data queues into dl_pend.
     */
    s_volc_rtc_callbacks_ok = 1;
    if (live_stream_mgr.audio_handle < 0)
        (void)start_send_audio();
    else
        volc_dl_pend_flush();
    log_info("volc join ok: rtc_ok=1 audio_handle=0x%x", live_stream_mgr.audio_handle);
    printf("\njoin channel success %s elapsed %d ms\n", channel, elapsed_ms);
};

static void byte_rtc_on_user_joined(byte_rtc_engine_t engine,const char *channel, const char *user_name,int elapsed_ms){
    /* Print only a short state snapshot once user joined. */
    log_info("byte_rtc_on_user_joined: channel=%s user=%s elapsed_ms=%d engine=%p play=%d room=%s",
             channel ? channel : "(null)",
             user_name ? user_name : "(null)",
             elapsed_ms,
             (void *)engine,
             (int)g_volc_engine_play,
             g_byte_rtc_data.room_id);

    printf("\nremote user joined  %s:%s\n",channel,user_name);
};

static void byte_rtc_on_user_offline(byte_rtc_engine_t engine,const char *channel, const char *user_name , int reason){
    printf("\nremote user offline  %s:%s\n",channel,user_name);
};

static void byte_rtc_on_user_mute_audio(byte_rtc_engine_t engine,const char *channel, const char *user_name ,int muted){
    printf("\nremote user mute audio  %s:%s %d\n",channel,user_name,muted);

};

static void byte_rtc_on_user_mute_video(byte_rtc_engine_t engine,const char *channel, const char *user_name ,int muted){
    printf("\nremote user mute video  %s:%s %d\n",channel,user_name,muted);
};

static void byte_rtc_on_connection_lost(byte_rtc_engine_t engine,const char *channel){
    g_byte_rtc_data.channel_joined = false;
    log_warn("rtc connection_lost: channel=%s engine=%p", channel ? channel : "(null)", (void *)engine);
};

static void byte_rtc_on_audio_data(byte_rtc_engine_t engine, const char* room, const char* uid, uint16_t sent_ts,
    audio_data_type_e codec, const void * data_ptr, size_t data_len)
{
    static unsigned int s_audio_cb_print_cnt = 0;
    static unsigned int s_talk_send_ok_cnt = 0;
    static unsigned int s_talk_send_fail_cnt = 0;
    static int s_session_max_peak = 0;
    if (s_audio_cb_print_cnt < 2) {
        log_info("byte_rtc_on_audio_data[%-u]: engine=%p room=%s uid=%s sent_ts=%u codec=%d play=%d data_ptr=%p data_len=%zu",
                 s_audio_cb_print_cnt,
                 (void *)engine,
                 room ? room : "(null)",
                 uid ? uid : "(null)",
                 (unsigned int)sent_ts,
                 (int)codec,
                 (int)g_volc_engine_play,
                 data_ptr,
                 data_len);
        s_audio_cb_print_cnt++;
    }

    if (!s_volc_rtc_callbacks_ok || g_volc_engine_play == 0 || g_byte_rtc_data.engine == NULL) {
        return;
    }

    if (data_len == 0 || data_ptr == NULL) {
        log_warn("byte_rtc_on_audio_data: empty audio data (data_ptr=%p data_len=%zu), skip",
                 data_ptr, data_len);
        return;
    }

    /* data_len is bytes:
     * - PCMA/PCMU: 1 byte per sample => samples_8k = data_len
     * - PCM: 2 bytes per sample => samples_8k = data_len / 2
     */
    size_t samples_8k = 0;
    if (codec == AUDIO_DATA_TYPE_PCMA || codec == AUDIO_DATA_TYPE_PCMU) {
        samples_8k = data_len;
    } else if (codec == AUDIO_DATA_TYPE_PCM) {
        if (data_len % 2 != 0) {
            log_warn("byte_rtc_on_audio_data: PCM data_len not aligned: %zu", data_len);
            return;
        }
        samples_8k = data_len / 2;
    } else {
        log_warn("byte_rtc_on_audio_data: unsupported codec=%d, skip", (int)codec);
        return;
    }
    if (samples_8k == 0 || samples_8k > 2048) {
        log_warn("byte_rtc_on_audio_data: invalid samples_8k=%zu", samples_8k);
        return;
    }

    static int16_t s_pcm_8k_buf[2048];
    int16_t *output_buf_8k = s_pcm_8k_buf;

    /* Decode to 8k PCM16 */
    if (codec == AUDIO_DATA_TYPE_PCMA) {
        pcma_to_pcm((const uint8_t*)data_ptr, output_buf_8k, samples_8k);
    } else if (codec == AUDIO_DATA_TYPE_PCMU) {
        pcmu_to_pcm((const uint8_t*)data_ptr, output_buf_8k, samples_8k);
    } else { /* AUDIO_DATA_TYPE_PCM */
        memcpy(output_buf_8k, data_ptr, samples_8k * sizeof(int16_t));
    }

    int peak = 0;
    for (size_t i = 0; i < samples_8k; i++) {
        int v = output_buf_8k[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }

    {
        static unsigned s_pcm_diag_log;
        if (peak > s_session_max_peak) {
            s_session_max_peak = peak;
        }
        if (s_pcm_diag_log < 1) {
            if (codec == AUDIO_DATA_TYPE_PCMA || codec == AUDIO_DATA_TYPE_PCMU) {
                const uint8_t *g711 = (const uint8_t *)data_ptr;
                log_info("volc: pcm8k peak=%d max_peak=%d samples=%zu codec=%d g711[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x",
                         peak, s_session_max_peak, samples_8k, (int)codec,
                         g711[0], g711[1], g711[2], g711[3],
                         g711[4], g711[5], g711[6], g711[7]);
            } else {
                log_info("volc: pcm8k peak=%d max_peak=%d samples=%zu codec=%d",
                         peak, s_session_max_peak, samples_8k, (int)codec);
            }
            s_pcm_diag_log++;
        }
    }

    /* Half-duplex only: gate uplink while bot TTS has real speech (peak>=800). */
    if (!volc_agent_full_duplex() && uid != NULL && g_byte_rtc_data.user_id[0] != '\0' &&
        strcmp(uid, g_byte_rtc_data.user_id) != 0) {
        if (peak >= VOLC_BOT_SPEECH_PEAK_THRESH) {
            s_volc_bot_audio_active = 1;
            s_volc_bot_audio_last_ms = (uint64_t)system_get_sys_run_time64();
        } else if (peak < VOLC_BOT_SILENCE_PEAK_THRESH && s_volc_bot_audio_active) {
            /* Comfort-noise frames (peak≈8): do not extend gate; let hold timer expire. */
        }
    }

    /* PTT held: do not play bot TTS to SPK (half-duplex). Keep far ring as
     * silence so SDK/App AEC does not treat residual room echo as far speech. */
    if (s_volc_voice_enable) {
        static int16_t s_zero_far[2048];
        static unsigned s_ptt_dl_mute_log;
        volc_far_push(s_zero_far, samples_8k);
        if (s_ptt_dl_mute_log < 3u || (s_ptt_dl_mute_log % 100u) == 0u) {
            log_info("volc: PTT mute downlink (skip AO) peak_would=%d cnt=%u",
                     peak, s_ptt_dl_mute_log);
        }
        s_ptt_dl_mute_log++;
        return;
    }

    /* 下行 PCM 写入 far 环（实时覆盖）+ 标记是否在真正说话，供上行压制/相关诊断。 */
    volc_far_push(output_buf_8k, samples_8k);

    /* AO talk 路径已是 8k，直送 PCM 避免 16k 上采样再降采样 */
    media_frame_info_t frame = {0};
    frame.data = (unsigned char *)output_buf_8k;
    frame.data_len = (unsigned int)(samples_8k * sizeof(int16_t));
    frame.code_type = MEDIA_CODEC_AUDIO_PCM;
    frame.sample_rate = 8000;
    frame.sample_bits = 16;
    frame.channel_num = 1;

    if (!live_stream_mgr.multimedia || !live_stream_mgr.multimedia->audio ||
        !live_stream_mgr.multimedia->audio->talk_send_data) {
        volc_dl_pend_push(output_buf_8k, samples_8k);
        log_warn("byte_rtc_on_audio_data: talk_send_data not ready, pend %zu samples",
                 samples_8k);
        return;
    }

    int talk_ret = live_stream_mgr.multimedia->audio->talk_send_data(0, &frame);
    if (talk_ret == MULTIMEDIA_OK) {
        s_talk_send_ok_cnt++;
        if (s_talk_send_ok_cnt <= 2 || (s_talk_send_ok_cnt % 50) == 0) {
            log_info("volc: talk_send_data ok #%u bytes=%u sr=%d peak=%d max_peak=%d",
                     s_talk_send_ok_cnt, frame.data_len, frame.sample_rate, peak, s_session_max_peak);
        }
        if (s_talk_send_ok_cnt == 100 && s_session_max_peak < 64) {
            log_warn("volc: remote audio still near-silence after 100 frames (max_peak=%d)",
                     s_session_max_peak);
        }
    } else {
        /* AO not open yet (or transient): queue for flush after playback_start. */
        volc_dl_pend_push(output_buf_8k, samples_8k);
        s_talk_send_fail_cnt++;
        if (s_talk_send_fail_cnt <= 10) {
            log_warn("volc: talk_send_data fail #%u ret=%d bytes=%u -> pend",
                     s_talk_send_fail_cnt, talk_ret, frame.data_len);
        }
    }
    return ;
};

/* Unicode code point (U+XXXX) to UTF-8 */
static char* unicode_to_utf8(unsigned int codepoint) {
    char *utf8 = malloc(4);  /* max 4 bytes */
    if (!utf8) return NULL;

    if (codepoint <= 0x7F) {
        utf8[0] = (char)codepoint;
        utf8[1] = '\0';
    } else if (codepoint <= 0x7FF) {
        utf8[0] = 0xC0 | (codepoint >> 6);
        utf8[1] = 0x80 | (codepoint & 0x3F);
        utf8[2] = '\0';
    } else if (codepoint <= 0xFFFF) {
        utf8[0] = 0xE0 | (codepoint >> 12);
        utf8[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        utf8[2] = 0x80 | (codepoint & 0x3F);
        utf8[3] = '\0';
    } else if (codepoint <= 0x10FFFF) {
        utf8[0] = 0xF0 | (codepoint >> 18);
        utf8[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        utf8[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        utf8[3] = 0x80 | (codepoint & 0x3F);
        utf8[4] = '\0';
    } else {
        free(utf8);
        return NULL;
    }

    return utf8;
}

/* Replace \\uXXXX in string with UTF-8; returns malloc'd string, caller must free */
static char* byte_convert_unicode_to_utf8(const char *input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    char *output = malloc(len + 1);  /* worst case: no \\u, copy as-is */
    if (!output) return NULL;

    char *out_ptr = output;
    const char *in_ptr = input;

    while (*in_ptr) {
        if (in_ptr[0] == '\\' && in_ptr[1] == 'u' && isxdigit(in_ptr[2]) && 
            isxdigit(in_ptr[3]) && isxdigit(in_ptr[4]) && isxdigit(in_ptr[5])) {
            /* Extract 4 hex digits */
            char hex[5] = {in_ptr[2], in_ptr[3], in_ptr[4], in_ptr[5], '\0'};
            unsigned int codepoint = (unsigned int)strtol(hex, NULL, 16);

            /* Convert to UTF-8 */
            char *utf8 = unicode_to_utf8(codepoint);
            if (utf8) {
                strcpy(out_ptr, utf8);
                out_ptr += strlen(utf8);
                free(utf8);
            } else {
                /* Skip on failure */
                *out_ptr++ = '\\';
                *out_ptr++ = 'u';
                *out_ptr++ = in_ptr[2];
                *out_ptr++ = in_ptr[3];
                *out_ptr++ = in_ptr[4];
                *out_ptr++ = in_ptr[5];
            }

            in_ptr += 6;  /* Skip \\uXXXX */
        } else {
            *out_ptr++ = *in_ptr++;
        }
    }

    *out_ptr = '\0';
    return output;
}

static int volc_parse_recv_text_json(const char *json_string,char **text_out,char *user_id,long long *time,int *paragraph)
{
    if (!json_string) return -1;

    // 解析JSON
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        fprintf(stderr, "JSON 解析失败: %s\n", cJSON_GetErrorPtr());
        return -1;
    }

    // 检查是否为对象
    if (root->type != cJSON_Object) {
        fprintf(stderr, "根对象不是JSON 对象\n");
        cJSON_Delete(root);
        return -1;
    }

    // 提取 type
//    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
//    if (type_obj && type_obj->type == cJSON_String) {
//        response->type = strdup(type_obj->valuestring);
//    }

    // 提取 data 数组
    cJSON *data_array = cJSON_GetObjectItem(root, "data");
    if (!data_array || data_array->type != cJSON_Array) {
        fprintf(stderr, "缺少 'data' 数组\n");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(data_array);

    // 遍历数组，解析每个元
    cJSON *item = data_array->child;
    for (int i = 0; i < count; i++) {
        if (!item) break;


        // definite
//        cJSON *definite = cJSON_GetObjectItem(item, "definite");
//        if (definite && definite->type == cJSON_True) {
//            sub->definite = 1;
//        } else if (definite && definite->type == cJSON_False) {
//            sub->definite = 0;
//        }

        // language
//        cJSON *lang = cJSON_GetObjectItem(item, "language");
//        if (lang && lang->type == cJSON_String) {
//            sub->language = strdup(lang->valuestring);
//        }

        // mode
//        cJSON *mode = cJSON_GetObjectItem(item, "mode");
//        if (mode && mode->type == cJSON_Number) {
//            sub->mode = mode->valueint;
//        }

        // paragraph
        cJSON *para = cJSON_GetObjectItem(item, "paragraph");
        if (para && para->type == cJSON_True) {
            *paragraph = 1;
        } else if (para && para->type == cJSON_False) {
            *paragraph = 0;
        }

        // roundId
//        cJSON *round = cJSON_GetObjectItem(item, "roundId");
//        if (round && round->type == cJSON_Number) {
//            sub->roundId = round->valueint;
//        }

        // sequence
//        cJSON *seq = cJSON_GetObjectItem(item, "sequence");
//        if (seq && seq->type == cJSON_Number) {
//            sub->sequence = seq->valueint;
//        }

        // text
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (text && text->type == cJSON_String) {
            //sub->text = strdup(text->valuestring);
			char *result = byte_convert_unicode_to_utf8(text->valuestring);
		    if (result) {
		        //printf("size=%d data_size:%d recv remote msg: %s\n", size,data_len,result);
		        //free(result);
				*text_out = result;
		    }
        }

        // timestamp
        cJSON *ts = cJSON_GetObjectItem(item, "timestamp");
        if (ts && ts->type == cJSON_Number) {
            *time = ts->valuedouble;  // double 可容long long
        }

        // userId
        cJSON *uid = cJSON_GetObjectItem(item, "userId");
        if (uid && uid->type == cJSON_String) {
            //sub->userId = strdup(uid->valuestring);
			strcpy(user_id,uid->valuestring);
        }

        item = item->next;
    }

    // 释放 JSON 
    cJSON_Delete(root);
    return 0;
}

void byte_rtc_on_message_received(byte_rtc_engine_t engine, const char* room, const char* src, const uint8_t* message, int size, bool binary)
{
    (void)engine;
    (void)room;
    (void)src;
    static unsigned int s_msg_log_cnt = 0;
    if (s_msg_log_cnt < 5) {
        log_info("byte_rtc_on_message_received[%u]: size=%d binary=%d", s_msg_log_cnt, size, (int)binary);
        s_msg_log_cnt++;
    }
    if (!s_volc_rtc_callbacks_ok) {
        return;
    }

    //Big-Endian
    const uint8_t *this = message + sizeof(int) * 2;
    int data_len = 0;
    memcpy(&data_len,message+sizeof(int),sizeof(int));
    data_len = ntohl(data_len);

	char user_id[64] = {0};
	int user = 0;
	char *text = NULL;
	long long time;
	int paragraph;

	volc_parse_recv_text_json(this,&text,user_id,&time,&paragraph);

	if(strcmp(user_id,g_byte_rtc_data.user_id) != 0)
	{
		user = 1;
	}
	/* Bot 一句结束 (para=1)：半双工下立即放开上行 gate */
	if (!volc_agent_full_duplex() && user == 1 && paragraph == 1) {
		s_volc_bot_audio_active = 0;
		s_volc_bot_audio_last_ms = 0;
		log_info("volc: bot sentence end, uplink gate released");
	}
	//(int user_id,char *text,int text_len,int text_time,int paragraph)
    //char *result = byte_convert_unicode_to_utf8(this);

    if (text) {
        if (!s_volc_rtc_callbacks_ok) {
            free(text);
            return;
        }

        /* self_asr 流式 para=0 极高频入队会打满 64 深度队列；整句 para=1 必打 log */
        int push_to_ui_queue = (user != 0) || (paragraph != 0);
        int force_log = (user == 0 && paragraph == 1);
        if (user == 1 && paragraph == 0) {
            static clock_t s_last_bot_ui_clock;
            clock_t nowc = clock();
            if (s_last_bot_ui_clock != 0 &&
                (nowc - s_last_bot_ui_clock) * 1000 / CLOCKS_PER_SEC < 120) {
                push_to_ui_queue = 0;
            } else if (push_to_ui_queue) {
                s_last_bot_ui_clock = nowc;
            }
        }

        if (push_to_ui_queue || force_log) {
            size_t tlen = strlen(text);
            const int cap = 512;
            if (tlen > (size_t)cap) {
                log_info("volc recv_text: role=%s uid=%s para=%d len=%zu text=%.*s...",
                         user == 0 ? "self_asr" : "peer_bot",
                         user_id,
                         paragraph,
                         tlen,
                         cap,
                         text);
            } else {
                log_info("volc recv_text: role=%s uid=%s para=%d len=%zu text=%s",
                         user == 0 ? "self_asr" : "peer_bot",
                         user_id,
                         paragraph,
                         tlen,
                         text);
            }
        }

        if (push_to_ui_queue) {
            notification_t *notif = (notification_t *)app_main_get_notification();
            if (notif && notif->queue) {
                int handle = notif->queue->create("volc2ui_user");
                int handle1 = notif->queue->create("volc2ui_bot");
                if (user == 0)
                    (void)notif->queue->write(handle, text, (int)strlen(text));
                else
                    (void)notif->queue->write(handle1, text, (int)strlen(text));
            }
        }

        if (volc_recv_text_cb) {
            volc_recv_text_cb(user, text, (int)strlen(text), time, paragraph);
        }

        free(text);
    }

    return ;
}

static void byte_rtc_on_video_data(byte_rtc_engine_t engine,const char *channel, const char *user_name ,uint16_t sent_ts,
                      video_data_type_e codec,  int is_key_frame,
                      const void *data_ptr, size_t data_len) 
{
    if (!s_volc_rtc_callbacks_ok || g_volc_engine_play == 0) {
        return;
    }

    static unsigned int s_video_cb_print_cnt = 0;
    if (s_video_cb_print_cnt < 3) {
        log_info("byte_rtc_on_video_data[%-u]: engine=%p channel=%s user=%s sent_ts=%u codec=%d is_key=%d data_ptr=%p data_len=%zu play=%d room=%s",
                 s_video_cb_print_cnt,
                 (void *)engine,
                 channel ? channel : "(null)",
                 user_name ? user_name : "(null)",
                 (unsigned int)sent_ts,
                 (int)codec,
                 is_key_frame,
                 data_ptr,
                 data_len,
                 (int)g_volc_engine_play,
                 g_byte_rtc_data.room_id);
        s_video_cb_print_cnt++;
    }

    /* Keep old behavior but avoid spam: only print for key frame the first few times. */
    if (is_key_frame && s_video_cb_print_cnt <= 3) {
        printf("\nreceived key frame of user: %s\n", user_name);
    }
};

static void byte_rtc_on_channel_error(byte_rtc_engine_t engine,const char *channel, int code, const char *msg){
    log_error("rtc room_error: channel=%s code=%d msg=%s",
              channel ? channel : "(null)", code, msg ? msg : "(null)");
};

static void byte_rtc_on_gloable_error(byte_rtc_engine_t engine,int code, const char *msg){
    log_error("rtc global_error: code=%d msg=%s", code, msg ? msg : "(null)");
};

static void byte_rtc_on_keyframe_gen_req(byte_rtc_engine_t engine,const char *channel, const char *user_name){
    printf("\nremote req key frame %s:%s\n",channel,user_name);
};

static void byte_rtc_on_target_bitrate_changed(byte_rtc_engine_t engine,const char *channel, uint32_t target_bps){
};

#if CONFIG_NETWORK_SUPPORTED
static int volc_ipv4_usable(const char *ip)
{
    return ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

static int volc_network_ipv4_ready(void)
{
    network_t *net = (network_t *)app_main_get_network();
    char ip[64];

    if (!net) {
        return 0;
    }

    if (net->wifi && net->wifi->station) {
        network_wifi_station_state_e state;
        if (net->wifi->station->get_state(&state) == NETWORK_OK &&
            state == NETWORK_WIFI_STATION_STATE_CONNECTED &&
            net->wifi->station->get_ip_address(ip, sizeof(ip)) == NETWORK_OK &&
            volc_ipv4_usable(ip)) {
            return 1;
        }
    }

    if (net->ethernet) {
        ip[0] = '\0';
        if (net->ethernet->get_ip &&
            net->ethernet->get_ip(ip) == NETWORK_OK &&
            volc_ipv4_usable(ip)) {
            return 1;
        }
        if (net->ethernet->get_ip_by_name) {
            static const char *ifaces[] = {"wlan0", "eth0", "eth1", "net0", "enp0s0"};
            for (unsigned i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
                ip[0] = '\0';
                if (net->ethernet->get_ip_by_name(ifaces[i], ip, sizeof(ip)) == NETWORK_OK &&
                    volc_ipv4_usable(ip)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}
#else
static int volc_network_ipv4_ready(void)
{
    return 1;
}
#endif

static int volc_rtc_token_refresh(unsigned int ttl_sec)
{
    char new_token[MAX_TOKEN_LEN];

#if CONFIG_NETWORK_SUPPORTED
    if (!volc_network_ipv4_ready()) {
        log_error("rtc token: network not ready, skip generate");
        return VOLC_ENGINE_ERR_NETWORK;
    }
#endif

    if (volc_rtc_token_generate(new_token, sizeof(new_token),
                                g_byte_rtc_data.app_id,
                                g_byte_rtc_config.rtc_app_key,
                                g_byte_rtc_data.room_id,
                                g_byte_rtc_data.user_id,
                                ttl_sec) == 0) {
        snprintf(g_byte_rtc_data.token, sizeof(g_byte_rtc_data.token), "%s", new_token);
        log_info("rtc token generated locally (len=%zu ttl=%u)",
                 strlen(g_byte_rtc_data.token), ttl_sec);
        return 0;
    }
    if (s_rtc_token_fallback[0] != '\0') {
        snprintf(g_byte_rtc_data.token, sizeof(g_byte_rtc_data.token), "%s", s_rtc_token_fallback);
        log_warn("rtc token local generate failed, using ini fallback (len=%zu)",
                 strlen(g_byte_rtc_data.token));
        return 0;
    }
    log_error("rtc token local generate failed and no ini fallback");
    return -1;
}

static void byte_rtc_on_token_privilege_will_expire(byte_rtc_engine_t engine, const char *room)
{
    (void)room;
    if (volc_rtc_token_refresh(VOLC_RTC_TOKEN_DEFAULT_TTL_SEC) != 0) {
        log_error("rtc token renew generate failed");
        return;
    }
    int ret = byte_rtc_renew_token(engine, g_byte_rtc_data.room_id, g_byte_rtc_data.token);
    if (ret != 0) {
        log_error("rtc token renew failed ret=%d", ret);
    } else {
        log_info("rtc token renewed (len=%zu)", strlen(g_byte_rtc_data.token));
    }
}
static void byte_rtc_on_fini_notify(byte_rtc_engine_t engine) {
    g_byte_rtc_data.fini_notifyed = true;
    g_byte_rtc_data.channel_joined = false;
    log_warn("rtc fini_notify: engine=%p", (void *)engine);
}


int volc_recv_info_printf(const command_pkt *pParams, char *pcAck)
{
    debug_print = atoi(pParams->p[0]);
    printf("debug_print = %d \n",debug_print);

    return 0;
}

static void volc_http_response_reset(void)
{
    if (g_byte_rtc_data.data) {
        free(g_byte_rtc_data.data);
        g_byte_rtc_data.data = NULL;
    }
    g_byte_rtc_data.size = 0;
}

static const char *volc_task_id_for_api(void)
{
    return (g_byte_rtc_config.task_id[0] != '\0') ? g_byte_rtc_config.task_id : "task1";
}

static int volc_build_stop_chat_body(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return -1;
    }
    int n = snprintf(buf, buflen,
                     "{\"AppId\":\"%s\",\"RoomId\":\"%s\",\"TaskId\":\"%s\"}",
                     g_byte_rtc_data.app_id,
                     g_byte_rtc_data.room_id,
                     volc_task_id_for_api());
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}

/* 0=ok, 1=already started, -1=error */
static int volc_parse_start_voice_chat_result(const char *json)
{
    if (!json || json[0] == '\0') {
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return -1;
    }

    int ret = -1;
    cJSON *result = cJSON_GetObjectItem(root, "Result");
    const char *rs = (result && cJSON_IsString(result)) ? result->valuestring : NULL;
    if (rs) {
        if (strcmp(rs, "ok") == 0) {
            ret = 0;
        } else if (strstr(rs, "already") != NULL || strstr(rs, "repeatedly") != NULL) {
            ret = 1;
        }
    }

    cJSON_Delete(root);
    return ret;
}

static int volc_engine_invoke_voice_chat_api(const char *query, const char *body,
                                             HTTPS_WRITE_CALLBACK write_cb, void *param)
{
    const char *host = "rtc.volcengineapi.com";
    const char *method = "POST";
    const char *uri = "/";
    const char *AK = g_byte_rtc_config.ak;
    const char *SK = g_byte_rtc_config.sk;

    return request_rtc_api_start(host, method, uri, query, body, AK, SK, write_cb, param);
}

int volc_engine_start_chat(const char *body, HTTPS_WRITE_CALLBACK write_cb, void *param)
{
    return volc_engine_invoke_voice_chat_api("Action=StartVoiceChat&Version=2024-12-01",
                                             body, write_cb, param);
}

int volc_engine_stop_chat(const char *body, HTTPS_WRITE_CALLBACK write_cb, void *param)
{
    return volc_engine_invoke_voice_chat_api("Action=StopVoiceChat&Version=2024-12-01",
                                             body, write_cb, param);
}

static int volc_engine_start_chat_checked(const char *body)
{
    int parse_ret;

    volc_http_response_reset();
    if (volc_engine_start_chat(body, byte_rtc_perform_req_callback, &g_byte_rtc_data) != 0) {
        log_error("volc_engine_start_chat_checked: HTTP request failed");
        return -1;
    }

    parse_ret = volc_parse_start_voice_chat_result(g_byte_rtc_data.data);
    if (parse_ret == 0) {
        return 0;
    }

    if (parse_ret == 1) {
        char stop_body[512];

        if (volc_build_stop_chat_body(stop_body, sizeof(stop_body)) < 0) {
            log_error("volc_engine_start_chat_checked: build StopVoiceChat body failed");
            return -1;
        }

        log_warn("StartVoiceChat: task already started (task=%s), calling StopVoiceChat then retry",
                 volc_task_id_for_api());
        volc_http_response_reset();
        (void)volc_engine_stop_chat(stop_body, byte_rtc_perform_req_callback, &g_byte_rtc_data);

        volc_http_response_reset();
        if (volc_engine_start_chat(body, byte_rtc_perform_req_callback, &g_byte_rtc_data) != 0) {
            log_error("volc_engine_start_chat_checked: retry StartVoiceChat HTTP failed");
            return -1;
        }
        parse_ret = volc_parse_start_voice_chat_result(g_byte_rtc_data.data);
        if (parse_ret == 0) {
            log_info("StartVoiceChat: retry ok after StopVoiceChat");
            return 0;
        }
    }

    log_error("StartVoiceChat failed: %s",
              (g_byte_rtc_data.data && g_byte_rtc_data.data[0]) ? g_byte_rtc_data.data : "(empty)");
    return -1;
}

static void volc_engine_stop_cloud_task(void)
{
    char stop_body[512];

    if (g_byte_rtc_data.app_id[0] == '\0' || g_byte_rtc_data.room_id[0] == '\0') {
        return;
    }
    if (volc_build_stop_chat_body(stop_body, sizeof(stop_body)) < 0) {
        return;
    }

    volc_http_response_reset();
    log_info("volc_engine_stop_cloud_task: StopVoiceChat room=%s task=%s",
             g_byte_rtc_data.room_id, volc_task_id_for_api());
    (void)volc_engine_stop_chat(stop_body, byte_rtc_perform_req_callback, &g_byte_rtc_data);
    volc_http_response_reset();
}
//int volc_recv_text(int user_id,char *text,int text_len,long long text_time,int paragraph)
//{
//	printf("user_id=%d,text_len=%d,text_time=%lld,paragraph=%d\n",user_id,text_len,text_time,paragraph);
//	printf("text=%s\n",text);

//	return 0;
//}

/* 彻底释放 RTC engine，避免 deinit 后 channel_joined 仍为 true 导致二次 init 假在房 */
static void volc_rtc_engine_release(void)
{
    byte_rtc_engine_t engine = g_byte_rtc_data.engine;

    if (engine == NULL)
        return;

    log_info("volc_rtc_release: engine=%p joined=%d room=%s",
             (void *)engine, g_byte_rtc_data.channel_joined ? 1 : 0,
             g_byte_rtc_data.room_id);

    if (g_byte_rtc_data.channel_joined && g_byte_rtc_data.room_id[0] != '\0') {
        (void)byte_rtc_leave_room(engine, g_byte_rtc_data.room_id);
        g_byte_rtc_data.channel_joined = false;
    }

    g_byte_rtc_data.fini_notifyed = false;
    byte_rtc_fini(engine);

    for (int i = 0; i < 60 && !g_byte_rtc_data.fini_notifyed; i++)
        usleep(50000);

    if (!g_byte_rtc_data.fini_notifyed)
        log_warn("volc_rtc_release: fini_notify timeout, destroy anyway");

    byte_rtc_destroy(engine);
    g_byte_rtc_data.engine = NULL;
    g_byte_rtc_data.fini_notifyed = false;
    s_volc_bot_audio_active = 0;
    s_volc_bot_audio_last_ms = 0;
}

int volc_engine_init() 
{
//	volc_engine_set_text_cb(volc_recv_text);
    log_info("volc_engine_init: pid=%d tid=%lu inited=%d joined=%d engine=%p",
             (int)getpid(),
             (unsigned long)pthread_self(),
             g_volc_engine_inited,
             (int)g_byte_rtc_data.channel_joined,
             (void *)g_byte_rtc_data.engine);

    if (g_volc_engine_inited) {
        return 0;
    }

    s_volc_rtc_callbacks_ok = 0;
    volc_dl_pend_clear();

    /* 每次进入会话：上行默认静音（按键说话）。画面开关在读完 ini 后再定。 */
    s_volc_voice_enable = 0;
    s_volc_video_enable = 0;

    if (g_byte_rtc_data.engine != NULL) {
        log_warn("volc_engine_init: stale engine=%p joined=%d, release before full init",
                 (void *)g_byte_rtc_data.engine, (int)g_byte_rtc_data.channel_joined);
        stop_send_video();
        stop_send_audio();
        volc_rtc_engine_release();
    }

    live_stream_mgr.video_handle = -1;
    live_stream_mgr.audio_handle = -1;

    if(access(volc_work_path, F_OK) != 0)
    {
        char cmd[64] = {0};
    	sprintf(cmd,"mkdir -p %s",volc_work_path);
    	system(cmd);
    }
    //memset(&g_byte_rtc_data,0,sizeof(byte_rtc_data));

    console_cmd_register("ak-volc-print", volc_recv_info_printf, "watdog-help");
//    console_cmd_register("ak-volc-json", byte_rtc_config_to_json, "watdog-help");
    if (byte_rtc_init_default_data() != 0) {
        printf("volc_engine_init: app_volc_config.ini missing or invalid, skip\n");
        return VOLC_ENGINE_ERR;
    }
    /* vision_enable=1 → keep H.264 uplink even if UI stays in text mode. */
    s_volc_video_enable = g_byte_rtc_config.vision_enable ? 1 : 0;
    log_info("volc video_enable init=%d (vision=%d)",
             s_volc_video_enable, g_byte_rtc_config.vision_enable);

#if CONFIG_NETWORK_SUPPORTED
    if (!volc_network_ipv4_ready()) {
        log_error("volc_engine_init: network not ready");
        return VOLC_ENGINE_ERR_NETWORK;
    }
#endif

    /* 上次退出若 multimedia_close 过，需重新 open，否则 app_main 指针指向已 teardown 的单例 */
    {
        multimedia_t *mm = NULL;
        if (multimedia_open(&mm) != 0 || mm == NULL) {
            log_error("volc_engine_init: multimedia_open failed");
            return -1;
        }
        app_main_register_multimedia(mm);
    }

    /* Resolve multimedia + codec before HTTP/RTC; actual capture clients register in volc_start_media()
     * after StartVoiceChat + join_room, same order as old fw volc_start() after join. */
    live_stream_mgr.multimedia = (multimedia_t *)app_main_get_multimedia();

    media_audio_enc_param_t audio_enc_param = {0};
    if (live_stream_mgr.multimedia && live_stream_mgr.multimedia->audio) {
        live_stream_mgr.multimedia->audio->stream_get_enc_param(0, &audio_enc_param);
    }
    switch (audio_enc_param.codec) {
        case MEDIA_CODEC_AUDIO_AAC:
            live_stream_mgr.audio_codec = AUDIO_DATA_TYPE_AACLC;
            break;
        case MEDIA_CODEC_AUDIO_G711A:
            live_stream_mgr.audio_codec = AUDIO_DATA_TYPE_PCMA;
            break;
        case MEDIA_CODEC_AUDIO_G711U:
            live_stream_mgr.audio_codec = AUDIO_DATA_TYPE_PCMU;
            break;
        default:
            break;
    }

    /* 按新需求：先把本地音视频流初始化好（至少保证播放链路可用），再连接火山服务器。 */
    /* Video preview before join; mic uplink starts in byte_rtc_on_join_channel_success. */
    (void)volc_start_media(0);

    char content[6*1024] = {0};
	#if 1
	byte_rtc_config_to_json(&g_byte_rtc_config,&g_byte_rtc_data,content);
	#else
//	printf("content0:%s\n",content);
//	memset(content,0,sizeof(content));
    FILE *fd = fopen("/etc/config/volc_req.json","r");
    int send_len = fread(content,1,sizeof(content),fd);
    fclose(fd);
//	printf("content0:%s\n",content);
//	return 0;
#endif
    byte_rtc_event_handler_t eventHandle = {0};
    
    eventHandle.on_global_error            =   byte_rtc_on_gloable_error;
    eventHandle.on_join_room_success       =   byte_rtc_on_join_channel_success;
    eventHandle.on_room_error              =   byte_rtc_on_channel_error;
    eventHandle.on_user_joined             =   byte_rtc_on_user_joined;
    eventHandle.on_user_offline            =   byte_rtc_on_user_offline;
    eventHandle.on_user_mute_audio         =   byte_rtc_on_user_mute_audio;
    eventHandle.on_user_mute_video         =   byte_rtc_on_user_mute_video;

    eventHandle.on_audio_data              =   byte_rtc_on_audio_data;
    eventHandle.on_video_data              =   byte_rtc_on_video_data;

    eventHandle.on_key_frame_gen_req       =   byte_rtc_on_keyframe_gen_req;
    eventHandle.on_target_bitrate_changed  =   byte_rtc_on_target_bitrate_changed;
    eventHandle.on_token_privilege_will_expire =   byte_rtc_on_token_privilege_will_expire;
    eventHandle.on_fini_notify       =   byte_rtc_on_fini_notify;
    eventHandle.on_message_received = byte_rtc_on_message_received; 


    {
        int chat_ret = volc_engine_start_chat_checked(content);
        if (chat_ret != 0) {
            log_error("volc_engine_init: StartVoiceChat failed ret=%d", chat_ret);
            return -1;
        }
    }

    byte_rtc_engine_t engine =  byte_rtc_create(g_byte_rtc_data.app_id,&eventHandle);
    g_byte_rtc_data.engine = engine;

    byte_rtc_set_log_level(engine,BYTE_RTC_LOG_LEVEL_INFO);
	byte_rtc_config_log(engine, "/tmp/volc_log/", 1*1024*1024, 1);

#if defined BYTE_RTC_API_VERSION_NUM   && BYTE_RTC_API_VERSION_NUM >= 0x1003  
    byte_rtc_room_options_t option = { true, true, true, true};
#else 
    byte_rtc_room_options_t option = { true,false};
#endif


    log_info("rtc byte_rtc_init: engine=%p", (void *)engine);
    int ret = byte_rtc_init(engine);
    if(ret)
    {
        printf("byte_rtc_init ret:%d \n",ret);
        return -1;
    }

    ret = byte_rtc_set_audio_codec(engine,live_stream_mgr.audio_codec);
    if(ret)
    {
        printf("byte_rtc_set_audio_codec ret:%d \n",ret);
        return -1;
    }
    ret = byte_rtc_set_video_codec(engine,VIDEO_CODEC_TYPE_H264);
    if(ret)
    {
        printf("byte_rtc_set_video_codec ret:%d \n",ret);
        return -1;
    }

    //set work path
    char set_work_path[256] = {0};

    int swp_n = snprintf(set_work_path, sizeof(set_work_path),
                         "{\"rtc\":{\"root_path\":\"%s\"}}", volc_work_path);
    if (swp_n < 0 || swp_n >= (int)sizeof(set_work_path)) {
        log_warn("rtc set_work_path truncated? n=%d path=%s", swp_n, volc_work_path);
    }

    byte_rtc_set_params(engine, set_work_path);
//    ret = byte_rtc_renew_token(engine,g_byte_rtc_data.room_id,g_byte_rtc_data.token);
//    if(ret)
//    {
//        printf("byte_rtc_renew_token ret:%d \n",ret);
//        return -1;
//    }


    log_info("rtc join_room: room=%s uid=%s opt{sub_a=%d sub_v=%d pub_a=%d pub_v=%d}",
             g_byte_rtc_data.room_id, g_byte_rtc_data.user_id,
             (int)option.auto_subscribe_audio, (int)option.auto_subscribe_video,
             (int)option.auto_publish_audio, (int)option.auto_publish_video);
    int token_ret = volc_rtc_token_refresh(VOLC_RTC_TOKEN_DEFAULT_TTL_SEC);
    if (token_ret != 0) {
        if (token_ret == VOLC_ENGINE_ERR_NETWORK) {
            log_error("volc_engine_init: rtc token skipped, network not ready");
            return VOLC_ENGINE_ERR_NETWORK;
        }
        log_error("volc_engine_init: rtc token refresh failed");
        return VOLC_ENGINE_ERR;
    }

    /*
     * Pre-open mic + AO before join_room. Welcome TTS often lands within tens of
     * ms after join; opening AI/AO only in the join callback costs ~1.5s and
     * drops the head of "你好呀，我是小凯". Uplink stays gated by rtc_ok until join.
     */
    log_info("volc: pre-open audio (mic+AO) before join_room");
    (void)start_send_audio();

    ret = byte_rtc_join_room(engine,g_byte_rtc_data.room_id,g_byte_rtc_data.user_id,g_byte_rtc_data.token,&option);
    if(ret)
    {
        printf("byte_rtc_join_room ret:%d \n",ret);
        return -1;
    }
    log_info("rtc join_room options applied: sub_a=%d sub_v=%d pub_a=%d pub_v=%d",
             (int)option.auto_subscribe_audio,
             (int)option.auto_subscribe_video,
             (int)option.auto_publish_audio,
             (int)option.auto_publish_video);
    //volc_engine_auth(content,byte_rtc_perform_req_callback,&g_byte_rtc_data);
//	byte_rtc_mute(engine, g_byte_rtc_data.room_id, g_byte_rtc_data.user_id, 0, true);
//	byte_rtc_mute(engine, g_byte_rtc_data.room_id, g_byte_rtc_data.user_id, 1, true);


    g_volc_engine_inited = 1;
    log_info("volc_engine_init done (await join callback for rtc_ok)");
    return 0;
}


int volc_engine_deinit() 
{
    if (s_volc_deinit_in_progress) {
        return 0;
    }
    s_volc_deinit_in_progress = 1;

    log_info("volc_engine_deinit: inited=%d engine=%p joined=%d",
             g_volc_engine_inited, (void *)g_byte_rtc_data.engine,
             g_byte_rtc_data.channel_joined ? 1 : 0);

    /* 先于其它步骤，避免 destroy 过程中仍有回调入队/写多媒体 */
    s_volc_rtc_callbacks_ok = 0;
    volc_dl_pend_clear();

    /* 立刻禁止下行播放与上行发送（SDK 仍可回调，但回调内直接 return） */
    g_volc_engine_play = 0;

    stop_send_video();
    stop_send_audio();

    /* 先关 talk 输出流并静音 AO，避免 ring buffer 继续播 RTC 已拉取的 PCM */
    if (live_stream_mgr.multimedia && live_stream_mgr.multimedia->audio) {
        if (live_stream_mgr.multimedia->audio->out_set_volume) {
            (void)live_stream_mgr.multimedia->audio->out_set_volume(0, 0);
        }
        if (live_stream_mgr.multimedia->audio->talk_stop) {
            (void)live_stream_mgr.multimedia->audio->talk_stop(0);
        }
    }

    volc_rtc_engine_release();

    /* 停止云端 VoiceChat 任务，避免下次进入返回 task already started */
    volc_engine_stop_cloud_task();

    /* 多媒体生命周期仍由系统主程序统一管理，这里不再调用 multimedia_close()，
     * 以避免 Volc 内部线程尚未完全退出时提前销毁 HAL/manager 造成崩溃。 */

    g_volc_engine_inited = 0;
    s_volc_deinit_in_progress = 0;
    log_info("volc_engine_deinit done");
    return 0;
}

