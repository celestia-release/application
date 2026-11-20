// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Nova camera album on onboard SD: /mnt/album (sibling of /mnt/install).
 */
#define LOG_TAG "TABLET_CAMERA_ALBUM"

#include "tablet_camera_storage.h"
#include "tablet_common.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mkdir_one(const char *path)
{
    if (!path || path[0] == '\0')
        return -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int tablet_album_is_ready(void)
{
    struct stat st;

    if (stat(TABLET_ALBUM_INSTALL_MARKER, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    if (access("/mnt", W_OK) != 0)
        return 0;
    return 1;
}

int tablet_album_ensure_subdir(const char *subdir, char *out_path, size_t out_len)
{
    if (!subdir || !out_path || out_len < 2)
        return -1;
    if (!tablet_album_is_ready())
        return -1;
    if (mkdir_one(TABLET_ALBUM_ROOT) != 0)
        return -1;
    if (snprintf(out_path, out_len, "%s/%s", TABLET_ALBUM_ROOT, subdir) >= (int)out_len)
        return -1;
    if (mkdir_one(out_path) != 0)
        return -1;
    return 0;
}

void tablet_album_stamp_path(char *buf, size_t buf_size, const char *dir,
                             const char *prefix, const char *ext)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (!buf || buf_size == 0)
        return;

    if (!tm) {
        snprintf(buf, buf_size, "%s/%s%s", dir ? dir : "", prefix ? prefix : "", ext ? ext : "");
        return;
    }

    snprintf(buf, buf_size, "%s/%s%04d%02d%02d_%02d%02d%02d%s",
             dir ? dir : "", prefix ? prefix : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, ext ? ext : "");
}

void tablet_album_show_missing_hint(void)
{
    show_toast_note(lan("Cannot save"), 2500, 1);
}
