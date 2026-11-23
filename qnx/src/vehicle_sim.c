/*
 * vehicle_sim: stands in for the physical car (test rig, not ECU software).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Integrates the vehicle model at 1 kHz and exposes it as memory-mapped
 * "hardware": sensor registers that wheel_sensors samples, and actuator
 * registers that brake_ctl and steer_ctl write. Like real physics it keeps
 * running whatever happens to the software that observes it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"
#include "plant.h"

#define WHO "vehicle_sim"

/* Deterministic sensor noise: a small LCG, +/- amplitude. */
static uint32_t lcg = 12345u;
static float noise(float amp)
{
    lcg = lcg * 1664525u + 1013904223u;
    return amp * ((float)(lcg >> 8) / 8388608.0f - 1.0f);
}

int main(void)
{
    if (cz_set_sched(SCHED_FIFO, CZ_PRIO_VEHICLE_SIM) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));
    cz_pin_cpu(0);

    struct cz_hw *hw = cz_hw_create();
    if (!hw) { cz_log(WHO, "cannot create %s: %s", CZ_HW_NAME, strerror(errno)); return 1; }

    int gcoid = cz_guardian_connect(CZ_P_VEHICLE_SIM);
    if (gcoid == -1) cz_log(WHO, "guardian connect: %s (running unsupervised)", strerror(errno));

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    uint64_t release = cz_align_up(cz_now_ns() + 2000000ull, CZ_PLANT_PERIOD_NS);
    timer_t tmr;
    if (cz_oneshot_pulse_timer(coid, CZ_PRIO_VEHICLE_SIM, CZ_PULSE_TICK, &tmr) == -1 || cz_timer_arm_abs(tmr, release) == -1) {
        cz_log(WHO, "timer: %s", strerror(errno));
        return 1;
    }

    cz_plant_t plant;
    cz_plant_init(&plant, 16.7f);
    const float dt = (float)CZ_PLANT_PERIOD_NS / 1e9f;
    int ready_sent = 0;

    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) continue;
        if (p.code != CZ_PULSE_TICK) continue;
        /* Physics advances in fixed 1 ms steps on an absolute grid; if the
         * platform stalls, it catches up step by step so simulated time stays
         * equal to real time. */
        uint64_t now = cz_now_ns();
        int steps = 0;
        while (release <= now && steps < 20) { release += CZ_PLANT_PERIOD_NS; steps++; }
        cz_timer_arm_abs(tmr, release);

        float brake[4];
        for (int i = 0; i < 4; i++)
            brake[i] = cz_bits_to_f(atomic_load_explicit(&hw->brake_cmd_bits[i], memory_order_acquire));
        float assist = cz_bits_to_f(atomic_load_explicit(&hw->steer_cmd_bits, memory_order_acquire));

        for (int k = 0; k < steps; k++) {
            cz_driver_update(&plant);
            cz_plant_step(&plant, brake, assist, dt);
        }

        cz_seq_write_begin(&hw->sseq);
        hw->sample_ns = cz_now_ns();
        hw->sample_count++;
        for (int i = 0; i < 4; i++) {
            float v = cz_wheel_speed(&plant, i) + noise(0.01f);
            hw->wheel_speed[i] = v > 0.0f ? v : 0.0f;
        }
        hw->pedal = plant.pedal;
        hw->throttle = plant.throttle;
        hw->steer_torque = plant.driver_torque + noise(0.02f);
        hw->steer_angle = plant.steer_angle;
        hw->steer_rate = plant.steer_rate;
        hw->true_speed = plant.v;
        hw->mu = plant.mu;
        hw->sim_time = plant.t;
        cz_seq_write_end(&hw->sseq);

        if (!ready_sent && gcoid != -1) {
            cz_guardian_ready(gcoid, CZ_P_VEHICLE_SIM);
            cz_log(WHO, "physics running at 1 kHz on cpu %d", cz_current_cpu());
            ready_sent = 1;
        }
    }
    return 0;
}
