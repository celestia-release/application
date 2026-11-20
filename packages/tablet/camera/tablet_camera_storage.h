// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Local SD album paths for Nova camera (sibling of /mnt/install).
 */
#ifndef APPLICATION_TABLET_CAMERA_STORAGE_H
#define APPLICATION_TABLET_CAMERA_STORAGE_H

#include <stddef.h>

#define TABLET_ALBUM_ROOT               "/mnt/album"
#define TABLET_ALBUM_INSTALL_MARKER     "/mnt/install"
#define TABLET_PHOTO_SUBDIR             "Pictures"
#define TABLET_RECORD_SUBDIR            "Record"
#define TABLET_PHOTO_PREFIX             "IMG_"
#define TABLET_VIDEO_PREFIX             "VID_"
#define TABLET_ALBUM_ERR_NOT_READY      (-2)

int tablet_album_is_ready(void);
int tablet_album_ensure_subdir(const char *subdir, char *out_path, size_t out_len);
void tablet_album_stamp_path(char *buf, size_t buf_size, const char *dir,
                             const char *prefix, const char *ext);
void tablet_album_show_missing_hint(void);

#endif /* APPLICATION_TABLET_CAMERA_STORAGE_H */
