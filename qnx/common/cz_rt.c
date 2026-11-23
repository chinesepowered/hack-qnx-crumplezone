/*
 * Crumple Zone: QNX real-time helpers.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "cz_rt.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/syspage.h>
#include <sys/trace.h>

/* Timestamps come from ClockCycles() (the TSC on x86_64), converted with the
 * syspage cycles_per_sec and anchored once to CLOCK_MONOTONIC. Reading the
 * cycle counter is a single instruction, so timestamping costs nothing inside
 * the control loops. The syspage flag QTIME_FLAG_GLOBAL_CLOCKCYCLES says the
 * counter is synchronised across CPUs; otherwise we fall back to the kernel
 * clock. */
static uint64_t cc0, ns0, cps;
static int use_cc = -1;

static uint64_t mono_ns(void)
{
    uint64_t t;
    if (clock_gettime_ns(CLOCK_MONOTONIC, &t) == 0) return t;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t cz_now_ns(void)
{
    if (use_cc < 0) {
        struct qtime_entry *qt = SYSPAGE_ENTRY(qtime);
        cps = qt->cycles_per_sec;
        use_cc = (cps != 0 && (qt->flags & QTIME_FLAG_GLOBAL_CLOCKCYCLES)) ? 1 : 0;
        cc0 = ClockCycles();
        ns0 = mono_ns();
    }
    if (!use_cc) return mono_ns();
    uint64_t d = ClockCycles() - cc0;
    return ns0 + (d / cps) * 1000000000ull + ((d % cps) * 1000000000ull) / cps;
}

int cz_clock_is_cycles(void)
{
    (void)cz_now_ns();
    return use_cc == 1;
}

int cz_set_sched(int policy, int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc != 0) { errno = rc; return -1; }
    return 0;
}

int cz_num_cpus(void)
{
    return _syspage_ptr->num_cpu;
}

int cz_current_cpu(void)
{
    return (int)SchedGetCpuNum();
}

int cz_pin_cpu(int cpu)
{
    if (cpu < 0) return 0;
    int ncpu = _syspage_ptr->num_cpu;
    if (cpu >= ncpu) cpu = ncpu - 1;
    int rsize = RMSK_SIZE(ncpu);
    /* struct _thread_runmask is variable sized: int size; unsigned runmask[size]; unsigned inherit[size]; */
    unsigned buf[1 + 2 * 8];
    if (rsize > 8) { errno = EINVAL; return -1; }
    memset(buf, 0, sizeof(buf));
    int *rsizep = (int *)buf;
    unsigned *rmaskp = (unsigned *)(rsizep + 1);
    unsigned *inheritp = rmaskp + rsize;
    *rsizep = rsize;
    RMSK_SET(cpu, rmaskp);
    RMSK_SET(cpu, inheritp);
    if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, buf) == -1) return -1;
    return 0;
}

uint64_t cz_align_up(uint64_t t, uint64_t period)
{
    return ((t + period - 1) / period) * period;
}

int cz_periodic_pulse_timer(int coid, int pulse_prio, int code, uint64_t start_abs_ns,
                            uint64_t period_ns, timer_t *out)
{
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, pulse_prio, code, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) return -1;

    /* Request a high-resolution timer: tolerance below the tick size. */
    struct itimerspec tol;
    memset(&tol, 0, sizeof(tol));
    tol.it_value.tv_nsec = 1;
    if (timer_settime(tid, TIMER_TOLERANCE, &tol, NULL) == -1) {
        int e = errno;
        cz_log("rt", "timer tolerance not granted (%s), using tick-aligned timer", strerror(e));
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(start_abs_ns / 1000000000ull);
    its.it_value.tv_nsec = (long)(start_abs_ns % 1000000000ull);
    its.it_interval.tv_sec = (time_t)(period_ns / 1000000000ull);
    its.it_interval.tv_nsec = (long)(period_ns % 1000000000ull);
    if (timer_settime(tid, TIMER_ABSTIME, &its, NULL) == -1) return -1;
    if (out) *out = tid;
    return 0;
}

int cz_oneshot_pulse_timer(int coid, int pulse_prio, int code, timer_t *out)
{
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, pulse_prio, code, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) return -1;
    struct itimerspec tol;
    memset(&tol, 0, sizeof(tol));
    tol.it_value.tv_nsec = 1;
    if (timer_settime(tid, TIMER_TOLERANCE, &tol, NULL) == -1)
        cz_log("rt", "timer tolerance not granted (%s), using tick-aligned timer", strerror(errno));
    *out = tid;
    return 0;
}

int cz_timer_arm_abs(timer_t tid, uint64_t abs_ns)
{
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(abs_ns / 1000000000ull);
    its.it_value.tv_nsec = (long)(abs_ns % 1000000000ull);
    return timer_settime(tid, TIMER_ABSTIME, &its, NULL);
}

void cz_trace_mark(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Fails harmlessly (ENOTSUP/EPERM) when tracing is not active. */
    (void)TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, 100, buf);
}

void cz_log(const char *who, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uint64_t t = cz_now_ns();
    fprintf(stdout, "[%7.3f] %-13s %s\n", (double)(t / 1000000ull) / 1000.0, who, buf);
    fflush(stdout);
}
