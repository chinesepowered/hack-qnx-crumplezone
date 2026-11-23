/*
 * czctl: operator command line on the target.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 *   czctl status               process table (pid, priority, CPU, restarts, last recovery)
 *   czctl loops                brake and steering loop timing
 *   czctl watch [seconds]      print loop timing once per second
 *   czctl fault <name> [ms]    crash | storm | leak | flood | kill_sensor
 *   czctl reset                reset windowed statistics
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_msg.h"
#include "cz_rt.h"

static void print_status(struct cz_bus *b)
{
    static const char *sn[] = { "stopped", "starting", "running", "dead" };
    printf("%-14s %8s %-9s %4s %-4s %8s %14s\n", "process", "pid", "state", "prio", "cpu", "restarts", "last recovery");
    for (int i = 0; i < CZ_NPROC; i++) {
        struct cz_proc *p = &b->procs[i];
        char cpu[8] = "any";
        if (p->runmask) for (int c = 0; c < 64; c++) if (p->runmask == (1ull << c)) snprintf(cpu, sizeof(cpu), "%d", c);
        uint64_t rec = atomic_load(&p->last_recovery_ns);
        char recs[24] = "-";
        if (rec) snprintf(recs, sizeof(recs), "%.3f ms", (double)rec / 1e6);
        printf("%-14s %8d %-9s %3d%c %-4s %8u %14s\n", p->name, atomic_load(&p->pid), sn[atomic_load(&p->state) & 3],
               p->prio, p->policy ? p->policy : ' ', cpu, atomic_load(&p->restarts), recs);
    }
}

static void print_loop(const char *name, struct cz_loop *l)
{
    struct cz_loop_status s;
    cz_loop_read_status(l, &s);
    printf("%-6s %5u Hz cpu %d prio %d  %-9s iter %-9llu worst latency %7.1f us (window %7.1f us) "
           "jitter %6.1f us  misses %llu\n",
           name, s.period_ns ? 1000000000u / s.period_ns : 0, s.cpu, s.prio, cz_mode_name((int)s.mode),
           (unsigned long long)s.iterations, s.lat_max_total_ns / 1000.0, s.lat_max_win_ns / 1000.0,
           s.jit_max_win_ns / 1000.0, (unsigned long long)s.misses_total);
}

int main(int argc, char **argv)
{
    struct cz_bus *b = cz_bus_open(0);
    if (!b) { fprintf(stderr, "czctl: bus not available: %s\n", strerror(errno)); return 1; }
    const char *cmd = argc > 1 ? argv[1] : "status";

    if (strcmp(cmd, "status") == 0) { print_status(b); return 0; }
    if (strcmp(cmd, "loops") == 0) { print_loop("brake", &b->brake); print_loop("steer", &b->steer); return 0; }
    if (strcmp(cmd, "watch") == 0) {
        int n = argc > 2 ? atoi(argv[2]) : 10;
        for (int i = 0; i < n; i++) {
            print_loop("brake", &b->brake);
            print_loop("steer", &b->steer);
            sleep(1);
        }
        return 0;
    }
    if (strcmp(cmd, "fault") == 0 || strcmp(cmd, "reset") == 0) {
        int coid = ConnectAttach(0, b->guardian_pid, b->guardian_chid, _NTO_SIDE_CHANNEL, 0);
        if (coid == -1) { fprintf(stderr, "czctl: guardian: %s\n", strerror(errno)); return 1; }
        if (strcmp(cmd, "reset") == 0) {
            /* The bus is read-only for us: reset goes through telemetry or the dashboard. */
            fprintf(stderr, "czctl: use the dashboard (or telemetry 'reset') to reset windows\n");
            return 1;
        }
        if (argc < 3) { fprintf(stderr, "usage: czctl fault <crash|storm|leak|flood|kill_sensor> [ms]\n"); return 2; }
        int f = -1;
        for (int i = 1; i < CZ_FAULT_MAX; i++) if (strcmp(argv[2], cz_fault_name(i)) == 0) f = i;
        if (f < 0) { fprintf(stderr, "czctl: unknown fault %s\n", argv[2]); return 2; }
        struct cz_msg_fault m = { .hdr = { .type = CZ_MSG_FAULT, .proc = CZ_NPROC }, .fault = f,
                                  .arg_ms = argc > 3 ? atoi(argv[3]) : 6000 };
        struct cz_reply r;
        if (MsgSend(coid, &m, sizeof(m), &r, sizeof(r)) == -1) { perror("czctl: MsgSend"); return 1; }
        if (r.status) { fprintf(stderr, "czctl: guardian refused: %s\n", strerror(r.status)); return 1; }
        printf("fault %s injected at t=%.6f s\n", argv[2], (double)r.t_ns / 1e9);
        return 0;
    }
    fprintf(stderr, "usage: czctl status|loops|watch [n]|fault <name> [ms]\n");
    return 2;
}
