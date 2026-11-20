// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "VOLC_RTC_TOKEN"

#include "volc_engine_rtc_token.h"

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

#define VOLC_RTC_TOKEN_VERSION      "001"
#define VOLC_RTC_APP_ID_LEN         24

#define VOLC_RTC_PRIV_PUBLISH_STREAM    0
#define VOLC_RTC_PRIV_PUBLISH_AUDIO     1
#define VOLC_RTC_PRIV_PUBLISH_VIDEO     2
#define VOLC_RTC_PRIV_PUBLISH_DATA      3
#define VOLC_RTC_PRIV_SUBSCRIBE_STREAM  4

#define VOLC_RTC_MSG_BUF_SIZE         512
#define VOLC_RTC_CONTENT_BUF_SIZE     640
#define VOLC_RTC_MIN_VALID_UNIX_TIME  1577836800 /* 2020-01-01 UTC */

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
} volc_byte_buf_t;

static int volc_buf_init(volc_byte_buf_t *buf, uint8_t *storage, size_t cap)
{
    if (!buf || !storage || cap == 0) {
        return -1;
    }
    buf->data = storage;
    buf->cap = cap;
    buf->len = 0;
    return 0;
}

static int volc_buf_append(volc_byte_buf_t *buf, const void *src, size_t n)
{
    if (!buf || !src || buf->len + n > buf->cap) {
        return -1;
    }
    memcpy(buf->data + buf->len, src, n);
    buf->len += n;
    return 0;
}

static int volc_buf_put_u16_le(volc_byte_buf_t *buf, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xffU);
    b[1] = (uint8_t)((v >> 8) & 0xffU);
    return volc_buf_append(buf, b, sizeof(b));
}

static int volc_buf_put_u32_le(volc_byte_buf_t *buf, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xffU);
    b[1] = (uint8_t)((v >> 8) & 0xffU);
    b[2] = (uint8_t)((v >> 16) & 0xffU);
    b[3] = (uint8_t)((v >> 24) & 0xffU);
    return volc_buf_append(buf, b, sizeof(b));
}

static int volc_buf_put_bytes(volc_byte_buf_t *buf, const uint8_t *raw, size_t raw_len)
{
    if (raw_len > 0xFFFFU) {
        return -1;
    }
    if (volc_buf_put_u16_le(buf, (uint16_t)raw_len) != 0) {
        return -1;
    }
    return volc_buf_append(buf, raw, raw_len);
}

static int volc_buf_put_string(volc_byte_buf_t *buf, const char *s)
{
    if (!s) {
        return -1;
    }
    return volc_buf_put_bytes(buf, (const uint8_t *)s, strlen(s));
}

static int volc_buf_put_privilege_map(volc_byte_buf_t *buf, uint32_t expire_at)
{
    static const uint16_t keys[] = {
        VOLC_RTC_PRIV_PUBLISH_STREAM,
        VOLC_RTC_PRIV_PUBLISH_AUDIO,
        VOLC_RTC_PRIV_PUBLISH_VIDEO,
        VOLC_RTC_PRIV_PUBLISH_DATA,
        VOLC_RTC_PRIV_SUBSCRIBE_STREAM,
    };
    size_t i;

    if (volc_buf_put_u16_le(buf, (uint16_t)(sizeof(keys) / sizeof(keys[0]))) != 0) {
        return -1;
    }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (volc_buf_put_u16_le(buf, keys[i]) != 0) {
            return -1;
        }
        if (volc_buf_put_u32_le(buf, expire_at) != 0) {
            return -1;
        }
    }
    return 0;
}

static int volc_hmac_sha256_bytes(const uint8_t *key, size_t key_len,
                                  const uint8_t *msg, size_t msg_len,
                                  uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (!info || !key || !msg || !out) {
        return -1;
    }
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
}

static uint32_t volc_rtc_random_nonce(void)
{
    static int seeded;
    if (!seeded) {
        srand((unsigned int)(time(NULL) ^ (uintptr_t)&seeded));
        seeded = 1;
    }
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

int volc_rtc_token_generate(char *out_token, size_t out_token_len,
                            const char *app_id, const char *app_key,
                            const char *room_id, const char *user_id,
                            unsigned int ttl_sec)
{
    uint8_t msg_storage[VOLC_RTC_MSG_BUF_SIZE];
    uint8_t content_storage[VOLC_RTC_CONTENT_BUF_SIZE];
    volc_byte_buf_t msg_buf;
    volc_byte_buf_t content_buf;
    uint8_t signature[32];
    uint32_t nonce;
    uint32_t issued_at;
    uint32_t expire_at;
    size_t b64_len = 0;
    int ret;

    if (!out_token || out_token_len < 2 || !app_id || !app_key || !room_id || !user_id) {
        return -1;
    }
    if (strlen(app_id) != VOLC_RTC_APP_ID_LEN) {
        log_error("rtc token: invalid app_id length (expect %d)", VOLC_RTC_APP_ID_LEN);
        return -1;
    }
    if (app_key[0] == '\0' || room_id[0] == '\0' || user_id[0] == '\0') {
        log_error("rtc token: missing app_key/room_id/user_id");
        return -1;
    }
    if (ttl_sec == 0) {
        ttl_sec = VOLC_RTC_TOKEN_DEFAULT_TTL_SEC;
    }

    issued_at = (uint32_t)time(NULL);
    if (issued_at < VOLC_RTC_MIN_VALID_UNIX_TIME) {
        log_error("rtc token: system time invalid (now=%u), sync NTP first", issued_at);
        return -1;
    }
    expire_at = issued_at + ttl_sec;
    nonce = volc_rtc_random_nonce();

    if (volc_buf_init(&msg_buf, msg_storage, sizeof(msg_storage)) != 0) {
        return -1;
    }
    if (volc_buf_put_u32_le(&msg_buf, nonce) != 0 ||
        volc_buf_put_u32_le(&msg_buf, issued_at) != 0 ||
        volc_buf_put_u32_le(&msg_buf, expire_at) != 0 ||
        volc_buf_put_string(&msg_buf, room_id) != 0 ||
        volc_buf_put_string(&msg_buf, user_id) != 0 ||
        volc_buf_put_privilege_map(&msg_buf, expire_at) != 0) {
        log_error("rtc token: pack msg failed");
        return -1;
    }

    if (volc_hmac_sha256_bytes((const uint8_t *)app_key, strlen(app_key),
                               msg_buf.data, msg_buf.len, signature) != 0) {
        log_error("rtc token: hmac failed");
        return -1;
    }

    if (volc_buf_init(&content_buf, content_storage, sizeof(content_storage)) != 0) {
        return -1;
    }
    if (volc_buf_put_bytes(&content_buf, msg_buf.data, msg_buf.len) != 0 ||
        volc_buf_put_bytes(&content_buf, signature, sizeof(signature)) != 0) {
        log_error("rtc token: pack content failed");
        return -1;
    }

    ret = snprintf(out_token, out_token_len, "%s%s", VOLC_RTC_TOKEN_VERSION, app_id);
    if (ret < 0 || (size_t)ret >= out_token_len) {
        return -1;
    }

    ret = mbedtls_base64_encode((unsigned char *)out_token + strlen(out_token),
                                out_token_len - strlen(out_token), &b64_len,
                                content_buf.data, content_buf.len);
    if (ret != 0) {
        log_error("rtc token: base64 encode failed ret=%d", ret);
        return -1;
    }
    out_token[strlen(out_token) + b64_len] = '\0';
    return 0;
}
