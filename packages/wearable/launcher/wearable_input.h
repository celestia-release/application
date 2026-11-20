// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Gesture input: forwards swipe/tap to gui_shell_on_gesture().
 */
#ifndef APPLICATION_WEARABLE_INPUT_H
#define APPLICATION_WEARABLE_INPUT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void wearable_input_init(lv_obj_t *root);

/** Non-zero if the latest touch release was a horizontal page swipe. */
int wearable_input_had_horizontal_swipe(void);

/**
 * Non-zero if the current/last contact was classified as a swipe.
 * App-local tap handlers should skip their action when this is set, because
 * RELEASED bubbles to the app before wearable_input emits the swipe.
 */
int wearable_input_swipe_active(void);

/** Clear in-flight touch state (call when opening an app after a tile tap). */
void wearable_input_reset_gesture(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_WEARABLE_INPUT_H */
