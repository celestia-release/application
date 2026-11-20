// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Cached main/preview stream indices and channel_group_id (framework multimedia only).
 */
#ifndef WEARABLE_CAMERA_STREAMS_H
#define WEARABLE_CAMERA_STREAMS_H

#include "gui_common.h"

/** Resolve and cache streams from multimedia_t (via gui_camera_streams_resolve). */
int wearable_camera_streams_refresh(void);

/** Cached stream layout; valid after successful refresh. */
const gui_camera_streams_t *wearable_camera_streams_get(void);

#endif /* WEARABLE_CAMERA_STREAMS_H */
