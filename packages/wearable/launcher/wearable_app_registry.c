#define LOG_TAG "WEARABLE_APP_REGISTRY"

#include "wearable_app_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "log.h"
#include "cJSON.h"

/* Apps list: settings.json -> installed_apps; fallback legacy installed_apps.json. */
#define APP_SETTINGS_CONFIG_PATH "/etc/config/settings.json"
#define APP_RUNTIME_CONFIG_PATH  "/etc/config/app_runtime.json" /* legacy */
#define APP_USER_CONFIG_PATH     "/etc/config/app_user.json"    /* older legacy */

static FILE *app_settings_fopen_read(void)
{
    static const char *const paths[] = {
        APP_SETTINGS_CONFIG_PATH,
        APP_RUNTIME_CONFIG_PATH,
        APP_USER_CONFIG_PATH,
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *fp = fopen(paths[i], "rb");
        if (fp)
            return fp;
    }
    return NULL;
}

#define INSTALLED_APPS_LEGACY_PATH "/etc/config/installed_apps.json"
#define WEARABLE_APPS_BASE_DIR         "/usr/apps"
static int s_loaded;
static int s_count;
static wearable_app_desc_t s_desc[WEARABLE_APP_MAX_DYNAMIC];
static gui_shell_app_desc_t s_shell_desc[WEARABLE_APP_MAX_DYNAMIC];
static char s_shell_so_path[WEARABLE_APP_MAX_DYNAMIC][512];

static int file_exists(const char *path)
{
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static void json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p;
    const char *q;
    size_t n;

    if (!json || !key || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    p = strstr(json, key);
    if (!p) {
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;
    q = strchr(p, '"');
    if (!q) {
        return;
    }
    n = (size_t)(q - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

static void resolve_entry_so(const wearable_app_desc_t *app, char *out, size_t out_len)
{
    size_t n;

    if (!app || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (app->entry_so[0] == '\0') {
        return;
    }
    if (app->entry_so[0] == '/') {
        snprintf(out, out_len, "%s", app->entry_so);
        return;
    }
    if (app->install_path[0] == '\0') {
        snprintf(out, out_len, "%s", app->entry_so);
        return;
    }
    n = strlen(app->install_path);
    if (app->install_path[n - 1] == '/') {
        snprintf(out, out_len, "%s%s", app->install_path, app->entry_so);
    } else {
        snprintf(out, out_len, "%s/%s", app->install_path, app->entry_so);
    }
}

static void entry_so_lib_fallback(const wearable_app_desc_t *app, char *out, size_t out_len)
{
    const char *fn;
    size_t n;

    if (!app || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (app->install_path[0] == '\0' || app->entry_so[0] == '\0') {
        return;
    }
    fn = strrchr(app->entry_so, '/');
    fn = fn ? fn + 1 : app->entry_so;
    if (fn[0] == '\0') {
        return;
    }
    n = strlen(app->install_path);
    if (app->install_path[n - 1] == '/') {
        snprintf(out, out_len, "%slib/%s", app->install_path, fn);
    } else {
        snprintf(out, out_len, "%s/lib/%s", app->install_path, fn);
    }
}

static void load_app_manifest(wearable_app_desc_t *app)
{
    char path[256] = {0};
    FILE *fp;
    long size;
    char *buf;
    size_t n;
    char name[64] = {0};
    char icon[128] = {0};
    char entry_so[256] = {0};
    char entry_symbol[64] = {0};
    char destroy_symbol[64] = {0};

    if (app->install_path[0] != '\0') {
        snprintf(path, sizeof(path), "%s/app.json", app->install_path);
    } else {
        snprintf(path, sizeof(path), "%s/%s/app.json", WEARABLE_APPS_BASE_DIR, app->app_id);
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    size = ftell(fp);
    if (size <= 0 || size > (512 * 1024)) {
        fclose(fp);
        return;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';

    json_get_string(buf, "\"name\"", name, sizeof(name));
    json_get_string(buf, "\"icon\"", icon, sizeof(icon));
    json_get_string(buf, "\"entry_so\"", entry_so, sizeof(entry_so));
    json_get_string(buf, "\"entry_symbol\"", entry_symbol, sizeof(entry_symbol));
    json_get_string(buf, "\"destroy_symbol\"", destroy_symbol, sizeof(destroy_symbol));

    if (name[0] != '\0') {
        snprintf(app->display_name, sizeof(app->display_name), "%s", name);
    }
    if (icon[0] != '\0') {
        if (icon[0] == '/') {
            snprintf(app->icon_path, sizeof(app->icon_path), "%s", icon);
        } else if (app->install_path[0] != '\0') {
            size_t base_len = strlen(app->install_path);

            if (app->install_path[base_len - 1] == '/') {
                snprintf(app->icon_path, sizeof(app->icon_path), "%s%s",
                         app->install_path, icon);
            } else {
                snprintf(app->icon_path, sizeof(app->icon_path), "%s/%s",
                         app->install_path, icon);
            }
        } else {
            snprintf(app->icon_path, sizeof(app->icon_path), "%s/%s/%s",
                     WEARABLE_APPS_BASE_DIR, app->app_id, icon);
        }
    }
    if (entry_so[0] != '\0') {
        snprintf(app->entry_so, sizeof(app->entry_so), "%s", entry_so);
    }
    if (entry_symbol[0] != '\0') {
        snprintf(app->entry_symbol, sizeof(app->entry_symbol), "%s", entry_symbol);
    }
    if (destroy_symbol[0] != '\0') {
        snprintf(app->destroy_symbol, sizeof(app->destroy_symbol), "%s", destroy_symbol);
    }

    if (app->entry_so[0] == '\0' || app->entry_symbol[0] == '\0') {
        log_warn("app.json missing entry_so or entry_symbol: app_id=%s install=%s",
                 app->app_id, app->install_path[0] ? app->install_path : "(unset)");
    }

    free(buf);
}

static void reset_registry(void)
{
    memset(s_desc, 0, sizeof(s_desc));
    s_count = 0;
}

void wearable_app_registry_scan_installed(void)
{
    DIR *dir = opendir(WEARABLE_APPS_BASE_DIR);

    reset_registry();
    if (!dir) {
        s_loaded = 1;
        return;
    }

    while (1) {
        struct dirent *ent = readdir(dir);
        char app_json[256] = {0};
        wearable_app_desc_t *app;

        if (!ent) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (s_count >= WEARABLE_APP_MAX_DYNAMIC) {
            break;
        }
        snprintf(app_json, sizeof(app_json), "%s/%s/app.json", WEARABLE_APPS_BASE_DIR, ent->d_name);
        if (!file_exists(app_json)) {
            continue;
        }

        app = &s_desc[s_count];
        app->index = s_count;
        snprintf(app->app_id, sizeof(app->app_id), "%s", ent->d_name);
        snprintf(app->display_name, sizeof(app->display_name), "%s", ent->d_name);
        snprintf(app->install_path, sizeof(app->install_path), "%s/%s",
                 WEARABLE_APPS_BASE_DIR, ent->d_name);
        app->enabled = 1;
        load_app_manifest(app);
        s_count++;
    }

    closedir(dir);
    s_loaded = 1;
}

int wearable_app_registry_save(void)
{
    cJSON *root = NULL;
    cJSON *installed = NULL;
    cJSON *apps = NULL;
    char *printed = NULL;
    FILE *fp;
    char *buf = NULL;
    long size;
    size_t n;
    int i;

    /* RMW settings.json (create if missing); preserve program/modules/language/wifi. */
    fp = app_settings_fopen_read();
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            size = ftell(fp);
            if (size > 0 && size < (1024 * 1024)) {
                rewind(fp);
                buf = (char *)malloc((size_t)size + 1);
                if (buf) {
                    n = fread(buf, 1, (size_t)size, fp);
                    buf[n] = '\0';
                    root = cJSON_Parse(buf);
                }
            }
        }
        fclose(fp);
    }
    free(buf);

    if (!root)
        root = cJSON_CreateObject();
    if (!root) {
        log_error("create app_user root failed");
        return -1;
    }

    cJSON_DeleteItemFromObject(root, "installed_apps");
    installed = cJSON_CreateObject();
    cJSON_AddNumberToObject(installed, "schema_version", 1);
    apps = cJSON_CreateArray();
    for (i = 0; i < s_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "app_id", s_desc[i].app_id);
        cJSON_AddStringToObject(item, "install_path",
                                s_desc[i].install_path[0] ? s_desc[i].install_path
                                                          : WEARABLE_APPS_BASE_DIR);
        cJSON_AddBoolToObject(item, "enabled", s_desc[i].enabled ? 1 : 0);
        cJSON_AddItemToArray(apps, item);
    }
    cJSON_AddItemToObject(installed, "apps", apps);
    cJSON_AddItemToObject(root, "installed_apps", installed);

    printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        log_error("print app_user failed");
        return -1;
    }

    fp = fopen(APP_SETTINGS_CONFIG_PATH, "wb");
    if (!fp) {
        log_error("open %s failed", APP_SETTINGS_CONFIG_PATH);
        free(printed);
        return -1;
    }
    fputs(printed, fp);
    fclose(fp);
    free(printed);
    return 0;
}

static int wearable_load_apps_from_json_root(cJSON *root)
{
    cJSON *installed;
    cJSON *apps;
    cJSON *item;

    if (!root)
        return -1;

    installed = cJSON_GetObjectItem(root, "installed_apps");
    if (cJSON_IsObject(installed))
        apps = cJSON_GetObjectItem(installed, "apps");
    else
        apps = cJSON_GetObjectItem(root, "apps"); /* legacy installed_apps.json */

    if (!cJSON_IsArray(apps))
        return -1;

    reset_registry();
    cJSON_ArrayForEach(item, apps) {
        cJSON *id;
        cJSON *path;
        cJSON *en;
        wearable_app_desc_t *app;

        if (!cJSON_IsObject(item) || s_count >= WEARABLE_APP_MAX_DYNAMIC)
            continue;
        id = cJSON_GetObjectItem(item, "app_id");
        if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0])
            continue;

        app = &s_desc[s_count];
        memset(app, 0, sizeof(*app));
        app->index = s_count;
        snprintf(app->app_id, sizeof(app->app_id), "%s", id->valuestring);
        snprintf(app->display_name, sizeof(app->display_name), "%s", id->valuestring);

        path = cJSON_GetObjectItem(item, "install_path");
        if (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) {
            snprintf(app->install_path, sizeof(app->install_path), "%s", path->valuestring);
        } else {
            snprintf(app->install_path, sizeof(app->install_path), "%s/%s",
                     WEARABLE_APPS_BASE_DIR, app->app_id);
        }

        en = cJSON_GetObjectItem(item, "enabled");
        app->enabled = cJSON_IsFalse(en) ? 0 : 1;
        load_app_manifest(app);
        s_count++;
    }
    s_loaded = 1;
    return (s_count > 0) ? 0 : -1;
}

void wearable_app_registry_refresh(void)
{
    const char *paths[] = {
        APP_SETTINGS_CONFIG_PATH,
        APP_RUNTIME_CONFIG_PATH,
        APP_USER_CONFIG_PATH,
        INSTALLED_APPS_LEGACY_PATH
    };
    int i;

    for (i = 0; i < 4; i++) {
        FILE *fp = fopen(paths[i], "rb");
        char *buf;
        long size;
        size_t n;
        cJSON *root;

        if (!fp)
            continue;
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            continue;
        }
        size = ftell(fp);
        if (size <= 0 || size > (1024 * 1024)) {
            fclose(fp);
            continue;
        }
        rewind(fp);
        buf = (char *)malloc((size_t)size + 1);
        if (!buf) {
            fclose(fp);
            continue;
        }
        n = fread(buf, 1, (size_t)size, fp);
        fclose(fp);
        buf[n] = '\0';

        root = cJSON_Parse(buf);
        free(buf);
        if (!root)
            continue;
        if (wearable_load_apps_from_json_root(root) == 0) {
            cJSON_Delete(root);
            return;
        }
        cJSON_Delete(root);
    }

    wearable_app_registry_scan_installed();
    wearable_app_registry_save();
    log_warn("installed apps not found in settings.json, generated from %s scan",
             WEARABLE_APPS_BASE_DIR);
}

const wearable_app_desc_t *wearable_app_registry_list(int *count)
{
    int i;

    if (!s_loaded) {
        wearable_app_registry_refresh();
    }
    if (count) {
        *count = s_count;
    }
    for (i = 0; i < s_count; i++) {
        load_app_manifest(&s_desc[i]);
    }
    return s_desc;
}

unsigned int wearable_app_registry_fill_gui_shell(gui_shell_app_desc_t *out, unsigned int out_max)
{
    int count = 0;
    const wearable_app_desc_t *apps;
    unsigned int written = 0;
    int i;

    if (!out || out_max == 0) {
        return 0;
    }

    apps = wearable_app_registry_list(&count);
    memset(s_shell_desc, 0, sizeof(s_shell_desc));
    memset(s_shell_so_path, 0, sizeof(s_shell_so_path));

    for (i = 0; apps && i < count && written < out_max; i++) {
        char resolved[512] = {0};
        char fallback[512] = {0};

        if (!apps[i].enabled) {
            continue;
        }
        if (apps[i].entry_so[0] == '\0' || apps[i].entry_symbol[0] == '\0') {
            log_warn("skip app without entry: %s", apps[i].app_id);
            continue;
        }

        resolve_entry_so(&apps[i], resolved, sizeof(resolved));
        entry_so_lib_fallback(&apps[i], fallback, sizeof(fallback));
        if (resolved[0] != '\0' && file_exists(resolved)) {
            snprintf(s_shell_so_path[written], sizeof(s_shell_so_path[written]), "%s", resolved);
        } else if (fallback[0] != '\0' && file_exists(fallback)) {
            snprintf(s_shell_so_path[written], sizeof(s_shell_so_path[written]), "%s", fallback);
        } else if (resolved[0] != '\0') {
            snprintf(s_shell_so_path[written], sizeof(s_shell_so_path[written]), "%s", resolved);
        } else {
            snprintf(s_shell_so_path[written], sizeof(s_shell_so_path[written]), "%s", fallback);
        }
        log_info("app %s so_path=%s", apps[i].app_id, s_shell_so_path[written]);

        s_shell_desc[written].id = apps[i].app_id;
        s_shell_desc[written].label = apps[i].display_name;
        s_shell_desc[written].icon_path =
            (apps[i].icon_path[0] != '\0') ? apps[i].icon_path : NULL;
        s_shell_desc[written].so_path = s_shell_so_path[written];
        s_shell_desc[written].entry_symbol = apps[i].entry_symbol;

        out[written] = s_shell_desc[written];
        written++;
    }

    return written;
}
