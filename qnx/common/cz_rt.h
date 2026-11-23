/*
 * Crumple Zone: QNX real-time helpers.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifndef CZ_RT_H
#define CZ_RT_H

#include <stdint.h>
#include <time.h>

/* Monotonic nanoseconds from ClockCycles(), anchored to CLOCK_MONOTONIC. */
uint64_t cz_now_ns(void);
int cz_clock_is_cycles(void);

/* Set the calling thread's policy (SCHED_FIFO/SCHED_RR) and priority. */
int cz_set_sched(int policy, int prio);

/* Pin the calling thread, and every thread it creates, to one CPU using
 * ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT). cpu < 0 leaves it alone. */
int cz_pin_cpu(int cpu);
int cz_num_cpus(void);
int cz_current_cpu(void);

/* Create a periodic high-resolution timer that delivers a pulse with the given
 * code to coid. The first expiry is at start_abs_ns (CLOCK_MONOTONIC), then
 * every period_ns. High resolution means a timer tolerance below the clock
 * tick (needs PROCMGR_AID_HIGH_RESOLUTION_TIMER), so expiry is not rounded to
 * the 1 ms tick. Returns 0 or -1 with errno set. */
int cz_periodic_pulse_timer(int coid, int pulse_prio, int code, uint64_t start_abs_ns,
                            uint64_t period_ns, timer_t *out);

/* One-shot high-resolution timer delivering a pulse; re-armed with an absolute
 * release time every period, so releases stay on a fixed grid (no drift, and
 * lateness is always measured against the true schedule). */
int cz_oneshot_pulse_timer(int coid, int pulse_prio, int code, timer_t *out);
int cz_timer_arm_abs(timer_t tid, uint64_t abs_ns);

/* Align t up to the next multiple of period. */
uint64_t cz_align_up(uint64_t t, uint64_t period);

/* Insert a printf-style user string event into the kernel trace (no-op when
 * no trace is running). Shows up in traceprinter output as USREVENT. */
void cz_trace_mark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Log a line prefixed with the process name to stdout (the QNX console). */
void cz_log(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
