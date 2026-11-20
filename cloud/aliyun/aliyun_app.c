// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/cloud/aliyun.
 *
 * Aliyun Bailian cloud app: device register, token, WebSocket client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>

#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#include "mbedtls/x509_crt.h"
#include "lib_c_storage.h"
#include "lib_c_sdk.h"
#include "libwebsockets.h"
#include "mbedtls/debug.h"
#include "circular_buffer.h"

#include <errno.h>
#include "log.h"
#include "system_config.h"

#define LOG_TAG "ALIYUN_APP"
#include "app_main.h"
#include "multimedia.h"
#include "notification.h"
#include "system_thread.h"
#include "gthread.h"


THREAD_RUN_DEF(play_a_th)

/* HTTP response buffer. */
typedef struct {

    char *data;
    size_t size;
} memory_struct_t;

/* User session data for WebSocket. */
typedef struct {
    GMutex Lock;
    char rx_buffer[1024 * 16];
    size_t rx_len;
    char tx_buffer[1024 * 16];
    size_t tx_len;
    int connected;
    int send_data_type;
    
    int64_t last_ping_sent;
    int64_t last_pong_received;
    int ping_retries;
    bool waiting_for_pong;

    struct lws *wsi;
} session_data_t;

#define ALIYUN_APP_PROTOCOL_NAME "wss"

/* Credentials loaded from config, do not hardcode. */
char c_appID[64] = {0};
char c_appSecret[64] = {0};
char c_DeviceName[64] = {0};
char c_APIKey[64] = {0};

/* bailian config path built at load via config_build_file_path("app_bailian_config.ini", ...) */

char c_header[64] = {0};

char c_register_api[64] = "/api/device/v1/register";
char c_register_host[128] = "https://bailian.multimodalagent.aliyuncs.com/api/device/v1/register";
char c_gettoken_host[128] = "https://bailian.multimodalagent.aliyuncs.com/api/token/v1/getToken";

static int wss_event_flg = 1;

/* TLS CA certificate. */
const char *g_ca_global_sign_root_ca_r3 = {
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"  
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\n"
    "gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\n"
    "KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\n"
    "QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\n"
    "XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\n"
    "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\n"
    "LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\n"
    "RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\n"
    "jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\n"
    "6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\n"
    "mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\n"
    "Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\n"
    "WD9f\n"
    "-----END CERTIFICATE-----\n"
};

memory_struct_t http_recv = {0};
static multimedia_t *g_multimedia = NULL;
static notification_t *g_aliyun_notification = NULL;
int client_handle = -1;

session_data_t session = {0};
pthread_t g_wss_event;

static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,void *user, void *in, size_t len);

/* LWS protocol list for WebSocket. */
static struct lws_protocols protocols[] = {
    {
        ALIYUN_APP_PROTOCOL_NAME,
        callback_client,
        0,
        0,
    },
    { NULL, NULL, 0, 0 }
};

int perform_register_request(const char *url,char *content);
static int hw_recorder_callback(int handle, media_frame_info_t *frame, void* user_data);
void *timer_thread_func(void *arg);
int aliyun_app_deinit();

static int cJSON_GetItemStr(cJSON *object, const char *pString, char *pDst,const int iDstSize)
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

int aliyun_get_https_register_data(char *json, char *data, int data_len)
{
	cJSON *pRoot = NULL;
	pRoot = cJSON_Parse(json);
    if(pRoot == NULL)
    {
        printf("json=%s error.\n",json);
        return -1;
    }

    cJSON * pParam = cJSON_GetObjectItem(pRoot, "data");
    char *data_str = cJSON_Print(pParam);
    if (!data_str)
    {
        cJSON_Delete(pRoot);
        printf("Failed to convert data to string.\n");
        return -1;
    }

    snprintf(data, data_len, "%s", data_str);
    cJSON_free(data_str);
	cJSON_Delete(pRoot);

    return 0;
}

/* Callback: handle received data. */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    memory_struct_t *response = (memory_struct_t *)userp;

    char *ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        printf("Memory allocation failed\n");
        return 0;
    }

    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = '\0';

    return realsize;
}

char *http_req_register_request(const char *url, char *req)
{
    int ret = perform_register_request(url,req);

    if(ret == 0)
    {

        return http_recv.data;
    }

    return NULL;
}

char *http_req_get_token_request(const char *url, char *req)
{
    char in[512] = {0};

    memset(&http_recv,0,sizeof(http_recv));

    int ret = perform_register_request(url,req);

    if(ret == 0)
    {
        return http_recv.data;
    }

    return NULL;
}

void debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    const char *text;
    (void)handle;
    (void)userptr;

    switch (type) {
        case CURLINFO_TEXT:         text = "TEXT"; break;
        case CURLINFO_HEADER_OUT:   text = "HEADER OUT"; break;
        case CURLINFO_HEADER_IN:    text = "HEADER IN"; break;
        case CURLINFO_DATA_OUT:     text = "DATA OUT"; break;
        case CURLINFO_DATA_IN:      text = "DATA IN"; break;
        case CURLINFO_SSL_DATA_OUT: text = "SSL DATA OUT"; break;
        case CURLINFO_SSL_DATA_IN:  text = "SSL DATA IN"; break;
        default:                    text = "UNKNOWN"; break;
    }

    printf("[%s] %.*s", text, (int)size, data);
}

/* Perform POP request */
int perform_register_request(const char *url,char *content)
{
    CURL *curl;
    CURLcode res;

    /* Init curl. */
    curl = curl_easy_init();
    if (!curl)
    {
        log_error("Failed to init CURL");
        return 1;
    }

    /* Build request headers */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    /* Certificate */
    struct curl_blob ca_blob = {
        (void *)g_ca_global_sign_root_ca_r3,
        strlen(g_ca_global_sign_root_ca_r3),
        CURL_BLOB_COPY  /* or CURL_BLOB_NOCOPY */
    };

    /* Set CURL options */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PORT, 443L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");  /* POP method */

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(content));

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&http_recv);

    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);  /* Client cert. */
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    //curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_callback);
    /* Perform request */
    res = curl_easy_perform(curl);

    /* Check result. */
    if (res != CURLE_OK)
    {
        log_error("curl_easy_perform failed: %s", curl_easy_strerror(res));
    }
    else
    {
        /* Get HTTP status code */
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        log_info("HTTP 状态码: %ld", http_code);
    }

    /* Cleanup */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : 1;
}

int recorder_start()
{
    if(client_handle == -1 && g_multimedia && g_multimedia->audio)
    {
        g_multimedia->audio->stream_reg_client(0,&client_handle,hw_recorder_callback,NULL);
    }
    log_info("record_start: %d", client_handle);

    return 0;
}

int recorder_stop()
{
    if(client_handle >= 0 && g_multimedia && g_multimedia->audio)
    {
        g_multimedia->audio->stream_unreg_client(client_handle);
        client_handle = -1;
    }

    return 0;
}

int player_data_thread(void *param)
{
    uint8_t data[1000];
    while(THREAD_RUN_CHECK(play_a_th))
    {
        media_frame_info_t frame = {0};
        uint32_t size = 0;

        size = c_mmi_get_player_data(data, sizeof(data));
        // FILE *fp = fopen("/etc/config/aliyun/audio_push_down", "ab");
        // if (fp) {
        //     fwrite(data, 1, size, fp);
        //     fclose(fp);
        // }
        if(size == 0)
        {
            usleep(20);
            continue;
        }
        frame.iCodeType = 6;
        frame.iSampleBits = 16;
        frame.iSampleRate = 16000;
        frame.iChannelNum = 1;
        log_warn("get audio frame:%d", size);
        if(size > 1024 && g_multimedia && g_multimedia->audio)
        {
            frame.pData = data;
            frame.uiDataLen = 1024;
            g_multimedia->audio->talk_send_data(0,&frame);
            frame.pData = data + 1024;
            frame.uiDataLen = size - 1024;
            g_multimedia->audio->talk_send_data(0,&frame);
        }
        else if(g_multimedia && g_multimedia->audio)
        {
            frame.pData = data;
            frame.uiDataLen = size;
            g_multimedia->audio->talk_send_data(0,&frame);
        }
    }
    log_warn("player data out");
    return 0;
}

int player_start()
{
    THREAD_RUN_START(play_a_th,player_data_thread,NULL,0,0);

    return 0;
}

int player_stop()
{
    THREAD_RUN_STOP(play_a_th);

//    aliyun_audio->talk_stop(0);
    return 0;
}
#if 1
/* Push-to-talk mode */
#define CONFIG_MMI_MODE C_MMI_MODE_PUSH2TALK
static int32_t _button_up(void)
{
    /* Force stop recorder. */
    recorder_stop();

    /* Notify cloud to handle audio. */
    int iRet = c_mmi_stop_speech();
    if(iRet != 0)
    {
        log_error("stop speech failed:%d", iRet);
        return -1;
    }
    
    return 0;
}
static int32_t _button_down(void)
{
    /* Force speaker off */
    player_stop();
    /* Turn on mic; activate on key to avoid clipping */
    recorder_start();
    /* Notify cloud that audio will be sent; SDK triggers C_MMI_EVENT_SPEECH_START on response */
    int iRet = c_mmi_start_speech();
    if(iRet != 0)
    {
        log_error("start speech failed:%d", iRet);
        return -1;
    }
    log_info("speech start");

    return 0;
}
#else 
#define CONFIG_MMI_MODE C_MMI_MODE_PUSH2TALK
static int32_t _button_click(void)
{
    /* Force speaker off */
    player_stop();

    /* Turn on mic; activate on key to avoid clipping */
    recorder_start();

    /* Notify cloud that audio will be sent; SDK triggers C_MMI_EVENT_SPEECH_START on response */
    c_mmi_start_speech();

    return 0;
}
#endif

/* input_manager callback */
static int aliyu_input_cb(const input_event_t *ev, void *user)
{
    (void)user;
    
    /* Handle button ID 1 only (was iButtonId=1). Use linux_code in new API. */
    if (ev->device != INPUT_DEVICE_KEYPAD) {
        return 0;
    }
    
    /* Handle key event by action */
    if (ev->action == INPUT_ACTION_DOWN) {
        _button_down();
        log_info("button down, linux_code: %d", ev->linux_code);
    } else if (ev->action == INPUT_ACTION_UP) {
        _button_up();
        log_info("button release, linux_code: %d", ev->linux_code);
    }
    
    return 0;
}

int aliyun_timer_cb(void *param)
{
    session_data *data = (session_data *)param;
    int len = 0;
    uint8_t data_type = WEBSOCKET_OPCODE_BINARY;
    unsigned char *buff = NULL;

    g_mutex_lock(&data->Lock);
    len = c_mmi_get_send_data(&data_type, data->tx_buffer + data->tx_len + LWS_PRE, sizeof(data->tx_buffer));
    if(len > 0)
    {
        log_info("read data:%d, type:%d, arr:%d, datalen:%d", len, data_type, sizeof(data->tx_buffer), data->tx_len);
        data->tx_len += len;
        data->send_data_type = data_type;
        lws_callback_on_writable(data->wsi);
        g_mutex_unlock(&data->Lock);
    }
    else
    {
        g_mutex_unlock(&data->Lock);
        usleep(10*1000);
    }

    return 0;
}

static int hw_recorder_callback(int handle, media_frame_info_t *frame, void* user_data)
{
    /* Feed captured audio into SDK ringbuffer */
    (void)handle;
    (void)user_data;
    if(frame->data != NULL && frame->data_len != 0)
    {
        c_mmi_put_recorder_data(frame->data, frame->data_len);
    }

    return 0;
}

int extract_text_from_json(const char* json_string, int buffer_size) {
    cJSON *root = NULL;
    cJSON *payload = NULL;
    cJSON *output = NULL;
    cJSON *event = NULL;
    cJSON *text = NULL;
    char response[512] = {0};
    const char *text_content = NULL;
    const char *event_content = NULL;

    /* Parse JSON string. */
    root = cJSON_Parse(json_string);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            log_warn("JSON parse error: %s", error_ptr);
        }
        return -1;
    }

    /* Get payload field */
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (payload == NULL || !cJSON_IsObject(payload)) {
        //log_warn("payload not found or not object");
        cJSON_Delete(root);
        return -1;
    }

    /* Get output field */
    output = cJSON_GetObjectItemCaseSensitive(payload, "output");
    if (output == NULL || !cJSON_IsObject(output)) {
        //log_warn("output not found or not object");
        cJSON_Delete(root);
        return -1;
    }

    /* Get text field */
    text = cJSON_GetObjectItemCaseSensitive(output, "text");
    if (text == NULL || !cJSON_IsString(text)) {
        //log_warn("找不到text字段或不是字符串类型");
        cJSON_Delete(root);
        return -1;
    }

    /* Get text content */
    text_content = cJSON_GetStringValue(text);
    if (text_content != NULL) {
        /* Copy to output buffer */
        strcpy(response, text_content);
        //log_warn("extracted text: %s\n", response);
    } else {
        //log_warn("无法获取文本内容\n");
        cJSON_Delete(root);
        return -1;
    }

    event = cJSON_GetObjectItemCaseSensitive(output, "event");
    if (event == NULL) {
        //log_warn("event not found or not object");
        cJSON_Delete(root);
        return -1;
    }
    event_content = cJSON_GetStringValue(event);
    //log_warn("event_content:%s\n", event_content);
    if(event_content != NULL && !strcmp("SpeechContent", event_content))
    {
        log_warn("SpeechContent:%s\n", response);
        if (g_aliyun_notification && g_aliyun_notification->queue) {
            int handle = g_aliyun_notification->queue->create("qwen2ui_user");
            g_aliyun_notification->queue->write(handle, response, (int)buffer_size);
        }
    }
    else if(event_content != NULL && !strcmp("RespondingContent", event_content))
    {
        log_warn("RespondingContent:%s\n", response);
        if (g_aliyun_notification && g_aliyun_notification->queue) {
            int handle = g_aliyun_notification->queue->create("qwen2ui_bot");
            g_aliyun_notification->queue->write(handle, response, (int)buffer_size);
        }
    }

    if (root) {
        cJSON_Delete(root);
    }

    return 0;
}

/* WebSocket client callback. */
static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,void *user, void *in, size_t len)
{
    log_warn("reason:%d", reason);
    session_data *data = (session_data *)user;
    switch (reason)
    {
        case LWS_CALLBACK_PROTOCOL_INIT:
            lwsl_user("Protocol initialized\n");
            break;
        case LWS_CALLBACK_CLIENT_HTTP_BIND_PROTOCOL:
            lwsl_user("Protocol bound to connection\n");
            break;
        case LWS_CALLBACK_CONNECTING:
            lwsl_user("Connecting to server...\n");
            break;
        case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
            lwsl_user("Server accepted new client\n");
            break;
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lwsl_user("Connected to server\n");
            data->connected = 1;
            pthread_t timer_thread;
            if (pthread_create(&timer_thread, NULL, timer_thread_func, &session) != 0) {
                log_error("Failed to create timer thread");
                return -1;
            }
            break;
        case LWS_CALLBACK_CLIENT_CLOSED:
            lwsl_user("Connection closed\n");
            data->connected = 0;
            wss_event_flg = 0;
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            lwsl_user("Connection error: %s\n", in ? (char *)in : "unknown");
            data->connected = 0;
            wss_event_flg = 0;
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
            const int64_t now = util_get_timestamp();
            data->last_pong_received = now;
            
            /* Compute network latency (ms) */
            const int64_t latency = (now - data->last_ping_sent) / 1000;
            log_warn("PONG received, latency: %ld ms", latency);
            
            /* Reset state. */
            data->waiting_for_pong = false;
            data->ping_retries = 0;
            
            /* Handle PONG frame payload if present */
            if (len > 0) {
                log_warn("PONG frame: %.*s", (int)len, (const char *)in);
            }
            break;
        /* Received data. */
        case LWS_CALLBACK_CLIENT_RECEIVE:
            /* Do not exceed buffer size */
            //log_warn("recv:len:%d", len);
            int is_binary = lws_frame_is_binary(wsi);
            if(is_binary == 0)
            {
                c_mmi_analyze_recv_data(WEBSOCKET_OPCODE_TEXT, in, len);
                extract_text_from_json(in, len);
                //log_warn("recv:%s", in);
            }
            else
                c_mmi_analyze_recv_data(WEBSOCKET_OPCODE_BINARY, in, len);
            break;
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            unsigned char **p = (unsigned char **)in;
            unsigned char *end = (*p) + len;

            /* Handle Authorization header. */
            const char *auth_prefix = "Authorization: ";
            const size_t prefix_len = strlen(auth_prefix);

            if (strncmp(c_header, auth_prefix, prefix_len) == 0)
            {
                const char *token = c_header + prefix_len;
                size_t token_len = strlen(token);
                
                /* Remove trailing newline. */
                while (token_len > 0 && (token[token_len-1] == '\n' || token[token_len-1] == '\r')) {
                    token_len--;
                }
                
                /* Add header safely. */
                if (lws_add_http_header_by_name(wsi,
                    (unsigned char *)"Authorization",
                    (unsigned char *)token,
                    token_len,
                    p, end)) 
                {
                    log_error("Failed to add Authorization header");
                    return -1;
                }
            }
            else
            {
                log_error("Invalid Authorization header format");
                return -1;
            }
            break;
        /* Can send data. */
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            g_mutex_lock(&data->Lock);

            if (data->tx_len > 0)
            {
                /* Send buffer includes LWS_PRE prefix. */
                unsigned char *send_ptr = data->tx_buffer + LWS_PRE;
                size_t send_len = data->tx_len;

                enum lws_write_protocol data_type;
                switch(data->send_data_type)
                {
                    case WEBSOCKET_OPCODE_TEXT:
                        data_type = LWS_WRITE_TEXT;
                        break;
                    case WEBSOCKET_OPCODE_BINARY:
                        data_type = LWS_WRITE_BINARY;
                        break;
                    case WEBSOCKET_OPCODE_PING:
                        data_type = LWS_WRITE_PING;
                        break;
                    case WEBSOCKET_OPCODE_PONG:
                        data_type = LWS_WRITE_PONG;
                        break;
                    default:
                        data_type = LWS_WRITE_BINARY;
                }

                int sent = lws_write(wsi, send_ptr, send_len, data_type);
                if(sent < 0)
                {
                    log_error("Send failed %d", sent);
                    return -1;
                }
                else if (sent < send_len)
                {
                    log_info("Partial send (%d/%zu bytes)", sent, data->tx_len);
                    /* Update remaining data */
                    size_t remaining = send_len - sent;
                    data->tx_len = remaining;
                    memmove(data->tx_buffer + LWS_PRE, data->tx_buffer + LWS_PRE + sent, remaining);
                    /* Request writable again */
                    lws_callback_on_writable(wsi);
                }
                else
                {
                    /* Log send content safely (avoid binary) */
                    if (data->send_data_type == WEBSOCKET_OPCODE_TEXT)
                    {
                        log_info("Message sent");
                    }
                    else if(data->send_data_type == WEBSOCKET_OPCODE_BINARY)
                    {
                        log_info("Sent %zu bytes binary", send_len);
                    }
                    else
                    {
                        log_info("Sent type %d data, %zu bytes", data->send_data_type, send_len);
                    }

                    data->tx_len = 0;  /* Reset send buffer */
                }
            }
            g_mutex_unlock(&data->Lock);
            break;
        /* Peer initiated close. */
        case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: 
            /* Parse close status. */
            uint16_t status_code = 0;
            const char *reason_str = NULL;
            size_t reason_len = 0;

            if (len >= 2)
            {
                /* First 2 bytes are close status in network order. */
                const unsigned char *buf = (unsigned char *)in;
                status_code = (buf[0] << 8) | buf[1];
                if (len > 2) {
                    /* Remainder is close reason (may be UTF-8). */
                    reason_str = (const char *)(buf + 2);
                    reason_len = len - 2;
                }
            }

            /* Log close reason */
            if (reason_str && reason_len > 0)
            {
                /* Print at most 28 chars to avoid long string. */
                char reason_buf[129];
                size_t copy_len = reason_len > 128 ? 128 : reason_len;
                memcpy(reason_buf, reason_str, copy_len);
                reason_buf[copy_len] = '\0';
                log_warn("WebSocket closed by peer, status: %d, reason: %s", status_code, reason_buf);
            }
            else
            {
                log_warn("WebSocket closed by peer, status: %d", status_code);
            }
            data->connected = 0;
            wss_event_flg = 0;
            break;
        default:
            break;
    }
    
    return 0;
}

void *wss_event_thread(void *arg)
{
    struct lws_context *context = (struct lws_context*)arg;
    /* Event loop */
    while (wss_event_flg == 1) {
        lws_service(context, 100);  /* 100ms timeout */
    }

    if(context)
    {
        lws_context_destroy(context);
    }
    log_info("wss thread close");
}

static int32_t _wss_init(void)
{
    char *host = c_mmi_get_wss_host();
    char *port = c_mmi_get_wss_port();
    char *api = c_mmi_get_wss_api();
    char *header = c_mmi_get_wss_header();

    strcpy(c_header, header);
    struct lws_context_creation_info info = {0};
    struct lws_client_connect_info connect_info = {0};
    struct lws_context *context = NULL;

    /* Init context info */
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN; /* We do not listen. */
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_ca_mem = g_ca_global_sign_root_ca_r3;
    info.client_ssl_ca_mem_len = strlen(g_ca_global_sign_root_ca_r3);

    /* Set log level before creating context. */
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_USER, NULL);
    /* Create context. */
    context = lws_create_context(&info);
    if (!context)
    {
        log_error("Failed to create libwebsocket context");
        return 1;
    }
    /* Configure connection */
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    connect_info.address = host;
    connect_info.port = atoi(port);
    connect_info.path = api;
    connect_info.host = connect_info.address;
    connect_info.protocol = ALIYUN_APP_PROTOCOL_NAME;
    connect_info.ietf_version_or_minus_one = -1;        // 使用RFC6455协议
    connect_info.userdata = &session;                   /* User data. */
    connect_info.ssl_connection = LCCSCF_USE_SSL;       // 启用 SSL

    /* Create client connection. */
    struct lws *wsi = lws_client_connect_via_info(&connect_info);
    if (!wsi)
    {
        log_error("Failed to create client connection\n");
        lws_context_destroy(context);
        return -1;
    }

    session.wsi = wsi;
    if (pthread_create(&g_wss_event, NULL, wss_event_thread, context) != 0) {
        log_error("Failed to create timer thread");
        return -1;
    }

    return UTIL_SUCCESS;
}

static int32_t wss_deinit() {
    session_data data = session;
    if (data.wsi && data.connected) {
        lwsl_user("Closing WebSocket connection\n");
        /* Send close frame */
        lws_close_reason(data.wsi, LWS_CLOSE_STATUS_NORMAL, (unsigned char *)"Client closed", 14);
        lws_cancel_service(lws_get_context(data.wsi));
    }
    wss_event_flg = 0;

    pthread_join(g_wss_event, NULL);

    return 0;
}

static int32_t _mmi_event_callback(uint32_t event, void *param)
{
    log_info("event trigger:%d", event);
    switch (event)
    {
        case C_MMI_EVENT_USER_CONFIG:
            /* SDK config (buffer size, mode, voice, etc.) should be done in this callback */
            /* Configure MMI audio data transfer mode */
            log_info("set event config");
            c_mmi_set_audio_mode_ringbuffer(16 * 1024, 16 * 1024);
            /* Configure work mode */
            c_mmi_set_work_mode(CONFIG_MMI_MODE);
            /* Configure text options */
            c_mmi_set_text_mode(C_MMI_TEXT_MODE_BOTH);
            /* Configure options */
            c_mmi_set_voice_id("longxiaochun_v2");
            break;
        case C_MMI_EVENT_DATA_INIT:
            /* After SDK init, start WSS connection. */
            _wss_init();
            break;
        case C_MMI_EVENT_SPEECH_START:
            /* On audio uplink start. */
            recorder_start();
            break;
        case C_MMI_EVENT_ASR_END:
            /* Triggered when ASR ends */
            recorder_stop();
            break;
        case C_MMI_EVENT_TTS_START:
            /* On audio downlink start. */
            player_start();
            break;
        case C_MMI_EVENT_TTS_END:
            /* On audio downlink done. */
            player_stop();
            break;
        case C_MMI_EVENT_DATA_DEINIT:
            c_mmi_deinit();
            session.connected = 0;
            log_warn("aliyun server deinit");
            break;
        default:
            break;
    }

    return UTIL_SUCCESS;
}

/* Screen UI code (disabled). */
#if 0
static int32_t _mmi_event_callback(uint32_t event, void *param)
{
    /* Configure text options */
    c_mmi_set_text_mode(C_mmi_TEXT_MODE_BOTH);
    switch (event)
    {

        /* Subscribe to events */
        case C_MMI_EVENT_ASR_INCOMPLETE:
        {
            /* Partial ASR text. */
            char *text = param;
            UTIL_LOG_D("ASR [%s]", text);
            break;
        }
        case C_MMI_EVENT_ASR_COMPLETE:
        {
            /* Full ASR text. */
            char *text = param;
            UTIL_LOG_D("ASR C [%s]", text);
            break;
        }
        case C_MMI_EVENT_LLM_INCOMPLETE:
        {
            /* Returns in-progress LLM text (full) */
            char *text = param;
            UTIL_LOG_D("LLM [%s]", text);
            break;
        }
        case C_MMI_EVENT_LLM_COMPLETE:
        {
            /* Returns completed LLM text (full) */
            char *text = param;
            UTIL_LOG_D("LLM C [%s]", text);
            break;
        }
    }
}

#endif

int aliyun_app_register_dev()
{
    char req[C_SDK_REQ_LEN_REGISTER] = {0};
    if (c_storage_device_is_registered() == 0)
    {
        int64_t timestamp = util_get_timestamp();
        char time_str[128] = {0};
        snprintf(time_str, sizeof(time_str), "%lld", timestamp);
        /* Build registration string req from server timestamp */
        c_device_gen_register_req(req, time_str);
        /* Get device info from server; implement per customer */
        char *rsp = http_req_register_request(c_register_host,req);

        char data[1024] = {0};
        aliyun_get_https_register_data(rsp, data, sizeof(data));

        /* Parse and complete registration. */
        int32_t err = c_device_analyze_register_rsp(data);
        if (err)
        {
            log_error("device register failed:%d", err);
            return err;
        }
    }

    return 0;
}


int aliyun_app_get_token_rsp()
{
    char req[C_SDK_REQ_LEN_GET_TOKEN];

    int64_t timestamp =  util_get_timestamp();
    char time_str[128] = {0};
    snprintf(time_str, sizeof(time_str), "%lld", timestamp);

    // 根据服务端下发时间戳timestamp，生成注册信息字串req
    int iRet = c_mmi_gen_get_token_req(req, time_str);
    if(iRet != 0)
    {
        log_error("get token req failed:%d", iRet);
        return -1;
    }

    // 获取服务端返回设备信息，该接口需客户自行实现
    char *rsp = http_req_get_token_request(c_gettoken_host,req);
    char data[1024] = {0};
    aliyun_get_https_register_data(rsp, data, sizeof(data));

    /* Parse interaction token */
    int32_t err = c_mmi_analyze_get_token_rsp(data);
    if (err)
    {
        log_error("get token failed:%d", err);
        return err;
    }

    return 0;
}

int aliyun_app_setup_connection()
{
    /* Register event callbacks */
    int iRet = c_mmi_register_event_callback(_mmi_event_callback);
    if(iRet != 0)
    {
        log_error("register callback event falied:%d", iRet);
        return -1;
    }

    return 0;
}

void *timer_thread_func(void *arg)
{
    session_data *data = &session;
    while(data->connected == 1)
    {
        aliyun_timer_cb(data);
    }

    log_info("wss thread close");
    return NULL;
}

static int byte_ali_bailian_config_init()
{
    char room_id[64] = {0};
    char bailian_cfg_path[256];
    if (config_build_file_path("app_bailian_config.ini", bailian_cfg_path, sizeof(bailian_cfg_path)) != 0) {
        return -1;
    }
    dictionary * handle = iniparser_load(bailian_cfg_path);
    if (!handle) {
        return -1;
    }

	sprintf(c_appID,"%s",iniparser_dict_getstring(handle, "bailian", "app_id", ""));
	sprintf(c_appSecret,"%s",iniparser_dict_getstring(handle, "bailian", "app_secret", ""));
	sprintf(c_APIKey,"%s",iniparser_dict_getstring(handle, "bailian", "api_key", ""));
	sprintf(c_DeviceName,"%s",iniparser_dict_getstring(handle, "bailian", "device_name", ""));

	iniparser_freedict(handle);

    return 0;
}

int aliyun_app_init()
{
    int iRet = 0;
    wss_event_flg = 1;
    g_multimedia = (multimedia_t *)app_main_get_multimedia();
    byte_ali_bailian_config_init();
    if(g_multimedia && g_multimedia->audio)
        g_multimedia->audio->out_set_volume(0, 80);
    /* Init input_manager */
    /* Input: use app_main handle (notification opened in app_main_platform_startup) */
    g_aliyun_notification = (notification_t *)app_main_get_notification();
    if (g_aliyun_notification && g_aliyun_notification->input)
        g_aliyun_notification->input->subscribe(0, 0, aliyu_input_cb, NULL);
    g_mutex_init(&session.Lock);

    /* Init SDK */
    c_mmi_sdk_init();
    c_mmi_license_disable();
    iRet = c_storage_device_is_registered();
    if (iRet == 0)
    {
        /* Clear storage */
        c_storage_reset();
        /* Preset AppId */
        c_storage_set_app_id_str(c_appID);
        /* Preset AppSecret */
        c_storage_set_app_secret_str(c_appSecret);
        /* Preset DeviceName */
        c_storage_set_device_name(c_DeviceName);
        /* Store device info */
        c_storage_save();
    }

    iRet = aliyun_app_register_dev();
    if(iRet != 0)
        return -1;

    iRet = aliyun_app_setup_connection();
    if(iRet != 0)
        return -1;

    iRet = aliyun_app_get_token_rsp();
    if(iRet != 0)
        return -1;

    return 0;
}

int aliyun_app_deinit()
{
    if(client_handle != -1)
    {
        recorder_stop();
    }

    /* Do not multimedia_close: app_main owns the instance via app_framework */
    g_multimedia = NULL;

    THREAD_RUN_STOP(play_a_th);
    /* Aliyun service exit. */
    c_mmi_deinit();
    log_warn("aliyun sdk deinit");
    /* Signal main loop to exit. */
    session.connected = 0;
    /* WSS server close. */
    wss_deinit();
    g_mutex_clear(&session.Lock);
    memset(&session, 0, sizeof(session_data));
    memset(&http_recv, 0, sizeof(memory_struct_t));
    /* Unsubscribe and deinit. */
    if (g_aliyun_notification && g_aliyun_notification->input) {
        g_aliyun_notification->input->unsubscribe(aliyu_input_cb, NULL);
        g_aliyun_notification->input->deinit();
    }
    if (g_aliyun_notification) {
        notification_close(g_aliyun_notification);
        g_aliyun_notification = NULL;
    }

    return 0;
}

