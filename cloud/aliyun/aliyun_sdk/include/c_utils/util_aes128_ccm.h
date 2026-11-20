/*
 * Copyright 2026 Alibaba Group Holding Ltd.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http: *www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __UTIL_AES128_CCM_H__
#define __UTIL_AES128_CCM_H__

#include "c_utils.h"

#define AES_CCM_KEY_SIZE        16
#define AES_CCM_NONCE_SIZE      13
#define AES_CCM_TAG_SIZE        4

int32_t util_aes128_ccm_encrypt(uint8_t *out, uint32_t *out_len, uint8_t *in, uint32_t in_len, uint8_t nonce[AES_CCM_NONCE_SIZE], uint8_t key[AES_CCM_KEY_SIZE]);
// 注意：out指针指向的有效内存大小必须大于等于in + TAG
int32_t util_aes128_ccm_decrypt(uint8_t *out, uint32_t *out_len, uint8_t *in, uint32_t in_len, uint8_t nonce[AES_CCM_NONCE_SIZE], uint8_t key[AES_CCM_KEY_SIZE]);

#endif