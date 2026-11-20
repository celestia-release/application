// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card helpers for Luna camera.
 */
#define LOG_TAG "WEARABLE_CAMERA_TFCARD"

#include "wearable_camera_tfcard.h"

#include "gui_common.h"
#include "storage.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static storage_t *s_storage;
static int s_inited;

static int wearable_tfcard_path_usable(const char *mount, int need_write)
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

int wearable_tfcard_ensure_init(void)
{
    if (s_inited)
        return 0;

    if (storage_open(&s_storage) != 0 || s_storage == NULL || s_storage->tfcard == NULL) {
        log_error("storage_open failed");
        return -1;
    }

    if (s_storage->tfcard->init(WEARABLE_TFCARD_MOUNT_DEFAULT) != 0) {
        log_warn("tfcard init returned error (card may be absent)");
    }

    s_inited = 1;
    return 0;
}

int wearable_tfcard_is_ready(void)
{
    if (wearable_tfcard_ensure_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL)
        return 1;

    /* Manual mount or mmcblk0 path: honor an already-mounted /mnt/tfcard. */
    return wearable_tfcard_path_usable(WEARABLE_TFCARD_MOUNT_DEFAULT, 1);
}

const char *wearable_tfcard_get_mount(void)
{
    const char *dir;

    if (wearable_tfcard_ensure_init() == 0 && s_storage != NULL && s_storage->tfcard != NULL &&
        s_storage->tfcard->get_status() == STORAGE_TFCARD_STATUS_NORMAL &&
        s_storage->tfcard->get_mounted_dir != NULL) {
        dir = s_storage->tfcard->get_mounted_dir();
        if (dir != NULL && dir[0] != '\0')
            return dir;
    }

    if (wearable_tfcard_path_usable(WEARABLE_TFCARD_MOUNT_DEFAULT, 0))
        return WEARABLE_TFCARD_MOUNT_DEFAULT;

    return NULL;
}

int wearable_tfcard_build_subdir(const char *subdir, char *out_path, size_t out_len)
{
    const char *mount;

    if (!subdir || !out_path || out_len < 2)
        return -1;

    mount = wearable_tfcard_get_mount();
    if (mount == NULL)
        return -1;

    if (snprintf(out_path, out_len, "%s/%s", mount, subdir) >= (int)out_len)
        return -1;

    if (mkdir(out_path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

void wearable_tfcard_show_missing_hint(void)
{
    if (get_lan_index() == 1)
        show_msgbox_note("请插入 T 卡");
    else
        show_msgbox_note("Please insert T card");
}
