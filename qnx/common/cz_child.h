/*
 * Crumple Zone: helpers for supervised processes (talking to the guardian)
 * and the binary sample formats served by wheel_sensors.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifndef CZ_CHILD_H
#define CZ_CHILD_H

#include <stdint.h>

/* Connect to the guardian channel given in the CZ_GUARDIAN environment variable
 * ("pid:chid") and say hello. The connection stays open for the life of the
 * process: when the process dies the kernel detaches it and the guardian's
 * _NTO_CHF_DISCONNECT channel receives a pulse. Returns the coid or -1. */
int cz_guardian_connect(int proc_id);
int cz_guardian_ready(int coid, int proc_id);

#define CZ_PATH_WHEEL  "/dev/vehicle/wheel_speed"
#define CZ_PATH_PEDAL  "/dev/vehicle/pedal"
#define CZ_PATH_STEER  "/dev/vehicle/steering"

#define CZ_SENSOR_OK        0u
#define CZ_SENSOR_IMPLAUSIBLE 1u

struct cz_wheel_sample {
    uint64_t sample_ns;    /* when the hardware sampled (CLOCK_MONOTONIC) */
    uint32_t seq;
    uint32_t status;
    float speed[4];        /* m/s at the rim */
};

struct cz_pedal_sample {
    uint64_t sample_ns;
    uint32_t seq;
    uint32_t status;
    float pedal;
    float pad;
};

struct cz_steer_sample {
    uint64_t sample_ns;
    uint32_t seq;
    uint32_t status;
    float torque;          /* driver torque, Nm */
    float angle;           /* rad */
    float rate;            /* rad/s */
    float speed;           /* vehicle speed from wheel average, m/s */
};

#endif
