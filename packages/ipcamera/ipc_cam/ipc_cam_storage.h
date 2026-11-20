// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card storage helpers for ipcamera: mount detection, capacity query,
 * and loop-recording cleanup (delete oldest segments when space runs low).
 */
#ifndef IPCAMERA_STORAGE_H
#define IPCAMERA_STORAGE_H

#include <stddef.h>

#define IPC_CAM_MOUNT_DEFAULT   "/mnt/tfcard"
#define IPC_CAM_RECORD_SUBDIR          "Ipcam"

int ipc_cam_storage_init(void);
void ipc_cam_storage_deinit(void);

/** @return 1 when the TF card is mounted and writable, 0 otherwise. */
int ipc_cam_storage_is_ready(void);

/**
 * @brief Resolve (and create) the record directory on the TF card.
 * @return 0 on success, negative on failure.
 */
int ipc_cam_storage_get_dir(char *out_path, size_t out_len);

/** @return free space of the TF card mount in MiB, or negative on error. */
long ipc_cam_storage_get_free_mb(void);

/**
 * @brief Ensure at least need_mb free by deleting oldest *.mp4 segments.
 * @return 0 when enough space is available (or freed), negative otherwise.
 */
int ipc_cam_storage_ensure_free(long need_mb);

#endif /* IPCAMERA_STORAGE_H */
