// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Luna digital clock face for launcher home tile.
 */
#ifndef WEARABLE_CLOCK_H
#define WEARABLE_CLOCK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create digital clock UI on parent (time, date, weather).
 * @param parent Tile or container; must outlive the clock widgets.
 * @return Root clock container, or NULL on failure.
 */
lv_obj_t *wearable_clock_create(lv_obj_t *parent);

/** Pause/resume 1 Hz clock timer (e.g. while a full-screen app is open). */
void wearable_clock_set_suspended(int suspended);

#ifdef __cplusplus
}
#endif

#endif /* WEARABLE_CLOCK_H */
