/*
 * Crumple Zone: host unit tests for the portable control, statistics and
 * vehicle model code (built with the host gcc by `make test`).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cz.h"
#include "control.h"
#include "plant.h"
#include "stats.h"

static int g_fail, g_pass;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b, tol) do { \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) <= (tol)) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s = %g, expected %g +/- %g\n", \
                             __FILE__, __LINE__, #a, _a, _b, (double)(tol)); } \
} while (0)

/* ---------------- statistics ---------------- */

static void test_stats_basic(void)
{
    static cz_stats_t s;
    cz_stats_reset(&s);
    CHECK(s.n == 0);
    CHECK(cz_stats_percentile(&s, 99) == 0);
    int64_t vals[] = { 10000, 20000, 30000, 40000, 50000 };
    for (int i = 0; i < 5; i++) cz_stats_add(&s, vals[i]);
    CHECK(s.n == 5);
    CHECK(s.min == 10000);
    CHECK(s.max == 50000);
    CHECK_NEAR(cz_stats_mean(&s), 30000.0, 1e-6);
    CHECK_NEAR(cz_stats_stddev(&s), 15811.388, 0.01);   /* sample stddev */
    CHECK(cz_stats_percentile(&s, 0) == 10000);
    CHECK(cz_stats_percentile(&s, 100) == 50000);
    /* p50 lands in the 30 us bin, upper edge 31 us */
    CHECK(cz_stats_percentile(&s, 50) == 31000);
}

static void test_stats_percentile_large(void)
{
    static cz_stats_t s;
    cz_stats_reset(&s);
    /* 10000 samples of 5 us and ten of 900 us: p99 must stay small, p99.95 must see the tail */
    for (int i = 0; i < 10000; i++) cz_stats_add(&s, 5000);
    for (int i = 0; i < 10; i++) cz_stats_add(&s, 900000);
    CHECK(cz_stats_percentile(&s, 99.0) == 6000);
    CHECK(cz_stats_percentile(&s, 99.95) == 900000);
    CHECK(s.max == 900000);
}

static void test_stats_overflow_and_negative(void)
{
    static cz_stats_t s;
    cz_stats_reset(&s);
    cz_stats_add(&s, -500);            /* early wake-ups land in bin 0 */
    cz_stats_add(&s, 9000000);         /* 9 ms, beyond the histogram */
    CHECK(s.hist[0] == 1);
    CHECK(s.hist[CZ_HIST_BINS] == 1);
    CHECK(cz_stats_percentile(&s, 100) == 9000000);
    CHECK(s.min == -500);
}

static void test_period_math(void)
{
    cz_period_t p = { .period_ns = 1000000, .start_ns = 5000000 };
    CHECK(cz_release_index(&p, 4000000) == 0);
    CHECK(cz_release_index(&p, 5000000) == 0);
    CHECK(cz_release_index(&p, 5999999) == 0);
    CHECK(cz_release_index(&p, 6000000) == 1);
    CHECK(cz_release_index(&p, 7500000) == 2);
    CHECK(cz_release_time(&p, 3) == 8000000);
    CHECK(!cz_deadline_missed(&p, 3, 8999999));
    CHECK(!cz_deadline_missed(&p, 3, 9000000));
    CHECK(cz_deadline_missed(&p, 3, 9000001));
}

/* ---------------- brake control ---------------- */

static void test_pedal_map(void)
{
    cz_brake_params_t p;
    cz_brake_default_params(&p);
    CHECK_NEAR(cz_pedal_to_pressure(&p, 0.0f), 0.0, 1e-6);
    CHECK_NEAR(cz_pedal_to_pressure(&p, 0.04f), 0.0, 1e-6);    /* dead zone */
    CHECK_NEAR(cz_pedal_to_pressure(&p, 1.0f), 160.0, 1e-3);
    CHECK_NEAR(cz_pedal_to_pressure(&p, 2.0f), 160.0, 1e-3);   /* clamped */
    float a = cz_pedal_to_pressure(&p, 0.3f), b = cz_pedal_to_pressure(&p, 0.6f);
    CHECK(a > 0.0f && b > a);
    CHECK(b > 2.0f * a);                                        /* progressive */
}

static void test_slip_and_slew(void)
{
    CHECK_NEAR(cz_slip(20.0f, 20.0f), 0.0, 1e-6);
    CHECK_NEAR(cz_slip(20.0f, 16.0f), 0.2, 1e-6);
    CHECK_NEAR(cz_slip(20.0f, 0.0f), 1.0, 1e-6);
    CHECK_NEAR(cz_slip(20.0f, 25.0f), 0.0, 1e-6);              /* driving slip clamps to 0 */
    CHECK_NEAR(cz_slip(0.1f, 0.0f), 0.0, 1e-6);                /* standstill */
    CHECK_NEAR(cz_slew(0.0f, 10.0f, 1000.0f, 0.001f), 1.0, 1e-6);
    CHECK_NEAR(cz_slew(10.0f, 0.0f, 1000.0f, 0.001f), 9.0, 1e-6);
    CHECK_NEAR(cz_slew(5.0f, 5.5f, 1000.0f, 0.001f), 5.5, 1e-6);
}

static void test_brake_tracks_pedal(void)
{
    cz_brake_params_t p; cz_brake_state_t s; cz_brake_input_t in;
    cz_brake_default_params(&p);
    cz_brake_init(&s);
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    for (int i = 0; i < 4; i++) in.wheel_speed[i] = 20.0f;
    in.pedal = 0.5f;
    float demand = cz_pedal_to_pressure(&p, 0.5f);
    for (int k = 0; k < 500; k++) cz_brake_step(&p, &s, &in, 0.001f);
    for (int i = 0; i < 4; i++) CHECK_NEAR(s.pressure[i], demand, 0.01);
    CHECK(s.mode == CZ_MODE_NORMAL);
    CHECK(s.abs_mask == 0);
    in.pedal = 0.0f;
    for (int k = 0; k < 500; k++) cz_brake_step(&p, &s, &in, 0.001f);
    for (int i = 0; i < 4; i++) CHECK_NEAR(s.pressure[i], 0.0, 1e-6);
}

static void test_abs_releases_locking_wheel(void)
{
    cz_brake_params_t p; cz_brake_state_t s; cz_brake_input_t in;
    cz_brake_default_params(&p);
    cz_brake_init(&s);
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    in.pedal = 1.0f;
    for (int i = 0; i < 4; i++) in.wheel_speed[i] = 25.0f;
    for (int k = 0; k < 200; k++) cz_brake_step(&p, &s, &in, 0.001f);
    float before = s.pressure[0];
    CHECK(before > 100.0f);
    /* front-left wheel suddenly at 50% slip: its pressure must drop, the others must not */
    in.wheel_speed[0] = 12.5f;
    for (int k = 0; k < 10; k++) cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.pressure[0] < before - 20.0f);
    CHECK(s.abs_mask & 1u);
    CHECK(!(s.abs_mask & 2u));
    CHECK(s.pressure[1] >= before - 1e-3f);
    CHECK(s.slip[0] > 0.4f);
}

static void test_safe_state_sequence(void)
{
    cz_brake_params_t p; cz_brake_state_t s; cz_brake_input_t in;
    cz_brake_default_params(&p);
    cz_brake_init(&s);
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    in.pedal = 0.4f;
    for (int i = 0; i < 4; i++) in.wheel_speed[i] = 20.0f;
    for (int k = 0; k < 300; k++) cz_brake_step(&p, &s, &in, 0.001f);
    float held = s.pressure[2];

    /* One stale period: SAFE_HOLD immediately, command frozen. */
    in.valid = 0;
    CHECK(cz_brake_step(&p, &s, &in, 0.001f) == 1);
    CHECK(s.mode == CZ_MODE_SAFE_HOLD);
    CHECK_NEAR(s.pressure[2], held, 1e-6);
    for (int k = 0; k < 40; k++) cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_SAFE_HOLD);
    CHECK_NEAR(s.pressure[2], held, 1e-6);
    /* Past the hold budget: SAFE_STOP ramps toward the safe pressure. */
    for (int k = 0; k < 20; k++) cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_SAFE_STOP);
    for (int k = 0; k < 3000; k++) cz_brake_step(&p, &s, &in, 0.001f);
    for (int i = 0; i < 4; i++) CHECK_NEAR(s.pressure[i], p.safe_pressure_bar, 1e-3);

    /* Fresh data returns: RESUMING for resume_samples-1 steps, then NORMAL. */
    in.valid = 1;
    cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_RESUMING);
    cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_RESUMING);
    cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_NORMAL);
    /* A single glitch during RESUMING restarts the count. */
    in.valid = 0; cz_brake_step(&p, &s, &in, 0.001f);
    in.valid = 1; cz_brake_step(&p, &s, &in, 0.001f);
    CHECK(s.mode == CZ_MODE_RESUMING);
}

static void test_steer_assist(void)
{
    cz_steer_params_t p;
    cz_steer_default_params(&p);
    CHECK_NEAR(cz_steer_assist(&p, 0.0f, 10.0f, 0.0f), 0.0, 1e-6);
    float low = cz_steer_assist(&p, 1.0f, 0.0f, 0.0f);
    float high = cz_steer_assist(&p, 1.0f, 30.0f, 0.0f);
    CHECK(low > high && high > 0.0f);                          /* less assist at speed */
    CHECK_NEAR(cz_steer_assist(&p, 100.0f, 0.0f, 0.0f), p.max_assist_nm, 1e-6);
    CHECK_NEAR(cz_steer_assist(&p, -100.0f, 0.0f, 0.0f), -p.max_assist_nm, 1e-6);
    CHECK(cz_steer_assist(&p, 0.0f, 10.0f, 1.0f) < 0.0f);      /* damping opposes motion */
}

/* ---------------- closed loop: plant + controller ---------------- */

static void run_stop(int with_abs, float mu, float *stop_dist, float *max_slip_after_settle, float *t_stop)
{
    cz_plant_t pl;
    cz_plant_init(&pl, 27.8f);
    pl.mu = mu;
    cz_brake_params_t p; cz_brake_state_t s;
    cz_brake_default_params(&p);
    cz_brake_init(&s);
    float dist = 0.0f, max_slip = 0.0f;
    float t = 0.0f;
    while (pl.v > 0.5f && t < 20.0f) {
        cz_brake_input_t in = { .valid = 1, .pedal = 1.0f };
        for (int i = 0; i < 4; i++) in.wheel_speed[i] = cz_wheel_speed(&pl, i);
        float cmd[4];
        if (with_abs) {
            cz_brake_step(&p, &s, &in, 0.001f);
            memcpy(cmd, s.pressure, sizeof(cmd));
        } else {
            for (int i = 0; i < 4; i++) cmd[i] = 160.0f;
        }
        cz_plant_step(&pl, cmd, 0.0f, 0.001f);
        pl.mu = mu;
        dist += pl.v * 0.001f;
        t += 0.001f;
        if (t > 0.3f && pl.v > 5.0f) {
            for (int i = 0; i < 4; i++) {
                float sl = cz_slip(pl.v, cz_wheel_speed(&pl, i));
                if (sl > max_slip) max_slip = sl;
            }
        }
    }
    *stop_dist = dist;
    *max_slip_after_settle = max_slip;
    *t_stop = t;
}

static void test_closed_loop_abs(void)
{
    float d_abs, slip_abs, t_abs, d_lock, slip_lock, t_lock;
    run_stop(1, 0.4f, &d_abs, &slip_abs, &t_abs);
    run_stop(0, 0.4f, &d_lock, &slip_lock, &t_lock);
    /* Without modulation the wheels lock on a wet road. */
    CHECK(slip_lock > 0.9f);
    /* With modulation slip stays bounded and the stop is shorter. */
    CHECK(slip_abs < 0.45f);
    CHECK(d_abs < d_lock);
    CHECK(t_abs < 20.0f);
    printf("  closed loop on mu=0.4 from 100 km/h: ABS %.1f m (max slip %.2f), locked %.1f m (slip %.2f)\n",
           d_abs, slip_abs, d_lock, slip_lock);
}

static void test_plant_physics(void)
{
    cz_plant_t pl;
    cz_plant_init(&pl, 20.0f);
    float zero[4] = { 0, 0, 0, 0 };
    /* Coasting slows the car only gently (drag + rolling resistance). */
    for (int k = 0; k < 1000; k++) cz_plant_step(&pl, zero, 0.0f, 0.001f);
    CHECK(pl.v < 20.0f && pl.v > 19.0f);
    /* Moderate braking on dry road: deceleration ~ 0.4..0.9 g */
    float cmd[4] = { 50, 50, 50, 50 };
    float v0 = pl.v;
    for (int k = 0; k < 1000; k++) cz_plant_step(&pl, cmd, 0.0f, 0.001f);
    float decel = v0 - pl.v;
    CHECK(decel > 3.5f && decel < 9.0f);
    /* Drive cycle reaches highway speed within the acceleration phase. */
    cz_plant_init(&pl, 16.7f);
    for (int k = 0; k < 8900; k++) { cz_driver_update(&pl); cz_plant_step(&pl, zero, 0.0f, 0.001f); }
    CHECK(pl.v > 24.0f && pl.v < 30.0f);
    CHECK_NEAR(cz_tire_mu(0.0f), 0.0, 1e-6);
    CHECK(cz_tire_mu(0.12f) > cz_tire_mu(0.6f));                /* peak before full lock */
    CHECK(cz_tire_mu(-0.1f) < 0.0f);
}

int main(void)
{
    test_stats_basic();
    test_stats_percentile_large();
    test_stats_overflow_and_negative();
    test_period_math();
    test_pedal_map();
    test_slip_and_slew();
    test_brake_tracks_pedal();
    test_abs_releases_locking_wheel();
    test_safe_state_sequence();
    test_steer_assist();
    test_plant_physics();
    test_closed_loop_abs();
    printf("host unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
