/*
 * steer_ctl: 500 Hz electric power steering assist loop.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Same real-time structure as brake_ctl: SCHED_FIFO, pinned to its own CPU,
 * high-resolution periodic timer pulse, TimerTimeout-bounded reads from the
 * sensor resource manager. When the torque sensor goes stale the assist fades
 * to zero (the driver keeps mechanical steering) and returns after three
 * consecutive good samples.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>

#include "control.h"
#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"
#include "stats.h"

#define WHO "steer_ctl"
#define SENSOR_TIMEOUT_NS   400000ull     /* bound on each driver round trip (20% of a steering period) */
#define STALE_AGE_NS    20000000ull
#define LINK_PULSE_RECONNECT (_PULSE_CODE_MINAVAIL + 10)

static struct cz_bus *bus;
static struct cz_hw *hw;
static _Atomic int fd_steer = -1;
static _Atomic int loop_iterations = 0;
static int link_coid = -1;
static int guardian_coid = -1;

#define LINK_PULSE_PATHSPACE (_PULSE_CODE_MINAVAIL + 11)

static void *link_thread(void *arg)
{
    int chid = (int)(intptr_t)arg;
    cz_set_sched(SCHED_FIFO, CZ_PRIO_STEER - 5);
    struct sigevent pev;
    SIGEV_PULSE_INIT(&pev, link_coid, CZ_PRIO_STEER - 5, LINK_PULSE_PATHSPACE, 0);
    if (MsgRegisterEvent(&pev, SYSMGR_COID) == -1 || procmgr_event_notify_add(PROCMGR_EVENT_PATHSPACE, &pev) == -1)
        cz_log(WHO, "pathspace notification unavailable (%s), polling", strerror(errno));
    int ready_sent = 0;
    for (;;) {
        if (!ready_sent && guardian_coid != -1 && atomic_load(&loop_iterations) >= 10) {
            cz_guardian_ready(guardian_coid, CZ_P_STEER);
            ready_sent = 1;
        }
        int need = atomic_load(&fd_steer) < 0;
        if (need) {
            int fd = open(CZ_PATH_STEER, O_RDONLY);
            if (fd >= 0) atomic_store(&fd_steer, fd);
        }
        struct _pulse p;
        uint64_t to = !ready_sent ? 1000000ull : (need ? 5000000ull : 50000000ull);
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == 0 && p.code == LINK_PULSE_RECONNECT) {
            if (p.value.sival_int >= 0) close(p.value.sival_int);
        }
    }
    return NULL;
}

static int bounded_read(void *buf, size_t len)
{
    int fd = atomic_load(&fd_steer);
    if (fd < 0) return CZ_SAFE_NO_DRIVER;
    uint64_t to = SENSOR_TIMEOUT_NS;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &to, NULL);
    ssize_t n = read(fd, buf, len);
    if (n == (ssize_t)len) return 0;
    int reason = (n == -1 && errno == ETIMEDOUT) ? CZ_SAFE_TIMEOUT : CZ_SAFE_READ_ERROR;
    int old = atomic_exchange(&fd_steer, -1);
    if (old >= 0) MsgSendPulse(link_coid, CZ_PRIO_STEER - 5, LINK_PULSE_RECONNECT, old);
    return reason;
}

int main(void)
{
    int ncpu = cz_num_cpus();
    int cpu = ncpu >= 3 ? ncpu - 2 : 0;
    if (cz_set_sched(SCHED_FIFO, CZ_PRIO_STEER) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));
    if (cz_pin_cpu(cpu) == -1)
        cz_log(WHO, "runmask cpu %d: %s", cpu, strerror(errno));

    while (!(bus = cz_bus_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    while (!(hw = cz_hw_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    guardian_coid = cz_guardian_connect(CZ_P_STEER);

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int link_chid = ChannelCreate(_NTO_CHF_PRIVATE);
    link_coid = ConnectAttach(0, 0, link_chid, _NTO_SIDE_CHANNEL, 0);
    atomic_store(&fd_steer, open(CZ_PATH_STEER, O_RDONLY));
    pthread_t lt;
    pthread_create(&lt, NULL, link_thread, (void *)(intptr_t)link_chid);

    cz_steer_params_t prm;
    cz_steer_default_params(&prm);
    static cz_stats_t win_lat;
    cz_stats_reset(&win_lat);
    cz_period_t per = { .period_ns = CZ_STEER_PERIOD_NS };
    per.start_ns = cz_align_up(cz_now_ns() + 5000000ull, CZ_STEER_PERIOD_NS) + 250000ull; /* after the plant, before the brake */
    timer_t tmr;
    if (cz_oneshot_pulse_timer(coid, CZ_PRIO_STEER, CZ_PULSE_TICK, &tmr) == -1 || cz_timer_arm_abs(tmr, per.start_ns) == -1) {
        cz_log(WHO, "timer: %s", strerror(errno));
        return 1;
    }
    uint64_t release = per.start_ns;   /* the release the next wake-up serves */
    uint64_t idx = 0;

    struct cz_loop *L = &bus->steer;
    uint64_t reset_gen = atomic_load(&bus->reset_req);
    uint64_t prev_wake = 0, iterations = 0, off_cpu = 0;
    uint64_t misses_total = L->st.misses_total, misses_win = 0, overruns = L->st.overruns_total;
    uint64_t stale_total = L->st.stale_total, safe_entries = L->st.safe_entries;
    uint32_t lat_max_total = L->st.lat_max_total_ns, lat_max_win = 0, jit_max_win = 0, exec_max_win = 0, resp_max_win = 0;
    int mode = CZ_MODE_NORMAL, good = 0;
    float assist = 0.0f, torque = 0.0f, angle = 0.0f;
    uint64_t safe_enter_ns = 0, last_good_ns = 0, last_miss_ev_ns = 0;
    const float dt = (float)CZ_STEER_PERIOD_NS / 1e9f;

    cz_log(WHO, "500 Hz loop, SCHED_FIFO %d, cpu %d", CZ_PRIO_STEER, cpu);

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

        uint64_t rq = atomic_load_explicit(&bus->reset_req, memory_order_relaxed);
        if (rq != reset_gen) {
            reset_gen = rq;
            cz_stats_reset(&win_lat);
            misses_win = 0; lat_max_win = 0; jit_max_win = 0; exec_max_win = 0; resp_max_win = 0;
        }

        struct cz_steer_sample ss;
        int reason = bounded_read(&ss, sizeof(ss));
        uint64_t now = cz_now_ns();
        uint32_t io = (uint32_t)(now - wake);
        if ((int)SchedGetCpuNum() != cpu || wake_cpu != cpu) off_cpu++;
        if (!reason && (ss.status != CZ_SENSOR_OK || now - ss.sample_ns > STALE_AGE_NS)) reason = CZ_SAFE_STALE;
        int prev_mode = mode;
        if (!reason) {
            last_good_ns = ss.sample_ns;
            torque = ss.torque;
            angle = ss.angle;
            if (mode != CZ_MODE_NORMAL) {
                mode = CZ_MODE_RESUMING;
                if (++good >= 3) { mode = CZ_MODE_NORMAL; good = 0; }
            }
            float target = cz_steer_assist(&prm, ss.torque, ss.speed, ss.rate);
            /* Blend back in with a rate limit after a dropout, no torque step. */
            float rate = (prev_mode == CZ_MODE_NORMAL) ? 400.0f : prm.fade_rate;
            float step = rate * dt;
            assist = target > assist + step ? assist + step : (target < assist - step ? assist - step : target);
        } else {
            stale_total++;
            flags |= CZ_SF_STALE;
            good = 0;
            mode = CZ_MODE_SAFE_HOLD;
            float step = prm.fade_rate * dt;     /* fade assist to zero */
            assist = assist > step ? assist - step : (assist < -step ? assist + step : 0.0f);
        }
        atomic_store_explicit(&hw->steer_cmd_bits, cz_f_to_bits(assist), memory_order_release);
        atomic_store_explicit(&hw->steer_cmd_ns, now, memory_order_release);

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
        uint32_t exec = (uint32_t)(done - wake), resp = (uint32_t)(done - release);
        if (cz_deadline_missed(&per, idx, done)) {
            /* Skipped releases were already counted above; this one is late too. */
            flags |= CZ_SF_MISS;
            misses_total++;
            misses_win++;
            if (done - last_miss_ev_ns > 50000000ull) {
                last_miss_ev_ns = done;
                cz_emit(bus, CZ_EVS_STEER, CZ_EV_DEADLINE_MISS, CZ_P_STEER, getpid(), 0, (int64_t)resp,
                        (int64_t)misses_total, "steer deadline");
            }
        }
        if (mode != CZ_MODE_NORMAL) flags |= CZ_SF_SAFE;
        if (prev_mode == CZ_MODE_NORMAL && mode != CZ_MODE_NORMAL) {
            safe_entries++;
            safe_enter_ns = wake;
            cz_emit(bus, CZ_EVS_STEER, CZ_EV_SAFE_ENTER, CZ_P_STEER, getpid(), reason,
                    last_good_ns ? (int64_t)(now - last_good_ns) : -1, (int64_t)idx, "assist fading");
            cz_trace_mark("CZ steer SAFE_ENTER reason=%d", reason);
        } else if (prev_mode != CZ_MODE_NORMAL && mode == CZ_MODE_NORMAL) {
            cz_emit(bus, CZ_EVS_STEER, CZ_EV_SAFE_EXIT, CZ_P_STEER, getpid(), 0,
                    (int64_t)(wake - safe_enter_ns), (int64_t)idx, "assist restored");
        }

        cz_stats_add(&win_lat, lat);
        uint32_t ulat = lat > 0 ? (uint32_t)lat : 0, ajit = (uint32_t)(jit < 0 ? -jit : jit);
        if (ulat > lat_max_win) lat_max_win = ulat;
        if (ulat > lat_max_total) lat_max_total = ulat;
        if (prev_wake && ajit > jit_max_win) jit_max_win = ajit;
        if (exec > exec_max_win) exec_max_win = exec;
        if (resp > resp_max_win) resp_max_win = resp;

        struct cz_sample smp = { .n = idx, .t_ns = wake, .lat_ns = ulat, .exec_ns = exec, .io_ns = io,
                                 .jitter_ns = jit, .flags = flags, .mode = (uint16_t)mode };
        cz_loop_push(L, &smp);

        cz_seq_write_begin(&L->seq);
        L->st.start_ns = per.start_ns;
        L->st.period_ns = per.period_ns;
        L->st.cpu = cpu;
        L->st.prio = CZ_PRIO_STEER;
        L->st.mode = (uint32_t)mode;
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
        L->st.steer_torque = torque;
        L->st.assist_torque = assist;
        L->st.steer_angle = angle;
        cz_seq_write_end(&L->seq);

        if (iterations <= 10) atomic_store(&loop_iterations, (int)iterations);
        idx = next_idx;
        release = next;
        prev_wake = wake;
    }
    return 0;
}
