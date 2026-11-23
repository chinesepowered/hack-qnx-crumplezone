/*
 * rtbench: characterise the platform the demo runs on.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Measures, on the running QNX target:
 *   - periodic timer wake-up latency for a 1 ms timer, with and without a
 *     high-resolution tolerance, at several phase offsets from the tick
 *   - MsgSend/MsgReceive/MsgReply round trip to a thread on the same CPU and
 *     on another CPU, and a read() round trip to the wheel_sensors resource
 *     manager
 * Run as root: rtbench [cpu]
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "cz_child.h"
#include "cz_rt.h"

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, uint64_t *v, int n)
{
    qsort(v, (size_t)n, sizeof(v[0]), cmp_u64);
    printf("%-44s n=%5d p50 %7.1f us  p90 %7.1f  p99 %7.1f  max %8.1f\n", what, n,
           v[n / 2] / 1000.0, v[n * 9 / 10] / 1000.0, v[n * 99 / 100] / 1000.0, v[n - 1] / 1000.0);
}

/* mode 0: periodic, tick tolerance. 1: periodic, high resolution.
 * 2: high resolution one-shot re-armed every period at an absolute release time. */
static void timer_test(int mode, uint64_t phase_ns, int n)
{
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, 60, _PULSE_CODE_MINAVAIL, 0);
    timer_t t;
    timer_create(CLOCK_MONOTONIC, &ev, &t);
    if (mode >= 1) {
        struct itimerspec tol;
        memset(&tol, 0, sizeof(tol));
        tol.it_value.tv_nsec = 1;
        if (timer_settime(t, TIMER_TOLERANCE, &tol, NULL) == -1) printf("tolerance: %s\n", strerror(errno));
    }
    uint64_t start = cz_align_up(cz_now_ns() + 3000000ull, 1000000ull) + phase_ns;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(start / 1000000000ull);
    its.it_value.tv_nsec = (long)(start % 1000000000ull);
    if (mode < 2) its.it_interval.tv_nsec = 1000000;
    timer_settime(t, TIMER_ABSTIME, &its, NULL);
    uint64_t *ulat = calloc((size_t)n, sizeof(uint64_t));
    uint64_t first = 0, last = 0, release = start;
    int late_period = 0;
    for (int i = 0; i < n; i++) {
        struct _pulse p;
        MsgReceivePulse(chid, &p, sizeof(p), NULL);
        uint64_t now = cz_now_ns();
        if (i == 0) first = now;
        last = now;
        if (mode < 2) {
            int64_t d = (int64_t)(now - start) % 1000000;     /* phase vs the nominal grid */
            if (d > 500000) d -= 1000000;
            ulat[i] = (uint64_t)(d + 1000000);
        } else {
            while (release + 1000000ull <= now) { release += 1000000ull; late_period++; }
            ulat[i] = (uint64_t)((int64_t)(now - release) + 1000000);
            release += 1000000ull;
            its.it_value.tv_sec = (time_t)(release / 1000000000ull);
            its.it_value.tv_nsec = (long)(release % 1000000000ull);
            timer_settime(t, TIMER_ABSTIME, &its, NULL);
        }
    }
    timer_delete(t);
    qsort(ulat, (size_t)n, sizeof(ulat[0]), cmp_u64);
    static const char *mn[] = { "periodic tick  ", "periodic hi-res", "abs re-arm hres" };
    printf("%s phase %3llu: mean interval %8.1f us, offset p1 %+7.1f p50 %+7.1f p99 %+7.1f max %+8.1f us skipped %d\n",
           mn[mode], (unsigned long long)(phase_ns / 1000), (double)(last - first) / (n - 1) / 1000.0,
           ((int64_t)ulat[n / 100] - 1000000) / 1000.0, ((int64_t)ulat[n / 2] - 1000000) / 1000.0,
           ((int64_t)ulat[n * 99 / 100] - 1000000) / 1000.0, ((int64_t)ulat[n - 1] - 1000000) / 1000.0, late_period);
    free(ulat);
    ConnectDetach(coid);
    ChannelDestroy(chid);
}

static int srv_chid;
static void *server(void *arg)
{
    int cpu = (int)(intptr_t)arg;
    cz_pin_cpu(cpu);
    char buf[64];
    for (;;) {
        rcvid_t r = MsgReceive(srv_chid, buf, sizeof(buf), NULL);
        if (r > 0) MsgReply(r, EOK, buf, 16);
    }
    return NULL;
}

static void msg_test(int client_cpu, int server_cpu, int n)
{
    srv_chid = ChannelCreate(0);
    pthread_t th;
    pthread_create(&th, NULL, server, (void *)(intptr_t)server_cpu);
    usleep(20000);
    int coid = ConnectAttach(0, 0, srv_chid, _NTO_SIDE_CHANNEL, 0);
    uint64_t *v = calloc((size_t)n, sizeof(uint64_t));
    char m[16] = "ping", r[16];
    for (int i = 0; i < n; i++) {
        uint64_t t0 = cz_now_ns();
        MsgSend(coid, m, sizeof(m), r, sizeof(r));
        v[i] = cz_now_ns() - t0;
    }
    char what[80];
    snprintf(what, sizeof(what), "MsgSend round trip cpu %d -> thread on cpu %d", client_cpu, server_cpu);
    report(what, v, n);
    free(v);
}

static void read_test(int n)
{
    int fd = open(CZ_PATH_WHEEL, O_RDONLY);
    if (fd == -1) { printf("read test: %s: %s\n", CZ_PATH_WHEEL, strerror(errno)); return; }
    uint64_t *v = calloc((size_t)n, sizeof(uint64_t));
    struct cz_wheel_sample s;
    for (int i = 0; i < n; i++) {
        uint64_t t0 = cz_now_ns();
        read(fd, &s, sizeof(s));
        v[i] = cz_now_ns() - t0;
    }
    report("read() of /dev/vehicle/wheel_speed", v, n);
    free(v);
    close(fd);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int cpu = argc > 1 ? atoi(argv[1]) : 1;
    cz_set_sched(SCHED_FIFO, argc > 3 ? atoi(argv[3]) : 60);
    cz_pin_cpu(cpu);
    printf("rtbench on cpu %d, %d CPUs, cycles/s %llu, clock from %s\n", cpu, cz_num_cpus(),
           (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec, cz_clock_is_cycles() ? "ClockCycles" : "kernel");
    struct timespec res;
    clock_getres(CLOCK_MONOTONIC, &res);
    printf("clock_getres(CLOCK_MONOTONIC) = %ld ns, ClockPeriod tick = ", res.tv_nsec);
    struct _clockperiod cp;
    if (ClockPeriod(CLOCK_MONOTONIC, NULL, &cp, 0) == 0) printf("%lu ns\n", (unsigned long)cp.nsec);
    else printf("? (%s)\n", strerror(errno));

    uint64_t t0 = cz_now_ns();
    for (int i = 0; i < 10000; i++) (void)cz_now_ns();
    printf("cz_now_ns() cost: %.1f ns\n", (double)(cz_now_ns() - t0) / 10000.0);
    struct timespec ts;
    t0 = cz_now_ns();
    for (int i = 0; i < 10000; i++) clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("clock_gettime() cost: %.1f ns\n", (double)(cz_now_ns() - t0) / 10000.0);

    if (argc > 2 && strcmp(argv[2], "abs") == 0) {
        timer_test(2, 0, 3000);
        timer_test(2, 500000, 3000);
        return 0;
    }
    for (int mode = 0; mode < 3; mode++)
        for (uint64_t ph = 0; ph < 1000000; ph += 500000) timer_test(mode, ph, 2000);
    msg_test(cpu, cpu, 5000);
    msg_test(cpu, (cpu + 1) % cz_num_cpus(), 5000);
    read_test(5000);
    return 0;
}
