/*
 * Crumple Zone: vehicle model used by vehicle_sim (portable C, unit tested).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A quarter-car-per-wheel longitudinal model with a simplified Pacejka tire
 * curve, rear-wheel drive, brake torque proportional to line pressure, plus a
 * first-order steering column. It stands in for the physical car: it is not
 * part of the ECU software under test.
 */
#ifndef CZ_PLANT_H
#define CZ_PLANT_H

typedef struct {
    float v;            /* vehicle speed, m/s */
    float omega[4];     /* wheel angular speed, rad/s: FL FR RL RR */
    float mu;           /* road friction coefficient */
    float t;            /* simulation time, s */
    float pedal;        /* driver brake pedal 0..1 */
    float throttle;     /* driver throttle 0..1 */
    float driver_torque;/* driver steering torque, Nm */
    float steer_angle;  /* rad */
    float steer_rate;   /* rad/s */
} cz_plant_t;

#define CZ_WHEEL_RADIUS 0.32f
#define CZ_DRIVE_CYCLE_S 15.0f

void  cz_plant_init(cz_plant_t *p, float v0);
/* Advance by dt seconds with the given brake line pressures (bar) and steering assist torque (Nm). */
void  cz_plant_step(cz_plant_t *p, const float brake_bar[4], float assist_nm, float dt);
/* Driver model: sets pedal, throttle, friction and steering torque for the current time. */
void  cz_driver_update(cz_plant_t *p);
float cz_tire_mu(float slip);
float cz_wheel_speed(const cz_plant_t *p, int i);

#endif
