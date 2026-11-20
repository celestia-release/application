// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Loop recording for Luna dashcam: configurable MP4 segments on the main H.264
 * stream with automatic deletion of the oldest segment when space runs low.
 * A dedicated worker thread drives recorder start/stop so the LVGL thread never blocks.
 */
#ifndef WEARABLE_DASHCAM_RECORD_H
#define WEARABLE_DASHCAM_RECORD_H

typedef struct {
    int recording;          /**< 1 while a segment is being written */
    int card_missing;       /**< 1 when TF card is absent/not writable */
    int storage_full;       /**< 1 when the card is full and no segment could be freed */
    int paused;             /**< 1 when recording is manually stopped (app still open) */
    int segment_elapsed_s;  /**< elapsed seconds of the current segment */
    int session_elapsed_s;  /**< total recorded seconds this dashcam session */
    int segment_count;      /**< number of segments recorded this session */
    long free_mb;           /**< last known free space (MiB), or -1 */
} dashcam_record_status_t;

/** Supported loop segment lengths (minutes). Default: 3. */
#define DASHCAM_SEGMENT_MIN_1   1
#define DASHCAM_SEGMENT_MIN_3   3
#define DASHCAM_SEGMENT_MIN_5   5
#define DASHCAM_SEGMENT_MIN_10  10

int dashcam_record_start(void);
void dashcam_record_stop(void);
int dashcam_record_is_active(void);
/** Manually pause/resume loop recording while the app stays open (0=record, 1=stop). */
void dashcam_record_set_paused(int paused);
int dashcam_record_is_paused(void);
void dashcam_record_get_status(dashcam_record_status_t *out);

/**
 * @brief Set loop segment length in minutes (1 / 3 / 5 / 10).
 * @details Must be called before dashcam_record_start(); ignored while worker runs.
 *          Drives segment cut timing and recorder set_max_duration for MP4 index sizing.
 */
void dashcam_record_set_segment_minutes(int minutes);
int dashcam_record_get_segment_minutes(void);

/** Force audio stream 0 to G711A @ 8 kHz (RTSP PCMA + MP4 alaw). Safe to call early. */
void dashcam_audio_ensure_g711a(void);

#endif /* WEARABLE_DASHCAM_RECORD_H */
