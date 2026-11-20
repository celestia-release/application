#define LOG_TAG "TABLET_APP_REGISTRY"

#include "tablet_app_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "log.h"
#include "app_globle_config.h"
#include "cJSON.h"
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
#ifndef APPS_BASE_DIR
#define APPS_BASE_DIR "/usr/apps"
#endif
#define TABLET_APPS_BASE_DIR APPS_BASE_DIR
#define TABLET_APPS_BASE_DIR_FALLBACK "/usr/apps"
#define TABLET_APP_MAX_DYNAMIC 64

static int s_loaded = 0;
static int s_count = 0;
static tablet_app_desc_t s_desc[TABLET_APP_MAX_DYNAMIC];

static int file_exists(const char *path)
{
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static void json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0)
        return;
    out[0] = '\0';
    const char *p = strstr(json, key);
    if (!p)
        return;
    p = strchr(p, ':');
    if (!p)
        return;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return;
    p++;
    const char *q = strchr(p, '"');
    if (!q)
        return;
    size_t n = (size_t)(q - p);
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static const char *tablet_apps_root_dir(void);

static void load_app_manifest(tablet_app_desc_t *app)
{
    char path[256] = {0};
    if (app->install_path[0] != '\0')
        snprintf(path, sizeof(path), "%s/app.json", app->install_path);
    else
        snprintf(path, sizeof(path), "%s/%s/app.json", tablet_apps_root_dir(), app->app_id);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    long size = ftell(fp);
    if (size <= 0 || size > (512 * 1024)) {
        fclose(fp);
        return;
    }
    rewind(fp);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';

    char name[64] = {0};
    char action[64] = {0};
    char entry_so[256] = {0};
    char entry_symbol[64] = {0};
    char exec[256] = {0};
    json_get_string(buf, "\"name\"", name, sizeof(name));
    json_get_string(buf, "\"action\"", action, sizeof(action));
    json_get_string(buf, "\"entry_so\"", entry_so, sizeof(entry_so));
    json_get_string(buf, "\"entry_symbol\"", entry_symbol, sizeof(entry_symbol));
    json_get_string(buf, "\"exec\"", exec, sizeof(exec));
    json_get_string(buf, "\"icon_kind\"", app->icon_kind, sizeof(app->icon_kind));
    json_get_string(buf, "\"icon_glyph\"", app->icon_glyph, sizeof(app->icon_glyph));
    json_get_string(buf, "\"icon_color\"", app->icon_color, sizeof(app->icon_color));
    json_get_string(buf, "\"icon_color2\"", app->icon_color2, sizeof(app->icon_color2));
    if (name[0] != '\0')
        snprintf(app->display_name, sizeof(app->display_name), "%s", name);
    if (action[0] != '\0')
        snprintf(app->action, sizeof(app->action), "%s", action);
    if (entry_so[0] != '\0')
        snprintf(app->entry_so, sizeof(app->entry_so), "%s", entry_so);
    if (entry_symbol[0] != '\0')
        snprintf(app->entry_symbol, sizeof(app->entry_symbol), "%s", entry_symbol);
    if (exec[0] != '\0')
        snprintf(app->exec, sizeof(app->exec), "%s", exec);

    /* 进程型应用（exec）不走 dlopen 入口，缺 entry 字段属正常 */
    if (app->exec[0] == '\0' &&
        (app->entry_so[0] == '\0' || app->entry_symbol[0] == '\0'))
        log_warn("app.json missing entry_so or entry_symbol: app_id=%s install=%s",
                 app->app_id, app->install_path[0] ? app->install_path : "(unset)");

    free(buf);
}

static void reset_registry(void)
{
    memset(s_desc, 0, sizeof(s_desc));
    s_count = 0;
}

static const char *tablet_apps_root_dir(void)
{
    DIR *d = opendir(TABLET_APPS_BASE_DIR);
    if (d) {
        closedir(d);
        return TABLET_APPS_BASE_DIR;
    }
    return TABLET_APPS_BASE_DIR_FALLBACK;
}

static int tablet_app_registry_find_by_id(const char *app_id)
{
    int i;

    if (!app_id || !app_id[0])
        return -1;
    for (i = 0; i < s_count; i++) {
        if (strcmp(s_desc[i].app_id, app_id) == 0)
            return i;
    }
    return -1;
}

/* Append apps under root that have app.json and are not already registered.
 * Returns number of apps added. Does not reset the registry. */
static int tablet_app_registry_scan_dir(const char *root)
{
    DIR *dir;
    struct dirent *ent;
    int added = 0;

    if (!root || !root[0])
        return 0;
    dir = opendir(root);
    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL) {
        char app_json[256] = {0};
        tablet_app_desc_t *app;

        if (ent->d_name[0] == '.')
            continue;
        {
            size_t name_len = strlen(ent->d_name);
            if (name_len >= 4 && strcmp(ent->d_name + name_len - 4, ".bak") == 0)
                continue;
        }
        if (s_count >= TABLET_APP_MAX_DYNAMIC)
            break;
        if (tablet_app_registry_find_by_id(ent->d_name) >= 0)
            continue;
        snprintf(app_json, sizeof(app_json), "%s/%s/app.json", root, ent->d_name);
        if (!file_exists(app_json))
            continue;

        app = &s_desc[s_count];
        memset(app, 0, sizeof(*app));
        app->index = s_count;
        snprintf(app->app_id, sizeof(app->app_id), "%s", ent->d_name);
        snprintf(app->display_name, sizeof(app->display_name), "%s", ent->d_name);
        snprintf(app->install_path, sizeof(app->install_path), "%s/%s", root, ent->d_name);
        snprintf(app->action, sizeof(app->action), "%s", ent->d_name);
        app->enabled = 1;
        load_app_manifest(app);
        s_count++;
        added++;
    }
    closedir(dir);
    return added;
}

static int tablet_app_registry_scan_all_dirs(void)
{
    int added = 0;

    added += tablet_app_registry_scan_dir(TABLET_APPS_BASE_DIR);
    if (strcmp(TABLET_APPS_BASE_DIR_FALLBACK, TABLET_APPS_BASE_DIR) != 0)
        added += tablet_app_registry_scan_dir(TABLET_APPS_BASE_DIR_FALLBACK);
#ifdef APPS_SD_BASE_DIR
    if (strcmp(APPS_SD_BASE_DIR, TABLET_APPS_BASE_DIR) != 0 &&
        strcmp(APPS_SD_BASE_DIR, TABLET_APPS_BASE_DIR_FALLBACK) != 0)
        added += tablet_app_registry_scan_dir(APPS_SD_BASE_DIR);
#endif
    return added;
}

void tablet_app_registry_scan_installed(void)
{
    reset_registry();
    (void)tablet_app_registry_scan_all_dirs();
    s_loaded = 1;
}

void tablet_app_registry_refresh(void)
{
    const char *paths[] = {
        APP_SETTINGS_CONFIG_PATH,
        APP_RUNTIME_CONFIG_PATH,
        APP_USER_CONFIG_PATH,
        INSTALLED_APPS_LEGACY_PATH
    };
    int pi;

    for (pi = 0; pi < 4; pi++) {
        FILE *fp = fopen(paths[pi], "rb");
        char *buf;
        long size;
        size_t n;
        cJSON *root;
        cJSON *installed;
        cJSON *apps;
        cJSON *item;
        int added;

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

        installed = cJSON_GetObjectItem(root, "installed_apps");
        if (cJSON_IsObject(installed))
            apps = cJSON_GetObjectItem(installed, "apps");
        else
            apps = cJSON_GetObjectItem(root, "apps");

        if (!cJSON_IsArray(apps)) {
            cJSON_Delete(root);
            continue;
        }

        reset_registry();
        cJSON_ArrayForEach(item, apps) {
            cJSON *id;
            cJSON *path;
            cJSON *en;
            tablet_app_desc_t *app;
            char app_json[288] = {0};

            if (!cJSON_IsObject(item) || s_count >= TABLET_APP_MAX_DYNAMIC)
                continue;
            id = cJSON_GetObjectItem(item, "app_id");
            if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0])
                continue;
            /* Push backups used to sit beside the app as "<id>.bak" and were scanned as a second app. */
            {
                size_t id_len = strlen(id->valuestring);
                if (id_len >= 4 && strcmp(id->valuestring + id_len - 4, ".bak") == 0)
                    continue;
            }

            app = &s_desc[s_count];
            memset(app, 0, sizeof(*app));
            app->index = s_count;
            snprintf(app->app_id, sizeof(app->app_id), "%s", id->valuestring);
            snprintf(app->display_name, sizeof(app->display_name), "%s", id->valuestring);
            snprintf(app->action, sizeof(app->action), "%s", id->valuestring);
            /* settings.json carries the name synced from each app's app.json;
             * load_app_manifest below still wins when the package ships one. */
            {
                cJSON *nm = cJSON_GetObjectItem(item, "name");
                if (cJSON_IsString(nm) && nm->valuestring && nm->valuestring[0])
                    snprintf(app->display_name, sizeof(app->display_name), "%s", nm->valuestring);
            }

            path = cJSON_GetObjectItem(item, "install_path");
            if (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) {
                snprintf(app->install_path, sizeof(app->install_path), "%s", path->valuestring);
            } else {
                snprintf(app->install_path, sizeof(app->install_path), "%s/%s",
                         tablet_apps_root_dir(), app->app_id);
            }

            snprintf(app_json, sizeof(app_json), "%s/app.json", app->install_path);
            if (!file_exists(app_json) &&
                strncmp(app->install_path, "/mnt/", 5) == 0) {
                continue;
            }

            en = cJSON_GetObjectItem(item, "enabled");
            app->enabled = cJSON_IsFalse(en) ? 0 : 1;
            load_app_manifest(app);
            s_count++;
        }
        s_loaded = 1;
        cJSON_Delete(root);

        /* Merge apps newly dropped under flash/SD install dirs (e.g. USB MSC). */
        added = tablet_app_registry_scan_all_dirs();
        if (added > 0) {
            if (tablet_app_registry_save() == 0)
                log_info("merged %d newly scanned app(s) into settings.json", added);
            else
                log_warn("merged %d app(s) in memory but failed to save settings.json", added);
        }
        if (s_count > 0)
            return;
    }

    tablet_app_registry_scan_installed();
    tablet_app_registry_save();
    log_warn("installed apps file not found, generated from app directory scan");
}

int tablet_app_registry_set_enabled_by_index(int index, int enabled)
{
    if (index < 0 || index >= s_count)
        return -1;
    s_desc[index].enabled = enabled ? 1 : 0;
    return 0;
}

int tablet_app_registry_save(void)
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
    const char *out_path = APP_SETTINGS_CONFIG_PATH;

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
        log_error("create apps config root failed");
        return -1;
    }

    cJSON_DeleteItemFromObject(root, "installed_apps");
    installed = cJSON_CreateObject();
    cJSON_AddNumberToObject(installed, "schema_version", 1);
    apps = cJSON_CreateArray();
    for (i = 0; i < s_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "app_id", s_desc[i].app_id);
        /* Keep the display name (synced from the installed app's app.json) in
         * settings.json so the Settings page shows it even when a package is
         * temporarily unreadable. */
        if (s_desc[i].display_name[0] && strcmp(s_desc[i].display_name, s_desc[i].app_id) != 0)
            cJSON_AddStringToObject(item, "name", s_desc[i].display_name);
        cJSON_AddStringToObject(item, "install_path",
                                s_desc[i].install_path[0] ? s_desc[i].install_path
                                                          : TABLET_APPS_BASE_DIR);
        cJSON_AddBoolToObject(item, "enabled", s_desc[i].enabled ? 1 : 0);
        cJSON_AddItemToArray(apps, item);
    }
    cJSON_AddItemToObject(installed, "apps", apps);
    cJSON_AddItemToObject(root, "installed_apps", installed);

    printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        log_error("print apps config failed");
        return -1;
    }

    fp = fopen(out_path, "wb");
    if (!fp) {
        log_error("open %s failed", out_path);
        free(printed);
        return -1;
    }
    fputs(printed, fp);
    fclose(fp);
    free(printed);
    return 0;
}

const tablet_app_desc_t *tablet_app_registry_list(int *count)
{
    if (!s_loaded)
        tablet_app_registry_refresh();
    if (count)
        *count = s_count;
    for (int i = 0; i < s_count; i++)
        load_app_manifest(&s_desc[i]);
    return s_desc;
}
