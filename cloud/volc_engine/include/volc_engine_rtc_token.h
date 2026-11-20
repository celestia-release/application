#ifndef APPLICATION_VOLC_ENGINE_RTC_TOKEN_H
#define APPLICATION_VOLC_ENGINE_RTC_TOKEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default RTC token TTL: 24 hours. */
#define VOLC_RTC_TOKEN_DEFAULT_TTL_SEC 86400

/**
 * @brief Generate VolcEngine RTC AccessToken (version 001).
 *
 * Uses app_key + HMAC-SHA256 per official AccessToken format.
 * room_id must be the exact value passed to byte_rtc_join_room (e.g. G711A + ini room_id).
 *
 * @param out_token     Output buffer for token string.
 * @param out_token_len Size of out_token (recommend >= 256).
 * @param app_id        RTC application ID (24 characters).
 * @param app_key       RTC AppKey from console.
 * @param room_id       Full room name used in join_room.
 * @param user_id       User ID used in join_room.
 * @param ttl_sec       Validity in seconds from now (e.g. 86400).
 * @return 0 on success, -1 on error.
 */
int volc_rtc_token_generate(char *out_token, size_t out_token_len,
                            const char *app_id, const char *app_key,
                            const char *room_id, const char *user_id,
                            unsigned int ttl_sec);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_VOLC_ENGINE_RTC_TOKEN_H */
