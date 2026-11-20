// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Battery SoC sampling with charge/discharge compensation.
 */

#ifndef TABLET_BATTERY_H
#define TABLET_BATTERY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Charger state is inferred (TC4056A CHRG pin drives LEDs only, no GPIO). */
typedef enum {
    TABLET_BAT_STATE_INIT = 0,      /* boot, watching level/slope to latch state */
    TABLET_BAT_STATE_CHARGING,
    TABLET_BAT_STATE_DISCHARGING,
    TABLET_BAT_STATE_SETTLING,      /* plug/unplug seen, waiting for cell relaxation */
} tablet_battery_state_t;

/* Poll once per second from the UI timer. Returns 0..100, or -1 if unknown. */
int tablet_battery_get_percent(void);

tablet_battery_state_t tablet_battery_get_state(void);

/*
 * 1 while plugged/charging (state CHARGING, or SETTLING toward charging).
 * Inferred from voltage behaviour; no charger GPIO exists on this board.
 */
int tablet_battery_is_charging(void);

#ifdef __cplusplus
}
#endif

#endif /* TABLET_BATTERY_H */
