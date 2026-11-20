// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/packages/tablet.
 *
 * Battery SoC from SARADC AIN0 (BAT_DET) with charge/discharge compensation.
 *
 * Pipeline (polled 1 Hz from the home timer):
 *   pin mV -> Vbat(x3) -> light EMA -> state machine -> OCV lookup -> ramp
 *
 * State machine: plugging/unplugging the charger shifts the cell voltage by
 * the IR term (~210 mV between on-charger and loaded at the same true SoC,
 * see TABLET_BAT_DISCHARGE_OFFSET_MV). A fast step detector catches plug
 * events and freezes the display through the cell relaxation window (the
 * true SoC did not change), then the state-specific lookup keeps the
 * reported SoC continuous. A slow 30 s trend check covers gentle charge
 * start/stop that skips the step detector. No charger GPIO / current sense
 * exists on this board (CHRG is LED-only), so state is always inferred from
 * voltage behaviour.
 */
#define LOG_TAG "TABLET_BAT"

#include "tablet_battery.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

/**********************
 *      DEFINES
 **********************/
/*
 * External divider (PDB_MBD schematics, R17/R18):
 *   Vbat --R17 1M-- BAT_DET --R18 2M-- GND  => pin = Vbat/3.
 * ak_saradc IIO in_voltage0_raw reports millivolts at BAT_DET.
 */
#define TABLET_BAT_ADC0_RAW_PATH        "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define TABLET_BAT_HW_DIVIDER           3
#define TABLET_BAT_EMPTY_MV             3000
#define TABLET_BAT_FULL_MV              4200

/*
 * Charging vs loaded-discharge gap measured 2026-09-14 with bat_log.txt:
 * the same true SoC read 4056 mV on charger vs 3846 mV loaded => ~210 mV IR
 * hysteresis. Discharging lookups add this offset so one OCV table serves
 * both states. Re-calibrate from a full-discharge log run if the load
 * profile changes materially.
 */
#define TABLET_BAT_DISCHARGE_OFFSET_MV  210

/* Fast plug/unplug detector: |V now - V 5 s ago| well above ADC noise (±2 mV). */
#define TABLET_BAT_STEP_MV              20
#define TABLET_BAT_STEP_WIN             5
/* Cell relaxation after a plug/unplug event before trusting the table again. */
#define TABLET_BAT_SETTLE_S             180
/* Slow trend check: charge start/stop shows as ±15 mV per 30 s sustained. */
#define TABLET_BAT_TREND_MV             15
#define TABLET_BAT_TREND_WIN            30

/* Display ramp: converges <=1 pct/s once a steady state is latched. */
#define TABLET_BAT_RAMP_PCT             1

/*
 * INIT latch: if the first 30 s show a clear slope, trust it; a flat line on
 * the high plateau (>= this pin level) is CV charging, else discharging.
 */
#define TABLET_BAT_INIT_SLOPE_MV        15
#define TABLET_BAT_INIT_LEVEL_PIN_MV    1320

/* Light EMA on millivolts: mv = v + (prev - v) * 3 / 4. */
#define TABLET_BAT_MV_EMA_SHIFT         2

/**********************
 *  STATIC PROTOTYPES
 **********************/
static int tablet_read_sysfs_capacity(void);
static int tablet_read_sysfs_status_charging(void);
static int tablet_read_adc0_pin_mv(void);
static int tablet_percent_from_vbat_mv(int mv);
static int tablet_push_win(int *win, int *len, int cap, int mv);
static void tablet_enter_settling(tablet_battery_state_t target, int delta_mv);

/**********************
 *  STATIC VARIABLES
 **********************/
static tablet_battery_state_t s_state = TABLET_BAT_STATE_INIT;
static tablet_battery_state_t s_settle_target = TABLET_BAT_STATE_DISCHARGING;
static int s_settle_left_s;
static int s_mv_ema = -1;               /* smoothed Vbat, mV; -1 = no sample yet */
static int s_disp_pct = -1;             /* rate-limited value shown by the UI */
static int s_logged_first;
static int s_step_win[TABLET_BAT_STEP_WIN];
static int s_step_len;
static int s_trend_win[TABLET_BAT_TREND_WIN];
static int s_trend_len;

/**********************
 *  STATIC FUNCTIONS
 **********************/
/* A registered power_supply class device is authoritative when present. */
static int tablet_read_sysfs_capacity(void)
{
    static const char *paths[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/bms/capacity",
        NULL,
    };
    size_t i;

    for (i = 0; paths[i] != NULL; i++) {
        FILE *fp = fopen(paths[i], "r");
        int pct = -1;

        if (!fp)
            continue;
        if (fscanf(fp, "%d", &pct) == 1) {
            fclose(fp);
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
            return pct;
        }
        fclose(fp);
    }
    return -1;
}

/* Best-effort charger state from power_supply; 1 charging, 0 not, -1 unknown. */
static int tablet_read_sysfs_status_charging(void)
{
    FILE *fp;
    char status[32] = {0};
    int charging = -1;

    fp = fopen("/sys/class/power_supply/battery/status", "r");
    if (!fp)
        return -1;
    if (fgets(status, sizeof(status), fp) != NULL) {
        if (!strncmp(status, "Charg", 5))
            charging = 1;
        else if (!strncmp(status, "Disc", 4) || !strncmp(status, "Full", 4))
            charging = 0;
    }
    fclose(fp);
    return charging;
}

static int tablet_read_adc0_pin_mv(void)
{
    FILE *fp;
    int mv = -1;

    fp = fopen(TABLET_BAT_ADC0_RAW_PATH, "r");
    if (!fp)
        return -1;
    if (fscanf(fp, "%d", &mv) != 1)
        mv = -1;
    fclose(fp);
    /* Pin after ÷3: expect roughly 1000..1400 mV for a Li-ion pack. */
    if (mv < 0 || mv > 5000)
        return -1;
    return mv;
}

/*
 * Base OCV table (charging-side curve; nominal cell 3.7V = 50%).
 * Points are (Vbat_mV, percent), descending voltage.
 */
static int tablet_percent_from_vbat_mv(int mv)
{
    static const struct {
        int mv;
        int pct;
    } ocv[] = {
        { TABLET_BAT_FULL_MV, 100 },
        { 4100, 90 },
        { 4000, 80 },
        { 3920, 70 },
        { 3850, 60 },
        { 3780, 55 },
        { 3700, 50 }, /* nominal */
        { 3620, 40 },
        { 3500, 25 },
        { 3400, 15 },
        { 3300, 8 },
        { TABLET_BAT_EMPTY_MV, 0 },
    };
    size_t n = sizeof(ocv) / sizeof(ocv[0]);
    size_t i;

    if (mv >= ocv[0].mv)
        return 100;
    if (mv <= ocv[n - 1].mv)
        return 0;

    for (i = 0; i + 1 < n; i++) {
        if (mv <= ocv[i].mv && mv >= ocv[i + 1].mv) {
            int mv_hi = ocv[i].mv;
            int mv_lo = ocv[i + 1].mv;
            int pct_hi = ocv[i].pct;
            int pct_lo = ocv[i + 1].pct;
            int span = mv_hi - mv_lo;

            if (span <= 0)
                return pct_lo;
            return pct_lo + (pct_hi - pct_lo) * (mv - mv_lo) / span;
        }
    }
    return 0;
}

/* Append to a sliding window; returns delta vs oldest once full, else 0. */
static int tablet_push_win(int *win, int *len, int cap, int mv)
{
    int i;

    if (*len < cap) {
        win[(*len)++] = mv;
        return 0;
    }
    for (i = 1; i < cap; i++)
        win[i - 1] = win[i];
    win[cap - 1] = mv;
    return win[cap - 1] - win[0];
}

static void tablet_enter_settling(tablet_battery_state_t target, int delta_mv)
{
    s_state = TABLET_BAT_STATE_SETTLING;
    s_settle_target = target;
    s_settle_left_s = TABLET_BAT_SETTLE_S;
    log_info("plug event %+dmV -> settling as %s", delta_mv,
             target == TABLET_BAT_STATE_CHARGING ? "charging" : "discharging");
}

/* Boot-time charger state latch from the first window of samples. */
static void tablet_latch_init_state(void)
{
    int slope = s_trend_win[s_trend_len - 1] - s_trend_win[0];

    if (slope >= TABLET_BAT_INIT_SLOPE_MV)
        s_state = TABLET_BAT_STATE_CHARGING;
    else if (slope <= -TABLET_BAT_INIT_SLOPE_MV)
        s_state = TABLET_BAT_STATE_DISCHARGING;
    else if (s_mv_ema >= TABLET_BAT_INIT_LEVEL_PIN_MV * TABLET_BAT_HW_DIVIDER)
        s_state = TABLET_BAT_STATE_CHARGING;
    else
        s_state = TABLET_BAT_STATE_DISCHARGING;
    log_info("charger state latched to %s (boot slope %+dmV)",
             s_state == TABLET_BAT_STATE_CHARGING ? "charging" : "discharging",
             slope);
}

/**********************
 *   PUBLIC FUNCTIONS
 **********************/
int tablet_battery_get_percent(void)
{
    int sysfs_pct;
    int sysfs_chg;
    int pin_mv;
    int vbat_mv;
    int lookup_mv;
    int target_pct;
    int step_delta;
    int trend_delta;

    /* A registered power_supply driver outranks the raw-ADC path entirely. */
    sysfs_pct = tablet_read_sysfs_capacity();
    if (sysfs_pct >= 0) {
        sysfs_chg = tablet_read_sysfs_status_charging();
        if (sysfs_chg == 1)
            s_state = TABLET_BAT_STATE_CHARGING;
        else if (sysfs_chg == 0 && s_state == TABLET_BAT_STATE_CHARGING)
            s_state = TABLET_BAT_STATE_DISCHARGING;
        s_disp_pct = sysfs_pct;
        return s_disp_pct;
    }

    pin_mv = tablet_read_adc0_pin_mv();
    if (pin_mv < 0) {
        /* Transient read failure: keep showing the last known value. */
        return s_disp_pct;
    }

    vbat_mv = pin_mv * TABLET_BAT_HW_DIVIDER;
    if (s_mv_ema < 0)
        s_mv_ema = vbat_mv;
    else
        s_mv_ema = vbat_mv + (((s_mv_ema - vbat_mv) *
                               ((1 << TABLET_BAT_MV_EMA_SHIFT) - 1)) >>
                              TABLET_BAT_MV_EMA_SHIFT);

    /* Sliding windows over the smoothed signal. */
    step_delta = tablet_push_win(s_step_win, &s_step_len, TABLET_BAT_STEP_WIN,
                                 s_mv_ema);
    trend_delta = tablet_push_win(s_trend_win, &s_trend_len,
                                  TABLET_BAT_TREND_WIN, s_mv_ema);

    /* Fast detector: a real plug/unplug moves 35+ mV inside 5 s (steady states only). */
    if (s_state != TABLET_BAT_STATE_SETTLING &&
        s_step_len >= TABLET_BAT_STEP_WIN && step_delta >= TABLET_BAT_STEP_MV)
        tablet_enter_settling(TABLET_BAT_STATE_CHARGING, step_delta);
    else if (s_step_len >= TABLET_BAT_STEP_WIN &&
             step_delta <= -TABLET_BAT_STEP_MV)
        tablet_enter_settling(TABLET_BAT_STATE_DISCHARGING, step_delta);
    /* Slow detector only runs in steady states (settling handles its own). */
    else if (s_state == TABLET_BAT_STATE_CHARGING &&
             s_trend_len >= TABLET_BAT_TREND_WIN &&
             trend_delta <= -TABLET_BAT_TREND_MV)
        tablet_enter_settling(TABLET_BAT_STATE_DISCHARGING, trend_delta);
    else if (s_state == TABLET_BAT_STATE_DISCHARGING &&
             s_trend_len >= TABLET_BAT_TREND_WIN &&
             trend_delta >= TABLET_BAT_TREND_MV)
        tablet_enter_settling(TABLET_BAT_STATE_CHARGING, trend_delta);
    else if (s_state == TABLET_BAT_STATE_INIT &&
             s_trend_len >= TABLET_BAT_TREND_WIN)
        tablet_latch_init_state();

    if (s_state == TABLET_BAT_STATE_SETTLING) {
        /* True SoC did not change: hold the pre-event number, let the cell
         * relax, then re-anchor from the state-specific table below. */
        if (--s_settle_left_s <= 0) {
            s_state = s_settle_target;
            /* The plug jump is handled: drop it from the fast window so the
             * stale slope cannot re-trigger settling every sample. */
            memset(s_step_win, 0, sizeof(s_step_win));
            s_step_len = 0;
            log_info("settled -> %s",
                     s_state == TABLET_BAT_STATE_CHARGING ? "charging"
                                                          : "discharging");
        }
        return s_disp_pct;
    }

    /* SoC lookup: discharge path compensates the IR drop so both states map
     * to the same displayed number at the same true charge level. */
    lookup_mv = s_mv_ema;
    if (s_state == TABLET_BAT_STATE_DISCHARGING)
        lookup_mv += TABLET_BAT_DISCHARGE_OFFSET_MV;
    target_pct = tablet_percent_from_vbat_mv(lookup_mv);

    /* Ramp governor: <=1 pct/s so convergence never looks like a jump. */
    if (s_disp_pct < 0) {
        s_disp_pct = target_pct;
    } else if (s_disp_pct < target_pct) {
        s_disp_pct += (target_pct - s_disp_pct < TABLET_BAT_RAMP_PCT)
                          ? (target_pct - s_disp_pct)
                          : TABLET_BAT_RAMP_PCT;
    } else if (s_disp_pct > target_pct) {
        s_disp_pct -= (s_disp_pct - target_pct < TABLET_BAT_RAMP_PCT)
                          ? (s_disp_pct - target_pct)
                          : TABLET_BAT_RAMP_PCT;
    }

    if (!s_logged_first && s_disp_pct >= 0) {
        s_logged_first = 1;
        log_info("first SoC %d%% (pin=%dmV Vbat=%dmV state=%d)", s_disp_pct,
                 pin_mv, vbat_mv, s_state);
    }
    return s_disp_pct;
}

tablet_battery_state_t tablet_battery_get_state(void)
{
    return s_state;
}

int tablet_battery_is_charging(void)
{
    if (s_state == TABLET_BAT_STATE_CHARGING)
        return 1;
    if (s_state == TABLET_BAT_STATE_SETTLING &&
        s_settle_target == TABLET_BAT_STATE_CHARGING)
        return 1;
    return 0;
}
