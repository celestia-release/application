// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Loop recording for ipcamera: 3-minute MP4 segments on the main H.264
 * stream with automatic deletion of the oldest segment when space runs low.
 * A dedicated worker thread drives recorder start/stop so the LVGL thread never blocks.
 */
#ifndef IPCAMERA_RECORD_H
#define IPCAMERA_RECORD_H

typedef struct {
    int recording;          /**< 1 while a segment is being written */
    int card_missing;       /**< 1 when TF card is absent/not writable */
    int storage_full;       /**< 1 when the card is full and no segment could be freed */
    int paused;             /**< 1 when recording is manually stopped (app still open) */
    int segment_elapsed_s;  /**< elapsed seconds of the current segment */
    int session_elapsed_s;  /**< total recorded seconds this session */
    int segment_count;      /**< number of segments recorded this session */
    long free_mb;           /**< last known free space (MiB), or -1 */
} ipc_cam_record_status_t;

int ipc_cam_record_start(void);
void ipc_cam_record_stop(void);
int ipc_cam_record_is_active(void);
/** Manually pause/resume loop recording while the app stays open (0=record, 1=stop). */
void ipc_cam_record_set_paused(int paused);
int ipc_cam_record_is_paused(void);
void ipc_cam_record_get_status(ipc_cam_record_status_t *out);

/** Force audio stream 0 to G711A @ 8 kHz (RTSP PCMA + MP4 alaw). Safe to call early. */
void ipc_cam_audio_ensure_g711a(void);

#endif /* IPCAMERA_RECORD_H */
