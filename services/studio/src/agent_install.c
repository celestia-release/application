/*
 * agent_install.c - APP_PUSH receiver for studio
 *
 * Payload (see studio_internal.h): [4B jsonlen][4B datalen][manifest][tar.gz]
 * manifest: {"type":"app"|"so", "app_id":"tablet", "version":"1.2.0", "md5":"..."}
 *
 * Install strategy (docs/design/nova/anypi_nova_pc_studio.md §6.2):
 *   app -> extract to /tmp, mv old /usr/apps/<id> to <id>.bak, mv new in,
 *          force exec bits (squashfs lesson: missing x shows as file missing),
 *          rollback to .bak on any failure after backup.
 *   so  -> copy files into /usr/lib.
 * tar/gzip integrity (CRC) is the transport check; md5sum is used only when
 * the manifest carries one and the busybox applet exists.
 */
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "studio_internal.h"

/*********************
 *      DEFINES
 *********************/
#define SA_WORK_DIR    "/tmp/studio"
#define SA_PUSH_TGZ    SA_WORK_DIR "/push.tar.gz"
#define SA_UNPACK_DIR  SA_WORK_DIR "/pkg"
#define SA_LOG_CAP     (4u * 1024u)
#define SA_SETTINGS    "/etc/config/settings.json"
#define SA_APP_FALLBACK SA_INSTALL_DIR_APP /* used when settings.json has no entry */

/*********************
 *   STATIC PROTOTYPES
 **********************/
static int run_capture(const char *cmd, char *out, size_t cap);
static bool resolve_app_path(const char *app_id, char *out, size_t cap);

/*********************
 *      FUNCTIONS
 **********************/

/*
 * Install path comes from the device's own settings.json installed_apps
 * table (SSOT): nova ships most apps under /mnt/install/apps/<id> but
 * some (setting) under /usr/apps/<id>. Fallback /usr/apps/<id>.
 */
static bool resolve_app_path(const char *app_id, char *out, size_t cap)
{
    bool ok = false;
    char txt[4096] = {0};
    FILE *fp = fopen(SA_SETTINGS, "r");
    if (fp)
    {
        size_t got = fread(txt, 1, sizeof(txt) - 1, fp);
        fclose(fp);
        txt[got] = 0;
    }
    cJSON *root = cJSON_Parse(txt);
    cJSON *table = root ? cJSON_GetObjectItem(root, "installed_apps") : NULL;
    cJSON *apps = table ? cJSON_GetObjectItem(table, "apps") : NULL;
    if (cJSON_IsArray(apps))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, apps)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "app_id"));
            const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(item, "install_path"));
            if (id && path && strcmp(id, app_id) == 0 && path[0] == '/')
            {
                snprintf(out, cap, "%s", path);
                ok = true;
                break;
            }
        }
    }
    cJSON_Delete(root);
    if (!ok)
        snprintf(out, cap, SA_APP_FALLBACK "/%s", app_id);
    return ok;
}

/* run shell cmd, append combined output to log (cap-bounded) */
static int run_capture(const char *cmd, char *log, size_t cap)
{
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        if (log && cap > 1)
        {
            size_t room = cap - strlen(log) - 1;
            if (n > room)
                n = room;
            strncat(log, buf, n);
        }
    }
    int rc = pclose(fp);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* keep the result log inside its buffer */
static void log_append(char *log, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void log_append(char *log, size_t cap, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (cap > strlen(log) + strlen(line) + 2)
    {
        strcat(log, line);
        strcat(log, "\n");
    }
}

/* app_id must be [a-z0-9_]+ so it can never escape /usr/apps */
static bool valid_app_id(const char *id)
{
    if (!id || !id[0] || strlen(id) > 31)
        return false;
    regex_t re;
    if (regcomp(&re, "^[a-z0-9_]+$", REG_NOSUB) != 0)
        return false;
    bool ok = regexec(&re, id, 0, NULL, 0) == 0;
    regfree(&re);
    return ok;
}

/* verify tar.gz md5 when manifest carries one and md5sum exists; 1=ok/-1=skip/0=mismatch */
static int check_md5(const char *md5, char *log, size_t cap)
{
    if (!md5 || !md5[0])
        return -1;
    char cmd[SA_LOG_CAP / 2];
    char out[128];
    snprintf(cmd, sizeof(cmd), "md5sum %s 2>/dev/null", SA_PUSH_TGZ);
    if (run_capture(cmd, out, sizeof(out)) != 0 || !out[0])
    {
        log_append(log, cap, "md5sum unavailable, skip verify");
        return -1;
    }
    if (strncmp(out, md5, 32) != 0)
    {
        log_append(log, cap, "MD5 MISMATCH: got %.32s want %.32s", out, md5);
        return 0;
    }
    return 1;
}

cJSON *sa_app_install(const char *manifest_json, const uint8_t *tgz, uint32_t tlen)
{
    char *log = calloc(1, SA_LOG_CAP);
    cJSON *result = cJSON_CreateObject();

    if (!log || !result)
    {
        free(log);
        cJSON_Delete(result);
        return NULL;
    }

    const char *type = NULL, *app_id = NULL, *version = NULL, *md5 = NULL;
    char target[128] = {0};
    bool done = false, rolled_back = false;

    cJSON *manifest = cJSON_Parse(manifest_json);
    if (!manifest)
    {
        log_append(log, SA_LOG_CAP, "manifest json parse failed");
        goto out;
    }
    type = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "type"));
    app_id = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "app_id"));
    version = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "version"));
    md5 = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "md5"));
    if (!type)
        type = "app";
    if (!version)
        version = "?";

    if (strcmp(type, "app") == 0 && !valid_app_id(app_id))
    {
        log_append(log, SA_LOG_CAP, "invalid app_id in manifest");
        goto out;
    }

    /* 1. stage payload to /tmp */
    run_capture("mkdir -p " SA_WORK_DIR " && rm -rf " SA_UNPACK_DIR, log, SA_LOG_CAP);
    int fd = open(SA_PUSH_TGZ, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        log_append(log, SA_LOG_CAP, "write %s failed: %s", SA_PUSH_TGZ, strerror(errno));
        goto out;
    }
    ssize_t wrote = write(fd, tgz, tlen);
    close(fd);
    if (wrote != (ssize_t)tlen)
    {
        log_append(log, SA_LOG_CAP, "payload write short (%zd/%u)", wrote, tlen);
        goto out;
    }
    log_append(log, SA_LOG_CAP, "payload %u bytes staged", tlen);

    if (check_md5(md5, log, SA_LOG_CAP) == 0)
        goto out;

    /* 2. unpack into pkg_tmp, then swap into place; tar gzip CRC catches transport corruption */
    run_capture("mkdir -p " SA_WORK_DIR "/pkg_tmp", NULL, 0);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -C " SA_WORK_DIR "/pkg_tmp 2>&1", SA_PUSH_TGZ);
    if (run_capture(cmd, log, SA_LOG_CAP) != 0)
    {
        log_append(log, SA_LOG_CAP, "tar extract failed");
        goto out;
    }
    run_capture("rm -rf " SA_UNPACK_DIR " && mv " SA_WORK_DIR "/pkg_tmp " SA_UNPACK_DIR, NULL, 0);

    /* 3. figure the install target; app.json wins for app_id if present */
    if (strcmp(type, "app") == 0)
    {
        char txt[1024] = {0};
        FILE *fp = fopen(SA_UNPACK_DIR "/app.json", "r");
        if (fp)
        {
            size_t got = fread(txt, 1, sizeof(txt) - 1, fp);
            fclose(fp);
            txt[got] = 0;
        }
        cJSON *parsed = cJSON_Parse(txt);
        const char *real_id = parsed ? cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "app_id")) : NULL;
        if (real_id && valid_app_id(real_id))
            app_id = real_id;
        cJSON_Delete(parsed);
        resolve_app_path(app_id, target, sizeof(target));
        log_append(log, SA_LOG_CAP, "install path: %s", target);
    }
    else
        snprintf(target, sizeof(target), SA_INSTALL_DIR_LIB);

    /* 4. backup + swap (app) or plain copy (so) */
    if (strcmp(type, "app") == 0)
    {
        snprintf(cmd, sizeof(cmd), "rm -rf %s.bak 2>&1", target);
        run_capture(cmd, log, SA_LOG_CAP);
        snprintf(cmd, sizeof(cmd), "if [ -d %s ]; then mv %s %s.bak 2>&1; fi", target, target, target);
        run_capture(cmd, log, SA_LOG_CAP);

        snprintf(cmd, sizeof(cmd), "mv " SA_UNPACK_DIR " %s 2>&1", target);
        if (run_capture(cmd, log, SA_LOG_CAP) != 0)
        {
            /* roll the old version back; it is the only runnable copy now */
            snprintf(cmd, sizeof(cmd), "if [ -d %s.bak ]; then mv %s.bak %s 2>&1; fi", target, target, target);
            run_capture(cmd, NULL, 0);
            rolled_back = true;
            log_append(log, SA_LOG_CAP, "install failed, rolled back to previous version");
            goto out;
        }

        /* exec bits: dirs +rx, .so and scripts +x (squashfs exec-bit lesson) */
        snprintf(cmd, sizeof(cmd),
                 "chmod -R a+rX %s 2>&1; "
                 "find %s -type f \\( -name '*.so' -o -name '*.sh' \\) -exec chmod 755 {} + 2>&1",
                 target, target);
        run_capture(cmd, log, SA_LOG_CAP);
        log_append(log, SA_LOG_CAP, "installed app %s v%s -> %s", app_id, version, target);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "cp -af " SA_UNPACK_DIR "/. %s/ 2>&1", target);
        if (run_capture(cmd, log, SA_LOG_CAP) != 0)
        {
            log_append(log, SA_LOG_CAP, "copy to %s failed", target);
            goto out;
        }
        snprintf(cmd, sizeof(cmd), "find %s -maxdepth 1 -name '*.so*' -exec chmod 644 {} + 2>&1", target);
        run_capture(cmd, NULL, 0);
        log_append(log, SA_LOG_CAP, "installed so -> %s", target);
    }

    done = true;
    log_append(log, SA_LOG_CAP, "NOTE: takes effect after the owning process restarts "
                                "(app: launcher restart; so: reboot or ipc_main restart)");

out:
    cJSON_AddBoolToObject(result, "ok", done);
    if (app_id)
        cJSON_AddStringToObject(result, "app_id", app_id);
    if (version)
        cJSON_AddStringToObject(result, "version", version);
    cJSON_AddBoolToObject(result, "rolled_back", rolled_back);
    cJSON_AddStringToObject(result, "log", log);

    cJSON_Delete(manifest);
    free(log);
    return result;
}
