// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card storage helpers for ipcamera.
 */
#define LOG_TAG "IPCAMERA_STORAGE"

#include "ipc_cam_storage.h"

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

static int ipc_cam_path_usable(const char *mount, int need_write)
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

int ipc_cam_storage_init(void)
{
    if (s_inited)
        return 0;

    if (storage_open(&s_storage) != 0 || s_storage == NULL || s_storage->tfcard == NULL) {
        log_error("storage_open failed");
        s_storage = NULL;
        return -1;
    }

    if (s_storage->tfcard->init(IPC_CAM_MOUNT_DEFAULT) != 0)
        log_warn("tfcard init returned error (card may be absent)");

    s_inited = 1;
    return 0;
}

void ipc_cam_storage_deinit(void)
{
    /* storage singleton is shared with other apps; keep it open. Only drop ref. */
    s_storage = NULL;
    s_inited = 0;
}

static const char *ipc_cam_storage_get_mount(void)
{
    const char *dir;

    if (ipc_cam_storage_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL &&
        s_storage->tfcard->get_mounted_dir != NULL) {
        dir = s_storage->tfcard->get_mounted_dir();
        if (dir != NULL && dir[0] != '\0')
            return dir;
    }

    if (ipc_cam_path_usable(IPC_CAM_MOUNT_DEFAULT, 0))
        return IPC_CAM_MOUNT_DEFAULT;

    return NULL;
}

int ipc_cam_storage_is_ready(void)
{
    if (ipc_cam_storage_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL)
        return 1;

    return ipc_cam_path_usable(IPC_CAM_MOUNT_DEFAULT, 1);
}

int ipc_cam_storage_get_dir(char *out_path, size_t out_len)
{
    const char *mount;

    if (!out_path || out_len < 2)
        return -1;

    mount = ipc_cam_storage_get_mount();
    if (mount == NULL)
        return -1;

    if (snprintf(out_path, out_len, "%s/%s", mount, IPC_CAM_RECORD_SUBDIR) >= (int)out_len)
        return -1;

    if (mkdir(out_path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

long ipc_cam_storage_get_free_mb(void)
{
    const char *mount = ipc_cam_storage_get_mount();
    struct statvfs st;
    unsigned long long free_bytes;

    if (mount == NULL || statvfs(mount, &st) != 0)
        return -1;

    free_bytes = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    return (long)(free_bytes / (1024ULL * 1024ULL));
}

static int ipc_cam_find_oldest(const char *dir, char *out_path, size_t out_len)
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

int ipc_cam_storage_ensure_free(long need_mb)
{
    char dir[300];
    int guard;

    if (need_mb <= 0)
        return 0;

    if (ipc_cam_storage_get_dir(dir, sizeof(dir)) != 0)
        return -1;

    /* Bounded loop: never delete more than a sane number of files per call. */
    for (guard = 0; guard < 4096; guard++) {
        long free_mb = ipc_cam_storage_get_free_mb();
        char oldest[512];

        if (free_mb < 0)
            return -1;
        if (free_mb >= need_mb)
            return 0;

        if (ipc_cam_find_oldest(dir, oldest, sizeof(oldest)) != 0) {
            /* Caller rate-limits; keep this quiet to avoid flood. */
            return -1;
        }

        if (unlink(oldest) != 0) {
            log_warn("loop-record: unlink failed: %s", oldest);
            return -1;
        }
        log_info("loop-record: deleted oldest segment %s (free was %ldMB)", oldest, free_mb);
    }

    /* Guard loop exhausted without reaching need_mb: space is NOT guaranteed. */
    log_warn("loop-record: still short of %ldMB free after 4096 deletions", need_mb);
    return -1;
}
