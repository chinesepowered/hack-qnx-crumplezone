/*
 * infotainment: media and navigation, lowest criticality, with chaos modes.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Sandboxed by the guardian: spawned as uid/gid 1500 (POSIX_SPAWN_SETCRED),
 * priority 8, and a 192 MB address-space limit imposed from outside with
 * prlimit(RLIMIT_AS). It has no write access to the shared bus or the
 * hardware registers. Its only links to the rest of the system are message
 * passing connections: one to the guardian, one to the cluster.
 *
 * Commands arrive by "long poll": a command thread is reply-blocked in
 * MsgSend(CZ_MSG_CMD_WAIT) on the guardian, which replies when an operator
 * injects a fault. The faults are deliberately bad behaviour:
 *   crash  dereference NULL (SIGSEGV)
 *   storm  one spinning thread pinned to every CPU, at infotainment priority
 *   leak   allocate and touch 4 MB every 40 ms until malloc fails, then abort()
 *   flood  four threads MsgSend 1 KB media updates to the cluster nonstop
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cz.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"

#define WHO "infotainment"

static int gcoid = -1;
static int ccoid = -1;
static _Atomic uint32_t cur_mode = CZ_FAULT_NONE;
static _Atomic uint32_t alloc_mb = 0;
static _Atomic uint64_t flood_sent = 0;
static _Atomic uint64_t tracks = 0;
static _Atomic uint64_t run_until_ns = 0;
static struct cz_msg_media flood_msg;   /* payload shared read-only by the flood threads */

static void sleep_ns(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
}

static void heartbeat(void)
{
    struct cz_msg_heartbeat hb;
    memset(&hb, 0, sizeof(hb));
    hb.hdr.type = CZ_MSG_HEARTBEAT;
    hb.hdr.proc = CZ_P_INFOTAINMENT;
    hb.mode = atomic_load(&cur_mode);
    hb.alloc_mb = atomic_load(&alloc_mb);
    hb.flood_sent = atomic_load(&flood_sent);
    hb.media_tracks = atomic_load(&tracks);
    struct cz_reply r;
    MsgSend(gcoid, &hb, sizeof(hb), &r, sizeof(r));
}

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    for (;;) {
        heartbeat();
        sleep_ns(100000000ull);
    }
    return NULL;
}

/* ---- storm ---- */
static void *storm_worker(void *arg)
{
    int cpu = (int)(intptr_t)arg;
    /* Pin to one CPU (single-CPU clusters always exist) but keep our own low
     * priority: the point is to compete with the control loops on their cores. */
    uint64_t mask = 1ull << cpu;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)mask);
    volatile uint64_t x = 0;
    while (cz_now_ns() < atomic_load(&run_until_ns)) {
        for (int i = 0; i < 10000; i++) x += (uint64_t)i * 2654435761u;
    }
    return NULL;
}

static void do_storm(int ms)
{
    int ncpu = cz_num_cpus();
    pthread_t th[64];
    atomic_store(&run_until_ns, cz_now_ns() + (uint64_t)ms * 1000000ull);
    cz_log(WHO, "CPU storm: %d spinning threads at priority %d for %d ms", ncpu, CZ_PRIO_INFOTAINMENT, ms);
    for (int i = 0; i < ncpu && i < 64; i++) pthread_create(&th[i], NULL, storm_worker, (void *)(intptr_t)i);
    for (int i = 0; i < ncpu && i < 64; i++) pthread_join(th[i], NULL);
    cz_log(WHO, "CPU storm over");
}

/* ---- leak ---- */
static void do_leak(void)
{
    const size_t chunk = 4u * 1024u * 1024u;
    cz_log(WHO, "leaking 4 MB every 40 ms (address space limit is enforced by the guardian)");
    for (;;) {
        char *p = malloc(chunk);
        if (!p) {
            cz_log(WHO, "malloc failed after %u MB: out of memory, aborting", atomic_load(&alloc_mb));
            heartbeat();
            abort();
        }
        memset(p, 0xA5, chunk);           /* touch every page so it is really committed */
        atomic_fetch_add(&alloc_mb, 4u);
        sleep_ns(40000000ull);
    }
}

/* ---- flood ---- */
static void *flood_worker(void *arg)
{
    (void)arg;
    struct cz_reply r;
    while (cz_now_ns() < atomic_load(&run_until_ns)) {
        if (MsgSend(ccoid, &flood_msg, sizeof(flood_msg), &r, sizeof(r)) == 0) atomic_fetch_add(&flood_sent, 1);
        else break;
    }
    return NULL;
}

static void do_flood(int ms)
{
    flood_msg.hdr.type = CZ_MSG_MEDIA;
    flood_msg.hdr.proc = CZ_P_INFOTAINMENT;
    flood_msg.len = sizeof(flood_msg.payload);
    memset(flood_msg.payload, 0x5a, sizeof(flood_msg.payload));
    pthread_t th[4];
    atomic_store(&run_until_ns, cz_now_ns() + (uint64_t)ms * 1000000ull);
    uint64_t before = atomic_load(&flood_sent);
    cz_log(WHO, "message flood: 4 threads MsgSend to the cluster for %d ms", ms);
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, flood_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    cz_log(WHO, "message flood over: %llu messages delivered",
           (unsigned long long)(atomic_load(&flood_sent) - before));
}

static void *command_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct cz_hdr w = { .type = CZ_MSG_CMD_WAIT, .proc = CZ_P_INFOTAINMENT };
        struct cz_reply cmd;
        if (MsgSend(gcoid, &w, sizeof(w), &cmd, sizeof(cmd)) == -1) {
            sleep_ns(100000000ull);
            continue;
        }
        atomic_store(&cur_mode, (uint32_t)cmd.cmd);
        heartbeat();
        switch (cmd.cmd) {
        case CZ_FAULT_CRASH: {
            cz_log(WHO, "chaos: dereferencing NULL");
            volatile int *volatile bad = NULL;
            *bad = 42;
            break;
        }
        case CZ_FAULT_STORM: do_storm(cmd.arg > 0 ? cmd.arg : 6000); break;
        case CZ_FAULT_LEAK:  do_leak(); break;
        case CZ_FAULT_FLOOD: do_flood(cmd.arg > 0 ? cmd.arg : 6000); break;
        default: break;
        }
        atomic_store(&cur_mode, CZ_FAULT_NONE);
        heartbeat();
    }
    return NULL;
}

int main(void)
{
    cz_set_sched(SCHED_RR, CZ_PRIO_INFOTAINMENT);
    gcoid = cz_guardian_connect(CZ_P_INFOTAINMENT);
    if (gcoid == -1) { cz_log(WHO, "guardian connect: %s", strerror(errno)); return 1; }
    for (int i = 0; i < 200 && ccoid == -1; i++) {
        ccoid = name_open(CZ_CLUSTER_NAME, 0);
        if (ccoid == -1) sleep_ns(10000000ull);
    }
    if (ccoid == -1) cz_log(WHO, "cluster not reachable: %s", strerror(errno));

    pthread_t hb, ct;
    pthread_create(&hb, NULL, heartbeat_thread, NULL);
    pthread_create(&ct, NULL, command_thread, NULL);
    cz_guardian_ready(gcoid, CZ_P_INFOTAINMENT);
    cz_log(WHO, "media and navigation up (uid %d, pid %d)", (int)getuid(), getpid());

    /* Normal operation: decode audio frames and push now-playing updates. */
    struct cz_msg_media m;
    memset(&m, 0, sizeof(m));
    m.hdr.type = CZ_MSG_MEDIA;
    m.hdr.proc = CZ_P_INFOTAINMENT;
    volatile double acc = 0.0;
    for (uint64_t k = 0;; k++) {
        for (int i = 0; i < 20000; i++) acc += (double)(i % 7) * 0.5;   /* "decode" */
        if ((k % 10) == 0 && ccoid != -1 && atomic_load(&cur_mode) == CZ_FAULT_NONE) {
            m.track = (uint32_t)atomic_fetch_add(&tracks, 1);
            m.len = (uint32_t)snprintf(m.payload, sizeof(m.payload), "track %u: now playing", m.track);
            struct cz_reply r;
            MsgSend(ccoid, &m, sizeof(m.hdr) + 8 + m.len, &r, sizeof(r));
        }
        sleep_ns(50000000ull);
    }
    return 0;
}
