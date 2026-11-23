/*
 * Crumple Zone: brake-by-wire and steering assist control laws
 * (portable C, unit tested on the host).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Brake: pedal -> demanded pressure, per-wheel slip-based anti-lock
 * modulation, rate limiting, and a degradation path when sensor data goes
 * stale: SAFE_HOLD (hold the last valid command) then SAFE_STOP (ramp to a
 * fixed, moderate stopping pressure) until fresh data returns.
 */
#ifndef CZ_CONTROL_H
#define CZ_CONTROL_H

#include <stdint.h>

typedef struct {
    float p_max_bar;           /* pressure at full pedal */
    float pedal_deadzone;      /* pedal travel ignored (0..1) */
    float pedal_gamma;         /* progressive pedal feel */
    float slip_release;        /* slip above which ABS releases pressure */
    float slip_apply;          /* slip below which ABS may re-apply */
    float abs_min_speed;       /* m/s: ABS inactive below this reference speed */
    float rate_track;          /* bar/s, normal pressure slew */
    float rate_apply;          /* bar/s, ABS re-apply slew */
    float rate_release;        /* bar/s, ABS release slew */
    float vref_decel_max;      /* m/s^2, limit on how fast the speed estimate may fall */
    float safe_hold_s;         /* seconds in SAFE_HOLD before SAFE_STOP */
    float safe_pressure_bar;   /* SAFE_STOP target pressure */
    float safe_rate;           /* bar/s ramp in SAFE_STOP */
    int   resume_samples;      /* consecutive good samples needed to resume */
} cz_brake_params_t;

typedef struct {
    int      mode;             /* enum cz_mode */
    float    pressure[4];      /* commanded, bar */
    float    slip[4];
    float    vref;             /* vehicle speed estimate, m/s */
    uint32_t abs_mask;         /* bit i set when wheel i is being modulated */
    float    stale_s;          /* time since the last valid sample */
    int      good_count;       /* consecutive good samples while RESUMING */
    float    last_pedal;
} cz_brake_state_t;

typedef struct {
    int   valid;               /* fresh, plausible sensor data this period */
    float wheel_speed[4];      /* m/s at the rim */
    float pedal;               /* 0..1 */
} cz_brake_input_t;

void  cz_brake_default_params(cz_brake_params_t *p);
void  cz_brake_init(cz_brake_state_t *s);
float cz_pedal_to_pressure(const cz_brake_params_t *p, float pedal);
float cz_slip(float vref, float vwheel);
float cz_slew(float current, float target, float rate_per_s, float dt);
/* One control step. Returns 1 when the mode changed. */
int   cz_brake_step(const cz_brake_params_t *p, cz_brake_state_t *s,
                    const cz_brake_input_t *in, float dt);

/* Electric power steering assist. */
typedef struct {
    float gain_low_speed;      /* assist gain at standstill */
    float speed_knee;          /* m/s where gain has halved */
    float max_assist_nm;
    float damping;             /* Nm per rad/s */
    float fade_rate;           /* Nm/s ramp to zero assist when inputs are stale */
} cz_steer_params_t;

void  cz_steer_default_params(cz_steer_params_t *p);
float cz_steer_assist(const cz_steer_params_t *p, float driver_torque, float speed, float steer_rate);

#endif
