// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card storage helpers for Luna dashcam.
 */
#define LOG_TAG "WEARABLE_DASHCAM_STORAGE"

#include "dashcam_storage.h"

#include "storage.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static storage_t *s_storage;
static int s_inited;

/* Must match dashcam_storage_dir_e order.
 * LoopVideo=循环视频, Photo=拍照文件, AlarmEvent=告警事件, ParkingEvent=停车监控事件 */
static const char *const s_dashcam_subdirs[DASHCAM_DIR_COUNT] = {
    "LoopVideo",
    "Photo",
    "AlarmEvent",
    "ParkingEvent",
};

static int dashcam_path_usable(const char *mount, int need_write)
{
    struct statvfs st;

    if (!mount || mount[0] == '\0')
        return 0;
    if (statvfs(mount, &st) != 0 || st.f_blocks == 0)
        return 0;
    if (need_write && access(mount, W_OK) != 0)
        return 0;
    return 1;
}

static int dashcam_mkdir_p(const char *path)
{
    if (!path || path[0] == '\0')
        return -1;
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    log_warn("dashcam mkdir failed: %s (%d)", path, errno);
    return -1;
}

static const char *dashcam_storage_get_mount(void)
{
    const char *dir;

    if (dashcam_storage_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL &&
        s_storage->tfcard->get_mounted_dir != NULL) {
        dir = s_storage->tfcard->get_mounted_dir();
        if (dir != NULL && dir[0] != '\0')
            return dir;
    }

    if (dashcam_path_usable(DASHCAM_MOUNT_DEFAULT, 0))
        return DASHCAM_MOUNT_DEFAULT;

    return NULL;
}

int dashcam_storage_ensure_dirs(void)
{
    const char *mount;
    char root[300];
    char path[360];
    int i;
    int failed = 0;

    mount = dashcam_storage_get_mount();
    if (mount == NULL)
        return -1;

    if (snprintf(root, sizeof(root), "%s/%s", mount, DASHCAM_ROOT_NAME) >= (int)sizeof(root))
        return -1;
    if (dashcam_mkdir_p(root) != 0)
        return -1;

    for (i = 0; i < (int)DASHCAM_DIR_COUNT; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", root, s_dashcam_subdirs[i]) >=
            (int)sizeof(path)) {
            failed = 1;
            continue;
        }
        if (dashcam_mkdir_p(path) != 0)
            failed = 1;
    }

    if (failed) {
        log_warn("dashcam: some subdirs under %s could not be created", root);
        return -1;
    }

    log_info("dashcam dirs ready under %s", root);
    return 0;
}

int dashcam_storage_init(void)
{
    if (s_inited)
        return 0;

    if (storage_open(&s_storage) != 0 || s_storage == NULL || s_storage->tfcard == NULL) {
        log_error("storage_open failed");
        s_storage = NULL;
        return -1;
    }

    if (s_storage->tfcard->init(DASHCAM_MOUNT_DEFAULT) != 0)
        log_warn("tfcard init returned error (card may be absent)");

    s_inited = 1;

    /* Create Dashcam/ + subdirs when the card is already mounted. */
    if (dashcam_storage_is_ready()) {
        if (dashcam_storage_ensure_dirs() != 0)
            log_warn("dashcam: ensure_dirs failed at init (will retry on record)");
    }

    return 0;
}

void dashcam_storage_deinit(void)
{
    /* storage singleton is shared with other apps; keep it open. Only drop ref. */
    s_storage = NULL;
    s_inited = 0;
}

int dashcam_storage_is_ready(void)
{
    if (s_inited && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL)
        return 1;

    /* Avoid recurse through init when called from init itself. */
    if (!s_inited) {
        if (dashcam_storage_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
            s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL)
            return 1;
    }

    return dashcam_path_usable(DASHCAM_MOUNT_DEFAULT, 1);
}

int dashcam_storage_get_subdir(dashcam_storage_dir_e which,
                               char *out_path, size_t out_len)
{
    const char *mount;
    char root[300];

    if (!out_path || out_len < 2)
        return -1;
    if (which < 0 || which >= DASHCAM_DIR_COUNT)
        return -1;

    mount = dashcam_storage_get_mount();
    if (mount == NULL)
        return -1;

    if (snprintf(root, sizeof(root), "%s/%s", mount, DASHCAM_ROOT_NAME) >= (int)sizeof(root))
        return -1;
    if (dashcam_mkdir_p(root) != 0)
        return -1;

    if (snprintf(out_path, out_len, "%s/%s", root, s_dashcam_subdirs[which]) >= (int)out_len)
        return -1;
    if (dashcam_mkdir_p(out_path) != 0)
        return -1;

    return 0;
}

int dashcam_storage_get_dir(char *out_path, size_t out_len)
{
    /* Ensure full Dashcam tree exists; recorder uses LoopVideo. */
    (void)dashcam_storage_ensure_dirs();
    return dashcam_storage_get_subdir(DASHCAM_DIR_LOOP_VIDEO, out_path, out_len);
}

long dashcam_storage_get_free_mb(void)
{
    const char *mount = dashcam_storage_get_mount();
    struct statvfs st;
    unsigned long long free_bytes;

    if (mount == NULL || statvfs(mount, &st) != 0)
        return -1;

    free_bytes = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    return (long)(free_bytes / (1024ULL * 1024ULL));
}

static int dashcam_find_oldest(const char *dir, char *out_path, size_t out_len)
{
    DIR *dp;
    struct dirent *ent;
    time_t oldest = 0;
    int found = 0;

    dp = opendir(dir);
    if (!dp)
        return -1;

    out_path[0] = '\0';
    while ((ent = readdir(dp)) != NULL) {
        char path[512];
        struct stat st;
        size_t len;

        if (ent->d_name[0] == '.')
            continue;
        len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 4, ".mp4") != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (!found || st.st_mtime < oldest) {
            oldest = st.st_mtime;
            snprintf(out_path, out_len, "%s", path);
            found = 1;
        }
    }

    closedir(dp);
    return found ? 0 : -1;
}

int dashcam_storage_ensure_free(long need_mb)
{
    char dir[360];
    int guard;

    if (need_mb <= 0)
        return 0;

    /* Only reclaim space from loop-video segments. */
    if (dashcam_storage_get_subdir(DASHCAM_DIR_LOOP_VIDEO, dir, sizeof(dir)) != 0)
        return -1;

    for (guard = 0; guard < 4096; guard++) {
        long free_mb = dashcam_storage_get_free_mb();
        char oldest[512];

        if (free_mb < 0)
            return -1;
        if (free_mb >= need_mb)
            return 0;

        if (dashcam_find_oldest(dir, oldest, sizeof(oldest)) != 0) {
            log_warn("loop-record: no segment to delete but free=%ldMB < need=%ldMB",
                     free_mb, need_mb);
            return -1;
        }

        if (unlink(oldest) != 0) {
            log_warn("loop-record: unlink failed: %s", oldest);
            return -1;
        }
        log_info("loop-record: deleted oldest segment %s (free was %ldMB)", oldest, free_mb);
    }

    log_warn("loop-record: still short of %ldMB free after 4096 deletions", need_mb);
    return -1;
}
