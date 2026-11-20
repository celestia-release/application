// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * TF card storage helpers for Luna dashcam: mount detection, Dashcam tree
 * layout, capacity query, and loop-recording cleanup.
 *
 * Layout on TF card:
 *   <mount>/Dashcam/
 *     LoopVideo/      — 循环视频 (current recorder output)
 *     Photo/          — 拍照文件 (reserved)
 *     AlarmEvent/     — 告警事件 (reserved)
 *     ParkingEvent/   — 停车监控事件 (reserved)
 */
#ifndef WEARABLE_DASHCAM_STORAGE_H
#define WEARABLE_DASHCAM_STORAGE_H

#include <stddef.h>

#define DASHCAM_MOUNT_DEFAULT   "/mnt/tfcard"
#define DASHCAM_ROOT_NAME       "Dashcam"

/** Subdirectories under Dashcam/ (English names; Chinese meaning in comments). */
typedef enum {
    DASHCAM_DIR_LOOP_VIDEO = 0, /**< LoopVideo — 循环视频 */
    DASHCAM_DIR_PHOTO,          /**< Photo — 拍照文件 */
    DASHCAM_DIR_ALARM,          /**< AlarmEvent — 告警事件 */
    DASHCAM_DIR_PARKING,        /**< ParkingEvent — 停车监控事件 */
    DASHCAM_DIR_COUNT
} dashcam_storage_dir_e;

int dashcam_storage_init(void);
void dashcam_storage_deinit(void);

/** @return 1 when the TF card is mounted and writable, 0 otherwise. */
int dashcam_storage_is_ready(void);

/**
 * @brief Ensure Dashcam/ and all standard subdirs exist on the TF card.
 * @return 0 on success, negative when the card is absent or mkdir fails.
 */
int dashcam_storage_ensure_dirs(void);

/**
 * @brief Resolve (and create) one Dashcam subdirectory.
 * @param which Subdir enum (loop / photo / alarm / parking).
 * @return 0 on success, negative on failure.
 */
int dashcam_storage_get_subdir(dashcam_storage_dir_e which,
                               char *out_path, size_t out_len);

/**
 * @brief Resolve the loop-video directory (Dashcam/LoopVideo).
 * @details Used by the current recorder; equivalent to
 *          dashcam_storage_get_subdir(DASHCAM_DIR_LOOP_VIDEO, ...).
 */
int dashcam_storage_get_dir(char *out_path, size_t out_len);

/** @return free space of the TF card mount in MiB, or negative on error. */
long dashcam_storage_get_free_mb(void);

/**
 * @brief Ensure at least need_mb free by deleting oldest *.mp4 in LoopVideo.
 * @return 0 when enough space is available (or freed), negative otherwise.
 */
int dashcam_storage_ensure_free(long need_mb);

#endif /* WEARABLE_DASHCAM_STORAGE_H */
