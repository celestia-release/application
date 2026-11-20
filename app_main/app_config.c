// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * Runtime config parser and app_main_run (in-process).
 * Reads settings.json: program, launch_mode, modules; loads SOs (type=so, enable=true); then UI or headless.
 */
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include "cJSON.h"
#include "log.h"

#define LOG_TAG "APP MAIN"

#define MAX_PATH 256
#define DEFAULT_GUI_CONFIG "/etc/config/app_gui_config.ini"

/* Internal: parsed config held in static for app_main_get_* and app_main_run. */
static struct {
    char *path_loaded;
    cJSON *json_root;
    bool launch_mode_ui;
    char gui_config_path[MAX_PATH];
    app_module_item_t *modules_head;
} g_config;

static void free_module_list(app_module_item_t **head)
{
    app_module_item_t *p = *head;
    while (p) {
        app_module_item_t *next = p->next;
        free((void *)p->name);
        free((void *)p->type);
        free((void *)p->so_path);
        free((void *)p->config_path);
        free(p);
        p = next;
    }
    *head = NULL;
}

void app_main_init(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.launch_mode_ui = false;
    g_config.gui_config_path[0] = '\0';
}

static int parse_modules(cJSON *modules_obj)
{
    if (!cJSON_IsObject(modules_obj))
        return 0;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, modules_obj) {
        if (!cJSON_IsObject(child))
            continue;
        cJSON *enable_item = cJSON_GetObjectItem(child, "enable");
        cJSON *type_item = cJSON_GetObjectItem(child, "type");
        bool enable = cJSON_IsTrue(enable_item);
        const char *type = type_item && cJSON_IsString(type_item) ? type_item->valuestring : "static";
        const char *so_path = NULL;
        cJSON *so_item = cJSON_GetObjectItem(child, "so_path");
        if (so_item && cJSON_IsString(so_item))
            so_path = so_item->valuestring;
        const char *config_path = NULL;
        cJSON *cfg_item = cJSON_GetObjectItem(child, "config_path");
        if (cfg_item && cJSON_IsString(cfg_item))
            config_path = cfg_item->valuestring;

        app_module_item_t *item = (app_module_item_t *)calloc(1, sizeof(app_module_item_t));
        if (!item)
            continue;
        item->name = child->string ? strdup(child->string) : NULL;
        item->enable = enable;
        item->type = type ? strdup(type) : NULL;
        item->so_path = so_path ? strdup(so_path) : NULL;
        item->config_path = config_path ? strdup(config_path) : NULL;
        item->next = g_config.modules_head;
        g_config.modules_head = item;
    }
    return 0;
}

int app_main_load_config(const char *path)
{
    const char *use_path;
    if (path && path[0]) {
        use_path = path;
    } else {
        /* Default: settings.json -> app_runtime.json -> app_user.json (cwd then /etc/config) */
        if (access(APP_CONFIG_DEFAULT_RELATIVE, F_OK) == 0)
            use_path = APP_CONFIG_DEFAULT_RELATIVE;
        else if (access(APP_CONFIG_LEGACY_RELATIVE, F_OK) == 0)
            use_path = APP_CONFIG_LEGACY_RELATIVE;
        else if (access(APP_CONFIG_DEFAULT_SYSTEM, F_OK) == 0)
            use_path = APP_CONFIG_DEFAULT_SYSTEM;
        else if (access(APP_CONFIG_LEGACY_SYSTEM, F_OK) == 0)
            use_path = APP_CONFIG_LEGACY_SYSTEM;
        else if (access(APP_CONFIG_LEGACY_USER, F_OK) == 0)
            use_path = APP_CONFIG_LEGACY_USER;
        else
            use_path = APP_CONFIG_DEFAULT_SYSTEM;
    }

    if (g_config.json_root) {
        cJSON_Delete(g_config.json_root);
        g_config.json_root = NULL;
    }
    free(g_config.path_loaded);
    g_config.path_loaded = NULL;
    free_module_list(&g_config.modules_head);
    g_config.launch_mode_ui = false;
    g_config.gui_config_path[0] = '\0';

    if (access(use_path, F_OK) != 0) {
        log_error("settings.json not found: %s (use default headless, no modules)", use_path);
        /* Degrade: no config, default headless, no SO load */
        return 0;
    }

    FILE *fp = fopen(use_path, "r");
    if (!fp) {
        log_error("failed to open %s", use_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return -1;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, (size_t)size, 1, fp) != 1) {
        free(buf);
        fclose(fp);
        return -1;
    }
    buf[size] = '\0';
    fclose(fp);

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        const char *err = cJSON_GetErrorPtr();
        log_error("JSON parse error: %s", err ? err : "unknown");
        return -1;
    }

    g_config.json_root = json;
    g_config.path_loaded = strdup(use_path);

    /* launch_mode: default headless */
    cJSON *mode = cJSON_GetObjectItem(json, "launch_mode");
    if (mode && cJSON_IsString(mode) && mode->valuestring &&
        strcmp(mode->valuestring, APP_CONFIG_LAUNCH_MODE_UI) == 0)
        g_config.launch_mode_ui = true;

    /* modules */
    cJSON *modules = cJSON_GetObjectItem(json, "modules");
    if (modules)
        parse_modules(modules);

    /* wearable / tablet config_path for app_lv_open */
    for (app_module_item_t *p = g_config.modules_head; p; p = p->next) {
        if (p->enable && p->config_path && p->name &&
            strstr(p->name, "gui") != NULL) {
            strncpy(g_config.gui_config_path, p->config_path, MAX_PATH - 1);
            g_config.gui_config_path[MAX_PATH - 1] = '\0';
            break;
        }
    }
    if (g_config.gui_config_path[0] == '\0')
        strncpy(g_config.gui_config_path, DEFAULT_GUI_CONFIG, MAX_PATH - 1);

    {
        const char *mode_str = "headless";
        if (g_config.launch_mode_ui)
            mode_str = "ui";
        else if (mode && cJSON_IsString(mode) && mode->valuestring)
            mode_str = mode->valuestring; /* headless | ipcamera | … */
        log_info("loaded config: %s launch_mode=%s", use_path, mode_str);
    }
    return 0;
}

bool app_main_get_launch_mode_ui(void)
{
    return g_config.launch_mode_ui;
}

const char *app_main_get_gui_config_path(void)
{
    return g_config.gui_config_path[0] ? g_config.gui_config_path : DEFAULT_GUI_CONFIG;
}

typedef struct so_loaded {
    void *handle;
    const char *so_path;
    int (*start)(void);
    struct so_loaded *next;
} so_loaded_t;

static so_loaded_t *g_so_loaded;

/* dlopen + module_init only; module_start runs later via app_main_start_so_modules(). */
static void load_so_module(const app_module_item_t *mod)
{
    if (!mod || !mod->enable || !mod->so_path || strcmp(mod->type, APP_CONFIG_MODULE_TYPE_SO) != 0)
        return;
    (void)dlerror();
    void *handle = dlopen(mod->so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char *err = dlerror();
        log_error("dlopen failed: %s (%s)", mod->so_path, err ? err : "unknown");
        return;
    }
    typedef int (*init_fn)(const char *);
    typedef int (*start_fn)(void);
    init_fn init = (init_fn)dlsym(handle, "module_init");
    start_fn start = (start_fn)dlsym(handle, "module_start");
    if (init && init(mod->config_path ? mod->config_path : "") != 0)
        log_warn("module_init failed for %s", mod->so_path);

    so_loaded_t *node = (so_loaded_t *)calloc(1, sizeof(so_loaded_t));
    if (!node) {
        log_error("oom tracking SO %s", mod->so_path);
        return;
    }
    node->handle = handle;
    node->so_path = mod->so_path;
    node->start = start;
    node->next = g_so_loaded;
    g_so_loaded = node;
}

void app_main_run(void)
{
    for (app_module_item_t *p = g_config.modules_head; p; p = p->next)
        load_so_module(p);
}

void app_main_start_so_modules(void)
{
    for (so_loaded_t *p = g_so_loaded; p; p = p->next) {
        if (!p->start)
            continue;
        if (p->start() != 0)
            log_warn("module_start failed for %s", p->so_path ? p->so_path : "?");
    }
}

void app_main_cleanup(void)
{
    if (g_config.json_root) {
        cJSON_Delete(g_config.json_root);
        g_config.json_root = NULL;
    }
    free(g_config.path_loaded);
    g_config.path_loaded = NULL;
    free_module_list(&g_config.modules_head);
    g_config.launch_mode_ui = false;
    g_config.gui_config_path[0] = '\0';
}
