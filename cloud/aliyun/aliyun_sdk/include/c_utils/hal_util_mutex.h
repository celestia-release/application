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

#ifndef APPLICATION_HAL_UTIL_MUTEX_H__
#define APPLICATION_HAL_UTIL_MUTEX_H__

#ifdef __cplusplus
extern "C"{
#endif

/* mutex struct */
typedef struct _util_mutex_t {
    void *mutex_handle;
} util_mutex_t;

#include "c_utils.h"

#define MUTEX_WAIT_FOREVER  0x7FFFFFFF

/*****************************************************
 * Function: util_mutex_create
 * Description: Create a mutex.
 * Parameter: None.
 * Return: util_mutex_t * --- pointer to mutex structure.
 ****************************************************/
util_mutex_t * util_mutex_create(void);

/*****************************************************
 * Function: util_mutex_delete
 * Description: Delete the given mutex.
 * Parameter:
 *     mutex --- pointer to mutex structure.
 * Return: None.
 ****************************************************/
void util_mutex_delete(util_mutex_t *mutex);

/*****************************************************
 * Function: util_mutex_lock
 * Description: Lock the given mutex with optional timeout.
 * Parameter:
 *     mutex --- pointer to mutex structure.
 *     timeout --- wait time in ms; MUTEX_WAIT_FOREVER for no limit.
 * Return: int32_t --- util_result_t.
 ****************************************************/
int32_t util_mutex_lock(util_mutex_t *mutex, int32_t timeout);

/*****************************************************
 * Function: util_mutex_unlock
 * Description: Unlock the given mutex.
 * Parameter:
 *     mutex --- pointer to mutex structure.
 * Return: int32_t --- util_result_t.
 ****************************************************/
int32_t util_mutex_unlock(util_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif
