#ifndef APPLICATION_VOLC_ENGINE_APP_H
#define APPLICATION_VOLC_ENGINE_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#define VOLC_ENGINE_OK           0
#define VOLC_ENGINE_ERR          -1
/** Device has no usable IPv4 (WiFi/Ethernet); do not generate RTC token. */
#define VOLC_ENGINE_ERR_NETWORK  -2

int volc_engine_init(void);
int volc_engine_deinit(void);

typedef int (*volc_engine_recv_text_callback_t)(int user_id, char *text, int text_len,
                                                 long long text_time, int paragraph);
int volc_engine_set_text_cb(volc_engine_recv_text_callback_t cb);

/* 按键说话（PTT）：0=上行发 G.711 静音帧（保持 RTP 节拍，服务端 VAD 静默），
 * 1=正常采样上行。进入页面默认 0，volc_engine_init() 复位为 0。 */
int volc_engine_set_voice_enable(int enable);
int volc_engine_get_voice_enable(void);

/* 上行画面开关：仅门控 send_video 回调，preview_bg 流注册保持，不重开 VENC。
 * 默认 0（UI 默认文本模式），volc_engine_init() 复位为 0；UI 连接成功后按当前模式 re-apply。 */
int volc_engine_set_video_enable(int enable);

/* 1=智能体 TTS 正在播放（半双工上行门状态，供 UI 状态胶囊用）。 */
int volc_engine_bot_speaking(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_VOLC_ENGINE_APP_H */
