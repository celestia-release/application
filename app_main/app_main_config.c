// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * JSON config parser and exec (moved from framework/runtime).
 */
#include "app_main_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

#include "cJSON.h"
#include "log.h"

#define LOG_TAG "APP MAIN"
#define MODULE_VERSION "V1.00"

#define CHECK_JSON_OBJECT(object, name, function, line) \
    do { \
        if (NULL == (object)) { \
            log_error("Failed to get JSON object: %s", name); \
            return APP_MAIN_ERROR_JSON_OBJECT_NOT_FOUND; \
        } \
    } while (0)

static int check_config(void *config, const char *function, int line)
{
    if (NULL == config) {
        log_error("Invalid config: config is NULL");
        return APP_MAIN_ERROR_INVALID_CONFIG;
    }
    cJSON *json = (cJSON *)config;
    cJSON *object = cJSON_GetObjectItem(json, "program");
    if ((NULL == object) || (NULL == object->valuestring) || (0 == strlen(object->valuestring))) {
        log_error("Invalid config: program field is missing or empty");
        return APP_MAIN_ERROR_CONFIG_PROGRAM_INVALID;
    }
    return APP_MAIN_OK;
}

static int check_object_value(const cJSON *object, const char *name, int type, const char *function, int line)
{
    if (cJSON_NULL == (cJSON_NULL & object->type))
        return APP_MAIN_ERROR_JSON_VALUE_NULL;
    if (type != (type & object->type))
        return APP_MAIN_ERROR_JSON_TYPE_MISMATCH;
    if (cJSON_String & object->type) {
        if ((NULL != object->valuestring) && (strlen(object->valuestring) > 0))
            return APP_MAIN_OK;
    }
    return APP_MAIN_ERROR_JSON_STRING_EMPTY;
}

static void log_object(const cJSON *object, const char *name)
{
    if (NULL != object) {
        if (cJSON_String & object->type) {
            if ((NULL != object->valuestring) && (strlen(object->valuestring) > 0)) {
                log_info("%s: %s", name, object->valuestring);
                return;
            }
        } else if (cJSON_Number & object->type) {
            log_info("%s: %d", name, object->valueint);
            return;
        }
    }
    log_info("%s: null", name);
}

char *app_main_module_version(void)
{
    return (char *)MODULE_VERSION;
}

int app_main_config_init(const char *path, void **config)
{
    if ((NULL == path) || (0 != access(path, F_OK))) {
        log_error("Invalid config path: %s", path ? path : "NULL");
        return APP_MAIN_ERROR_CONFIG_PATH_INVALID;
    }
    *config = NULL;
    FILE *fp = fopen(path, "r");
    if (NULL == fp) {
        log_error("Failed to open config file: %s", path);
        return APP_MAIN_ERROR_FILE_OPEN_FAILED;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        log_error("Config file is empty or invalid: %s", path);
        return APP_MAIN_ERROR_FILE_READ_FAILED;
    }
    fseek(fp, 0, SEEK_SET);
    char *buffer = (char *)malloc((size_t)file_size + 1);
    if (NULL == buffer) {
        fclose(fp);
        return APP_MAIN_ERROR_MEMORY_ALLOC_FAILED;
    }
    fread(buffer, (size_t)file_size, 1, fp);
    buffer[file_size] = '\0';
    fclose(fp);
    cJSON *json = cJSON_Parse(buffer);
    if (NULL == json) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
            log_error("JSON parse error before: %s", error_ptr);
        else
            log_error("JSON parse failed: unknown error");
        free(buffer);
        return APP_MAIN_ERROR_JSON_PARSE_FAILED;
    }
    free(buffer);
    *config = (void *)json;
    log_info("Configuration loaded successfully from: %s", path);
    return APP_MAIN_OK;
}

/* Idempotent release: config or *config NULL means nothing to release, return APP_MAIN_OK. */
int app_main_config_release(void **config)
{
    if (NULL == config || NULL == *config)
        return APP_MAIN_OK;
    if (APP_MAIN_OK != check_config(*config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    cJSON_Delete((cJSON *)(*config));
    *config = NULL;
    return APP_MAIN_OK;
}

int app_main_config_get_icon(void *config, char *icon, size_t icon_size)
{
    if (APP_MAIN_OK != check_config(config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if (NULL == icon || icon_size == 0)
        return APP_MAIN_ERROR_INVALID_PARAM;
    cJSON *json = (cJSON *)config;
    cJSON *object = cJSON_GetObjectItem(json, "icon");
    CHECK_JSON_OBJECT(object, "icon", __FUNCTION__, __LINE__);
    if (APP_MAIN_OK != check_object_value(object, "icon", cJSON_String, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_JSON_VALUE_NULL;
    if (0 == access(object->valuestring, F_OK)) {
        (void)snprintf(icon, icon_size, "%s", object->valuestring);
        return APP_MAIN_OK;
    }
    log_error("Icon file not found: %s", object->valuestring);
    return APP_MAIN_ERROR_FILE_NOT_FOUND;
}

int app_main_config_get_name(void *config, const char *language, char *name, size_t name_size)
{
    if (APP_MAIN_OK != check_config(config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if ((NULL == language) || (NULL == name) || name_size == 0)
        return APP_MAIN_ERROR_INVALID_PARAM;
    int ret = APP_MAIN_ERROR_JSON_OBJECT_NOT_FOUND;
    cJSON *json = (cJSON *)config;
    cJSON *array = cJSON_GetObjectItem(json, "name");
    CHECK_JSON_OBJECT(array, "name", __FUNCTION__, __LINE__);
    int name_num = cJSON_GetArraySize(array);
    for (int i = 0; i < name_num; i++) {
        cJSON *object = cJSON_GetArrayItem(array, i);
        CHECK_JSON_OBJECT(object, "name", __FUNCTION__, __LINE__);
        cJSON *subobject = cJSON_GetObjectItem(object, "language");
        CHECK_JSON_OBJECT(subobject, "language", __FUNCTION__, __LINE__);
        if (APP_MAIN_OK != check_object_value(subobject, "language", cJSON_String, __FUNCTION__, __LINE__))
            break;
        if (0 == strcmp(language, subobject->valuestring)) {
            subobject = cJSON_GetObjectItem(object, "string");
            CHECK_JSON_OBJECT(subobject, "string of name", __FUNCTION__, __LINE__);
            if (APP_MAIN_OK == check_object_value(subobject, "string", cJSON_String, __FUNCTION__, __LINE__)) {
                (void)snprintf(name, name_size, "%s", subobject->valuestring);
                ret = APP_MAIN_OK;
                break;
            }
            break;
        }
    }
    return ret;
}

int app_main_config_get_program(void *config, char *program, size_t program_size)
{
    if (APP_MAIN_OK != check_config(config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if (NULL == program || program_size == 0)
        return APP_MAIN_ERROR_INVALID_PARAM;
    cJSON *json = (cJSON *)config;
    cJSON *object = cJSON_GetObjectItem(json, "program");
    CHECK_JSON_OBJECT(object, "program", __FUNCTION__, __LINE__);
    if ((APP_MAIN_OK == check_object_value(object, "program", cJSON_String, __FUNCTION__, __LINE__)) &&
        (0 == access(object->valuestring, F_OK))) {
        (void)snprintf(program, program_size, "%s", object->valuestring);
        return APP_MAIN_OK;
    }
    log_error("Program file not found or invalid: %s", object && object->valuestring ? object->valuestring : "NULL");
    return APP_MAIN_ERROR_PROGRAM_NOT_FOUND;
}

static int get_launch_mode(void *config, int *headless)
{
    if (config == NULL || headless == NULL) {
        *headless = 0;
        return APP_MAIN_OK;
    }
    cJSON *json = (cJSON *)config;
    cJSON *object = cJSON_GetObjectItem(json, "launch_mode");
    if (object != NULL && cJSON_IsString(object) && object->valuestring != NULL) {
        if (strcmp(object->valuestring, "headless") == 0 ||
            strcmp(object->valuestring, "ipcamera") == 0) {
            *headless = 1;
            return APP_MAIN_OK;
        }
    }
    *headless = 0;
    return APP_MAIN_OK;
}

int app_main_exec_app(void **config)
{
    if (NULL == config || NULL == *config)
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if (APP_MAIN_OK != check_config(*config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    char program[256] = {0};
    if (APP_MAIN_OK != app_main_config_get_program(*config, program, sizeof(program))) {
        log_error("Program not found in configuration");
        return APP_MAIN_ERROR_PROGRAM_NOT_FOUND;
    }
    int headless = 0;
    get_launch_mode(*config, &headless);
    static const char *arg_ui = "--ui";
    static const char *arg_headless = "--headless";
    const char *argv[3];
    argv[0] = program;
    argv[1] = headless ? arg_headless : arg_ui;
    argv[2] = NULL;
    log_info("Launching application: %s (mode=%s)", program, headless ? "headless" : "ui");
    app_main_config_release(config);
    execv(program, (char *const *)argv);
    log_error("Failed to execute program: %s", program);
    return APP_MAIN_ERROR_EXEC_FAILED;
}

int app_main_save_config(void *config, const char *file)
{
    if (APP_MAIN_OK != check_config(config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    if (NULL == file)
        return APP_MAIN_ERROR_INVALID_PARAM;
    FILE *fp = fopen(file, "w");
    if (NULL == fp) {
        log_error("Failed to open file for writing: %s", file);
        return APP_MAIN_ERROR_FILE_OPEN_FAILED;
    }
    char *content = cJSON_Print((cJSON *)config);
    if (content == NULL) {
        fclose(fp);
        return APP_MAIN_ERROR_JSON_PRINT_FAILED;
    }
    fprintf(fp, "%s", content);
    fclose(fp);
    free(content);
    log_info("Configuration saved to: %s", file);
    return APP_MAIN_OK;
}

int app_main_printf_config(void *config)
{
    if (APP_MAIN_OK != check_config(config, __FUNCTION__, __LINE__))
        return APP_MAIN_ERROR_INVALID_CONFIG;
    cJSON *json = (cJSON *)config;
    log_info("Application configuration:");
    cJSON *object = cJSON_GetObjectItem(json, "id");
    log_object(object, "id");
    cJSON *array = cJSON_GetObjectItem(json, "name");
    if (NULL != array) {
        int name_num = cJSON_GetArraySize(array);
        log_info("name(%d):", name_num);
        for (int i = 0; i < name_num; i++) {
            object = cJSON_GetArrayItem(array, i);
            if (NULL == object) break;
            log_object(cJSON_GetObjectItem(object, "language"), "language");
            log_object(cJSON_GetObjectItem(object, "string"), "string");
        }
    } else {
        log_info("name: null");
    }
    log_object(cJSON_GetObjectItem(json, "description"), "description");
    log_object(cJSON_GetObjectItem(json, "icon"), "icon");
    log_object(cJSON_GetObjectItem(json, "program"), "program");
    log_object(cJSON_GetObjectItem(json, "launch_mode"), "launch_mode");
    log_object(cJSON_GetObjectItem(json, "config"), "config");
    log_object(cJSON_GetObjectItem(json, "enable"), "enable");
    return APP_MAIN_OK;
}
