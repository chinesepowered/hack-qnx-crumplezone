/*
 * Crumple Zone: vehicle model.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "plant.h"

#include <math.h>
#include <string.h>

#define MASS        1800.0f   /* kg */
#define GRAVITY     9.81f
#define WHEEL_J     1.1f      /* kg m^2 */
#define BRAKE_K     15.0f     /* Nm per bar */
#define DRIVE_MAX   1100.0f   /* Nm per rear wheel at full throttle */
#define DRAG_C      0.42f     /* N/(m/s)^2 */
#define ROLL_F      180.0f    /* N */
#define TIRE_B      12.0f
#define TIRE_C      1.6f
#define SLIP_V_MIN  3.0f      /* m/s floor in the slip denominator keeps the model stable near standstill */
#define SUBSTEPS    4

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float cz_tire_mu(float slip)
{
    /* Simplified Pacejka "magic formula": peaks near 12% slip, odd in slip. */
    return sinf(TIRE_C * atanf(TIRE_B * slip));
}

float cz_wheel_speed(const cz_plant_t *p, int i)
{
    return p->omega[i] * CZ_WHEEL_RADIUS;
}

void cz_plant_init(cz_plant_t *p, float v0)
{
    memset(p, 0, sizeof(*p));
    p->v = v0;
    for (int i = 0; i < 4; i++) p->omega[i] = v0 / CZ_WHEEL_RADIUS;
    p->mu = 0.9f;
}

void cz_plant_step(cz_plant_t *p, const float brake_bar[4], float assist_nm, float dt)
{
    const float h = dt / (float)SUBSTEPS;
    const float normal = MASS * GRAVITY / 4.0f;

    for (int k = 0; k < SUBSTEPS; k++) {
        float fx_total = 0.0f;
        for (int i = 0; i < 4; i++) {
            float vw = p->omega[i] * CZ_WHEEL_RADIUS;
            float denom = p->v > SLIP_V_MIN ? p->v : SLIP_V_MIN;
            float slip = (p->v - vw) / denom;          /* >0 braking, <0 driving */
            slip = clampf(slip, -1.0f, 1.0f);
            float fx = p->mu * normal * cz_tire_mu(slip); /* force decelerating the car */

            float tb = BRAKE_K * clampf(brake_bar[i], 0.0f, 250.0f);
            float td = (i >= 2) ? DRIVE_MAX * clampf(p->throttle, 0.0f, 1.0f) : 0.0f;
            /* wheel: J w' = Fx r + Td - Tb (brake torque opposes rotation) */
            float wdot = (fx * CZ_WHEEL_RADIUS + td - (p->omega[i] > 0.0f ? tb : 0.0f)) / WHEEL_J;
            float w = p->omega[i] + wdot * h;
            if (w < 0.0f) w = 0.0f;                   /* brakes hold a stopped wheel */
            p->omega[i] = w;
            fx_total += fx;
        }
        float drag = DRAG_C * p->v * p->v + (p->v > 0.05f ? ROLL_F : 0.0f);
        float vdot = (-fx_total - drag) / MASS;
        p->v += vdot * h;
        if (p->v < 0.0f) p->v = 0.0f;
    }

    /* Steering column: driver + assist against self-aligning stiffness and damping. */
    float k_align = 40.0f + 2.0f * p->v;
    float acc = (p->driver_torque + assist_nm - k_align * p->steer_angle - 6.0f * p->steer_rate) / 0.08f;
    p->steer_rate += acc * dt;
    p->steer_angle += p->steer_rate * dt;
    p->steer_angle = clampf(p->steer_angle, -0.6f, 0.6f);

    p->t += dt;
}

void cz_driver_update(cz_plant_t *p)
{
    /* A repeating 15 s drive cycle: accelerate to 100 km/h, lane change,
     * hard stop with a low-friction patch (anti-lock active), recover. */
    float c = fmodf(p->t, CZ_DRIVE_CYCLE_S);
    const float v_target_fast = 27.8f;   /* 100 km/h */
    const float v_target_slow = 16.7f;   /* 60 km/h */

    p->driver_torque = 0.0f;
    p->mu = 0.9f;
    if (c < 6.0f) {
        p->pedal = 0.0f;
        p->throttle = clampf((v_target_fast - p->v) * 0.35f, 0.0f, 1.0f);
    } else if (c < 9.0f) {
        p->pedal = 0.0f;
        p->throttle = clampf((v_target_fast - p->v) * 0.35f + 0.12f, 0.0f, 1.0f);
        p->driver_torque = 2.5f * sinf(2.0f * 3.14159265f * (c - 6.0f) / 3.0f);
    } else if (c < 12.0f) {
        p->throttle = 0.0f;
        float ramp = (c - 9.0f) / 0.25f;
        p->pedal = clampf(ramp, 0.0f, 1.0f) * 0.92f;
        if (c > 9.6f && c < 11.0f) p->mu = 0.40f;      /* wet patch */
    } else {
        p->pedal = 0.0f;
        p->throttle = clampf((v_target_slow - p->v) * 0.35f, 0.0f, 1.0f);
    }
}
