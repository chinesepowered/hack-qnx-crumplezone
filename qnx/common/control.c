/*
 * Crumple Zone: brake-by-wire and steering assist control laws.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "control.h"
#include "cz.h"

#include <math.h>
#include <string.h>

void cz_brake_default_params(cz_brake_params_t *p)
{
    p->p_max_bar = 160.0f;
    p->pedal_deadzone = 0.05f;
    p->pedal_gamma = 1.3f;
    p->slip_release = 0.20f;
    p->slip_apply = 0.10f;
    p->abs_min_speed = 2.0f;
    p->rate_track = 1200.0f;
    p->rate_apply = 600.0f;
    p->rate_release = 2500.0f;
    p->vref_decel_max = 12.0f;
    p->safe_hold_s = 0.050f;
    p->safe_pressure_bar = 30.0f;
    p->safe_rate = 200.0f;
    p->resume_samples = 3;
}

void cz_brake_init(cz_brake_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = CZ_MODE_NORMAL;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float cz_pedal_to_pressure(const cz_brake_params_t *p, float pedal)
{
    pedal = clampf(pedal, 0.0f, 1.0f);
    if (pedal <= p->pedal_deadzone) return 0.0f;
    float x = (pedal - p->pedal_deadzone) / (1.0f - p->pedal_deadzone);
    return p->p_max_bar * powf(x, p->pedal_gamma);
}

float cz_slip(float vref, float vwheel)
{
    if (vref < 0.5f) return 0.0f;          /* undefined near standstill */
    float s = (vref - vwheel) / vref;
    return clampf(s, 0.0f, 1.0f);
}

float cz_slew(float current, float target, float rate_per_s, float dt)
{
    float step = rate_per_s * dt;
    if (target > current + step) return current + step;
    if (target < current - step) return current - step;
    return target;
}

static void estimate_vref(const cz_brake_params_t *p, cz_brake_state_t *s,
                          const float ws[4], float dt)
{
    float vmax = ws[0];
    for (int i = 1; i < 4; i++) if (ws[i] > vmax) vmax = ws[i];
    /* The reference speed follows the fastest wheel up immediately, but may only
     * fall as fast as a physically plausible deceleration, so a locking wheel
     * cannot drag the estimate down with it. */
    float floor_v = s->vref - p->vref_decel_max * dt;
    s->vref = vmax > floor_v ? vmax : floor_v;
    if (s->vref < 0.0f) s->vref = 0.0f;
}

static void normal_control(const cz_brake_params_t *p, cz_brake_state_t *s,
                           const cz_brake_input_t *in, float dt)
{
    float demand = cz_pedal_to_pressure(p, in->pedal);
    estimate_vref(p, s, in->wheel_speed, dt);
    s->abs_mask = 0;
    for (int i = 0; i < 4; i++) {
        s->slip[i] = cz_slip(s->vref, in->wheel_speed[i]);
        float cur = s->pressure[i];
        float next;
        if (demand <= 0.0f) {
            next = cz_slew(cur, 0.0f, p->rate_release, dt);
        } else if (s->vref >= p->abs_min_speed && s->slip[i] > p->slip_release) {
            next = cz_slew(cur, 0.0f, p->rate_release, dt);      /* release */
            s->abs_mask |= 1u << i;
        } else if (s->vref >= p->abs_min_speed && s->slip[i] > p->slip_apply) {
            next = cur;                                          /* hold */
            if (cur < demand) s->abs_mask |= 1u << i;
        } else {
            float rate = (s->vref >= p->abs_min_speed && cur < demand && s->slip[i] > p->slip_apply * 0.5f)
                             ? p->rate_apply : p->rate_track;
            next = cz_slew(cur, demand, rate, dt);               /* apply / track */
        }
        s->pressure[i] = clampf(next, 0.0f, p->p_max_bar);
    }
    s->last_pedal = in->pedal;
}

int cz_brake_step(const cz_brake_params_t *p, cz_brake_state_t *s,
                  const cz_brake_input_t *in, float dt)
{
    int old = s->mode;

    if (!in->valid) {
        s->stale_s += dt;
        s->good_count = 0;
        s->abs_mask = 0;
        if (s->stale_s >= p->safe_hold_s) {
            s->mode = CZ_MODE_SAFE_STOP;
            for (int i = 0; i < 4; i++)
                s->pressure[i] = cz_slew(s->pressure[i], p->safe_pressure_bar, p->safe_rate, dt);
        } else {
            s->mode = CZ_MODE_SAFE_HOLD;   /* hold last valid command, no modulation */
        }
        /* The speed estimate coasts down at a gentle rate while blind. */
        s->vref = s->vref > 0.0f ? s->vref - 1.0f * dt : 0.0f;
        return s->mode != old;
    }

    if (s->mode == CZ_MODE_SAFE_HOLD || s->mode == CZ_MODE_SAFE_STOP) {
        s->mode = CZ_MODE_RESUMING;
        s->good_count = 0;
    }
    if (s->mode == CZ_MODE_RESUMING) {
        s->good_count++;
        if (s->good_count >= p->resume_samples) {
            s->mode = CZ_MODE_NORMAL;
            s->stale_s = 0.0f;
        }
    } else {
        s->stale_s = 0.0f;
    }
    /* While RESUMING the control law already runs on fresh data, with normal
     * slew limits, so there is no step in commanded pressure. */
    normal_control(p, s, in, dt);
    return s->mode != old;
}

void cz_steer_default_params(cz_steer_params_t *p)
{
    p->gain_low_speed = 3.0f;
    p->speed_knee = 15.0f;
    p->max_assist_nm = 8.0f;
    p->damping = 0.4f;
    p->fade_rate = 40.0f;
}

float cz_steer_assist(const cz_steer_params_t *p, float driver_torque, float speed, float steer_rate)
{
    if (speed < 0.0f) speed = 0.0f;
    float gain = p->gain_low_speed / (1.0f + speed / p->speed_knee);
    float a = gain * driver_torque - p->damping * steer_rate;
    return clampf(a, -p->max_assist_nm, p->max_assist_nm);
}
