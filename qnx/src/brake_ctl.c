/*
 * brake_ctl: 1 kHz brake-by-wire control loop (highest ECU criticality).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Real-time structure:
 *   - SCHED_FIFO priority 60, pinned to the last CPU with
 *     ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT)
 *   - released by a high-resolution periodic timer that delivers a pulse
 *     (timer_create + TIMER_TOLERANCE + TIMER_ABSTIME), received with
 *     MsgReceivePulse() on a private channel
 *   - reads wheel speed and pedal from the wheel_sensors resource manager;
 *     every read() is bounded by TimerTimeout(_NTO_TIMEOUT_SEND |
 *     _NTO_TIMEOUT_REPLY) so a hung driver cannot block the loop
 *   - never blocks on a lower-criticality process: it publishes to the bus
 *     (single-writer seqlock) and rings the cluster with MsgSendPulse(),
 *     which is asynchronous
 * Stale or missing sensor data is detected in the same period and handled by
 * the SAFE_HOLD / SAFE_STOP logic in control.c. Reconnecting to a restarted
 * driver is done by a helper thread so open() never runs in the loop.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>

#include "control.h"
#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"
#include "stats.h"

#define WHO "brake_ctl"
#define SENSOR_TIMEOUT_NS   200000ull     /* bound on each driver round trip */
#define STALE_AGE_NS      10000000ull     /* sample older than ten periods is stale (driver alive but frozen) */
#define LINK_PULSE_RECONNECT (_PULSE_CODE_MINAVAIL + 10)
#define PEDAL_DIVIDER 4                   /* pedal sampled at 1 kHz / 4 */

static struct cz_bus *bus;
static struct cz_hw *hw;
static _Atomic int fd_wheel = -1;
static _Atomic int fd_pedal = -1;
static _Atomic int cluster_coid = -1;
static _Atomic int loop_iterations = 0;
static int link_coid = -1;
static int guardian_coid = -1;

/* Helper thread: (re)opens the sensor devices and the cluster connection.
 * Runs below the loop's priority, on the same CPU, only when needed. */
#define LINK_PULSE_PATHSPACE (_PULSE_CODE_MINAVAIL + 11)

static void *link_thread(void *arg)
{
    int chid = (int)(intptr_t)arg;
    cz_set_sched(SCHED_FIFO, CZ_PRIO_BRAKE - 5);
    /* Wake up whenever the pathname space changes (a resource manager attached
     * or detached a path): a restarted sensor driver is picked up the moment it
     * registers /dev/vehicle/..., without polling open() in a tight loop. */
    struct sigevent pev;
    SIGEV_PULSE_INIT(&pev, link_coid, CZ_PRIO_BRAKE - 5, LINK_PULSE_PATHSPACE, 0);
    if (MsgRegisterEvent(&pev, SYSMGR_COID) == -1 || procmgr_event_notify_add(PROCMGR_EVENT_PATHSPACE, &pev) == -1)
        cz_log(WHO, "pathspace notification unavailable (%s), polling", strerror(errno));
    int ready_sent = 0;
    for (;;) {
        /* Report READY to the guardian once the loop has run 10 periods. The
         * MsgSend happens here so the control loop itself never blocks. */
        if (!ready_sent && guardian_coid != -1 && atomic_load(&loop_iterations) >= 10) {
            cz_guardian_ready(guardian_coid, CZ_P_BRAKE);
            ready_sent = 1;
        }
        int need = atomic_load(&fd_wheel) < 0 || atomic_load(&fd_pedal) < 0 || atomic_load(&cluster_coid) < 0;
        if (need) {
            if (atomic_load(&fd_wheel) < 0) {
                int fd = open(CZ_PATH_WHEEL, O_RDONLY);
                if (fd >= 0) atomic_store(&fd_wheel, fd);
            }
            if (atomic_load(&fd_pedal) < 0) {
                int fd = open(CZ_PATH_PEDAL, O_RDONLY);
                if (fd >= 0) atomic_store(&fd_pedal, fd);
            }
            if (atomic_load(&cluster_coid) < 0) {
                int c = name_open(CZ_CLUSTER_NAME, 0);
                if (c >= 0) atomic_store(&cluster_coid, c);
            }
        }
        /* Wait for a reconnect request (pulse value = fd to close) or poll every 1 ms. */
        struct _pulse p;
        uint64_t to = !ready_sent ? 1000000ull : (need ? 5000000ull : 50000000ull);
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == 0 && p.code == LINK_PULSE_RECONNECT) {
            int old = p.value.sival_int;
            if (old >= 0) close(old);
        }
    }
    return NULL;
}

static void drop_fd(_Atomic int *slot)
{
    int old = atomic_exchange(slot, -1);
    if (old >= 0) MsgSendPulse(link_coid, CZ_PRIO_BRAKE - 5, LINK_PULSE_RECONNECT, old);
}

/* Bounded read of one fixed-size sample. Returns 0 or a cz_safe_reason. */
static int bounded_read(_Atomic int *slot, void *buf, size_t len)
{
    int fd = atomic_load(slot);
    if (fd < 0) return CZ_SAFE_NO_DRIVER;
    uint64_t to = SENSOR_TIMEOUT_NS;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &to, NULL);
    ssize_t n = read(fd, buf, len);
    if (n == (ssize_t)len) return 0;
    int reason = (n == -1 && errno == ETIMEDOUT) ? CZ_SAFE_TIMEOUT : CZ_SAFE_READ_ERROR;
    drop_fd(slot);
    return reason;
}

int main(void)
{
    int ncpu = cz_num_cpus();
    int cpu = ncpu - 1;
    if (cz_set_sched(SCHED_FIFO, CZ_PRIO_BRAKE) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));
    if (cz_pin_cpu(cpu) == -1)
        cz_log(WHO, "runmask cpu %d: %s", cpu, strerror(errno));

    while (!(bus = cz_bus_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    while (!(hw = cz_hw_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    guardian_coid = cz_guardian_connect(CZ_P_BRAKE);

    /* Private channel for the timer pulse, a second one for the link thread. */
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int link_chid = ChannelCreate(_NTO_CHF_PRIVATE);
    link_coid = ConnectAttach(0, 0, link_chid, _NTO_SIDE_CHANNEL, 0);

    atomic_store(&fd_wheel, open(CZ_PATH_WHEEL, O_RDONLY));
    atomic_store(&fd_pedal, open(CZ_PATH_PEDAL, O_RDONLY));
    atomic_store(&cluster_coid, name_open(CZ_CLUSTER_NAME, 0));
    pthread_t lt;
    pthread_create(&lt, NULL, link_thread, (void *)(intptr_t)link_chid);

    cz_brake_params_t prm;
    cz_brake_state_t st;
    cz_brake_default_params(&prm);
    cz_brake_init(&st);

    static cz_stats_t win_lat;
    cz_stats_reset(&win_lat);
    cz_period_t per = { .period_ns = CZ_BRAKE_PERIOD_NS };
    /* Release half a period after the plant samples, so data is ~0.5 ms old. */
    per.start_ns = cz_align_up(cz_now_ns() + 5000000ull, CZ_BRAKE_PERIOD_NS) + CZ_BRAKE_PERIOD_NS / 2;
    timer_t tmr;
    if (cz_oneshot_pulse_timer(coid, CZ_PRIO_BRAKE, CZ_PULSE_TICK, &tmr) == -1 || cz_timer_arm_abs(tmr, per.start_ns) == -1) {
        cz_log(WHO, "timer: %s", strerror(errno));
        return 1;
    }
    uint64_t release = per.start_ns;   /* the release the next wake-up serves */
    uint64_t idx = 0;

    struct cz_loop *L = &bus->brake;
    uint64_t reset_gen = atomic_load(&bus->reset_req);
    uint64_t prev_wake = 0, iterations = 0, off_cpu = 0;
    uint64_t misses_total = L->st.misses_total, misses_win = 0, overruns = L->st.overruns_total;
    uint64_t stale_total = L->st.stale_total, safe_entries = L->st.safe_entries;
    uint32_t lat_max_total = L->st.lat_max_total_ns, lat_max_win = 0, jit_max_win = 0;
    uint32_t exec_max_win = 0, resp_max_win = 0;
    uint64_t safe_enter_ns = 0, last_good_ns = 0, last_miss_ev_ns = 0;
    struct cz_pedal_sample pedal_sample;
    memset(&pedal_sample, 0, sizeof(pedal_sample));
    int loud_safe = 0;
    cz_log(WHO, "1 kHz loop, SCHED_FIFO %d, cpu %d, high-resolution timer", CZ_PRIO_BRAKE, cpu);

    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) continue;
        if (p.code != CZ_PULSE_TICK) continue;
        uint64_t wake = cz_now_ns();
        int wake_cpu = (int)SchedGetCpuNum();

        /* Each wake-up serves the release we armed the one-shot timer for, so
         * latency is always measured against the fixed release grid and any
         * lateness, even beyond a period, shows up in full. */
        uint16_t flags = 0;
        int64_t lat = (int64_t)wake - (int64_t)release;
        int32_t jit = prev_wake ? (int32_t)((int64_t)(wake - prev_wake) - (int64_t)per.period_ns) : 0;

        /* Windowed statistics reset requested by the operator. */
        uint64_t rq = atomic_load_explicit(&bus->reset_req, memory_order_relaxed);
        if (rq != reset_gen) {
            reset_gen = rq;
            cz_stats_reset(&win_lat);
            misses_win = 0; lat_max_win = 0; jit_max_win = 0; exec_max_win = 0; resp_max_win = 0;
        }

        /* ---- sensing (bounded) ---- */
        struct cz_wheel_sample ws;
        struct cz_pedal_sample ps;
        cz_brake_input_t in;
        memset(&in, 0, sizeof(in));
        /* Wheel speed every period (1 kHz); the pedal is a human input and is
         * sampled every fourth period (250 Hz), so most periods need only one
         * resource manager round trip. */
        int reason = bounded_read(&fd_wheel, &ws, sizeof(ws));
        if (!reason && (iterations % PEDAL_DIVIDER) == 0) {
            reason = bounded_read(&fd_pedal, &pedal_sample, sizeof(pedal_sample));
        }
        ps = pedal_sample;
        uint64_t now = cz_now_ns();
        uint32_t io = (uint32_t)(now - wake);
        if ((int)SchedGetCpuNum() != cpu || wake_cpu != cpu) off_cpu++;
        if (!reason) {
            if (ws.status != CZ_SENSOR_OK || ps.status != CZ_SENSOR_OK) reason = CZ_SAFE_STALE;
            else if (now - ws.sample_ns > STALE_AGE_NS || now - ps.sample_ns > STALE_AGE_NS + 4 * CZ_BRAKE_PERIOD_NS)
                reason = CZ_SAFE_STALE;
        }
        in.valid = reason == 0;
        if (in.valid) {
            memcpy(in.wheel_speed, ws.speed, sizeof(in.wheel_speed));
            in.pedal = ps.pedal;
            last_good_ns = ws.sample_ns;
        } else {
            stale_total++;
            flags |= CZ_SF_STALE;
        }

        /* ---- control ---- */
        int prev_mode = st.mode;
        cz_brake_step(&prm, &st, &in, (float)per.period_ns / 1e9f);

        /* ---- actuation: write the valve registers ---- */
        for (int i = 0; i < 4; i++)
            atomic_store_explicit(&hw->brake_cmd_bits[i], cz_f_to_bits(st.pressure[i]), memory_order_release);
        atomic_store_explicit(&hw->brake_cmd_ns, now, memory_order_release);

        uint64_t done = cz_now_ns();
        /* Arm the next release. Releases that passed entirely while this one ran
         * are skipped (and counted as misses); a release only slightly in the
         * past fires immediately, so the loop catches up instead of idling. */
        uint64_t next = release + per.period_ns, next_idx = idx + 1;
        while (next + per.period_ns <= done) {
            next += per.period_ns;
            next_idx++;
            overruns++;
            misses_total++;
            misses_win++;
            flags |= CZ_SF_OVERRUN;
        }
        cz_timer_arm_abs(tmr, next);
        uint32_t exec = (uint32_t)(done - wake);
        uint32_t resp = (uint32_t)(done - release);
        if (cz_deadline_missed(&per, idx, done)) {
            /* Skipped releases were already counted above; this one is late too. */
            flags |= CZ_SF_MISS;
            misses_total++;
            misses_win++;
            /* At most one event per 50 ms; v1 carries the running total. */
            if (done - last_miss_ev_ns > 50000000ull) {
                last_miss_ev_ns = done;
                cz_emit(bus, CZ_EVS_BRAKE, CZ_EV_DEADLINE_MISS, CZ_P_BRAKE, getpid(), 0, (int64_t)resp,
                        (int64_t)misses_total, "brake deadline");
                cz_trace_mark("CZ brake deadline miss resp=%u ns", resp);
            }
        }
        if (st.mode != CZ_MODE_NORMAL) flags |= CZ_SF_SAFE;

        /* ---- mode transitions are safety events ---- */
        if (prev_mode == CZ_MODE_NORMAL && st.mode != CZ_MODE_NORMAL) {
            safe_entries++;
            safe_enter_ns = wake;
            int64_t age = last_good_ns ? (int64_t)(now - last_good_ns) : -1;
            cz_emit(bus, CZ_EVS_BRAKE, CZ_EV_SAFE_ENTER, CZ_P_BRAKE, getpid(), reason, age, (int64_t)idx, cz_mode_name(st.mode));
            cz_trace_mark("CZ brake SAFE_ENTER reason=%d", reason);
            if (reason != CZ_SAFE_STALE) {
                loud_safe = 1;
                cz_log(WHO, "sensor link lost (%s): %s, holding %.1f bar", reason == CZ_SAFE_TIMEOUT ? "timeout" :
                       (reason == CZ_SAFE_NO_DRIVER ? "no driver" : "read error"), cz_mode_name(st.mode), st.pressure[0]);
            }
        } else if (prev_mode != CZ_MODE_NORMAL && st.mode == CZ_MODE_NORMAL) {
            int64_t dur = (int64_t)(wake - safe_enter_ns);
            cz_emit(bus, CZ_EVS_BRAKE, CZ_EV_SAFE_EXIT, CZ_P_BRAKE, getpid(), 0, dur, (int64_t)idx, "normal control");
            cz_trace_mark("CZ brake SAFE_EXIT after %lld ns", (long long)dur);
            if (loud_safe) cz_log(WHO, "normal control resumed after %.3f ms", (double)dur / 1e6);
            loud_safe = 0;
        } else if (prev_mode != st.mode && st.mode == CZ_MODE_SAFE_STOP) {
            cz_emit(bus, CZ_EVS_BRAKE, CZ_EV_INFO, CZ_P_BRAKE, getpid(), st.mode, 0, (int64_t)idx, "SAFE_STOP");
        }

        /* ---- statistics and publication ---- */
        cz_stats_add(&win_lat, lat);
        uint32_t ulat = lat > 0 ? (uint32_t)lat : 0;
        uint32_t ajit = (uint32_t)(jit < 0 ? -jit : jit);
        if (ulat > lat_max_win) lat_max_win = ulat;
        if (ulat > lat_max_total) lat_max_total = ulat;
        if (prev_wake && ajit > jit_max_win) jit_max_win = ajit;
        if (exec > exec_max_win) exec_max_win = exec;
        if (resp > resp_max_win) resp_max_win = resp;

        struct cz_sample smp = { .n = idx, .t_ns = wake, .lat_ns = ulat, .exec_ns = exec, .io_ns = io,
                                 .jitter_ns = jit, .flags = flags, .mode = (uint16_t)st.mode };
        cz_loop_push(L, &smp);

        cz_seq_write_begin(&L->seq);
        L->st.start_ns = per.start_ns;
        L->st.period_ns = per.period_ns;
        L->st.cpu = cpu;
        L->st.prio = CZ_PRIO_BRAKE;
        L->st.mode = (uint32_t)st.mode;
        L->st.iterations = ++iterations;
        L->st.misses_total = misses_total;
        L->st.misses_win = misses_win;
        L->st.overruns_total = overruns;
        L->st.lat_last_ns = ulat;
        L->st.lat_max_win_ns = lat_max_win;
        L->st.lat_max_total_ns = lat_max_total;
        L->st.jit_max_win_ns = jit_max_win;
        L->st.exec_max_win_ns = exec_max_win;
        L->st.resp_max_win_ns = resp_max_win;
        L->st.lat_mean_win_ns = cz_stats_mean(&win_lat);
        L->st.win_count = win_lat.n;
        L->st.stale_total = stale_total;
        L->st.off_cpu = off_cpu;
        L->st.safe_entries = safe_entries;
        L->st.reset_gen = reset_gen;
        L->st.publish_ns = done;
        L->st.vref = st.vref;
        L->st.speed_kmh = st.vref * 3.6f;
        L->st.pedal = in.valid ? in.pedal : st.last_pedal;
        memcpy(L->st.pressure, st.pressure, sizeof(st.pressure));
        memcpy(L->st.slip, st.slip, sizeof(st.slip));
        L->st.abs_mask = st.abs_mask;
        L->st.true_speed_kmh = hw->true_speed * 3.6f;
        L->st.mu = hw->mu;
        cz_seq_write_end(&L->seq);

        /* Doorbell to the instrument cluster every 10 ms: asynchronous, never blocks. */
        if ((iterations % 10) == 0) {
            int c = atomic_load(&cluster_coid);
            if (c >= 0 && MsgSendPulse(c, CZ_PRIO_BRAKE_PULSE, CZ_PULSE_BRAKE, (int)(uint32_t)(done / 1000ull)) == -1) {
                if (errno == EBADF || errno == ESRCH) {
                    int old = atomic_exchange(&cluster_coid, -1);
                    if (old >= 0) name_close(old);
                }
            }
        }

        if (iterations <= 10) atomic_store(&loop_iterations, (int)iterations);
        idx = next_idx;
        release = next;
        prev_wake = wake;
    }
    return 0;
}
