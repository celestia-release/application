// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card helpers for Luna camera (photo/record require mounted T card).
 */
#ifndef WEARABLE_CAMERA_TFCARD_H
#define WEARABLE_CAMERA_TFCARD_H

#include <stddef.h>

#define WEARABLE_TFCARD_MOUNT_DEFAULT   "/mnt/tfcard"
#define WEARABLE_PHOTO_SUBDIR           "Pictures"
#define WEARABLE_RECORD_SUBDIR          "Record"
#define WEARABLE_RECORD_TMP_SUBDIR      ".rec_tmp"

/** Returned by capture/record when TF card is absent or not writable. */
#define WEARABLE_TFCARD_ERR_NOT_READY   (-2)

int wearable_tfcard_ensure_init(void);
int wearable_tfcard_is_ready(void);
const char *wearable_tfcard_get_mount(void);
int wearable_tfcard_build_subdir(const char *subdir, char *out_path, size_t out_len);
void wearable_tfcard_show_missing_hint(void);

#endif /* WEARABLE_CAMERA_TFCARD_H */
