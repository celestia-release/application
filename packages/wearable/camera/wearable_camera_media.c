// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 */
#define LOG_TAG "WEARABLE_CAMERA_MEDIA"

#include "wearable_camera_media.h"

#include "log.h"

#include <unistd.h>

int wearable_camera_media_delete_file(const char *path)
{
    if (!path)
        return -1;

    if (unlink(path) == 0)
        return 0;

    log_warn("delete failed: %s", path);
    return -1;
}
