/*
 * guardian: supervisor. Spawns every process with its scheduling policy,
 * priority, CPU affinity, credentials and limits, detects death through three
 * independent kernel paths, restarts in milliseconds, and records the
 * recovery time of every restart end to end.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Death detection (whichever arrives first triggers the restart):
 *   1. procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH): a pulse from the
 *      process manager carrying the dead pid (SIGEV_MAKE_UPDATEABLE)
 *   2. _NTO_CHF_DISCONNECT on the guardian channel: every supervised process
 *      keeps one connection open, the kernel detaches it on death and sends
 *      _PULSE_CODE_DISCONNECT with the server connection id
 *   3. a reaper thread in waitid(), which also provides the exit status
 * Fault injection is part of the guardian so the fault timestamp, the
 * detection, the restart and the READY message are all measured on one clock.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_msg.h"
#include "cz_rt.h"

#define WHO "guardian"
#define BOOT_TIMEOUT_NS 3000000000ull

enum { DETECT_PROCMGR = 1, DETECT_DISCONNECT = 2, DETECT_REAPER = 3 };
static const char *detect_name[] = { "?", "procmgr death event", "channel disconnect", "waitid reaper" };

struct child {
    int id;
    const char *path;
    int policy;
    int prio;
    int cpu;              /* -1: any CPU */
    int uid, gid;         /* -1: inherit (root) */
    uint64_t as_limit;    /* RLIMIT_AS imposed with prlimit(), 0 = none */
    int critical;
    pid_t pid;
    pid_t prev_pid;       /* the incarnation that died last, for late exit statuses */
    int scoid;
    int state;
    int restart_pending;
    int fault;            /* enum cz_fault injected into this process, 0 none */
    uint64_t t_fault, t_detect, t_spawn, t_ready;
    uint32_t restarts;
};

static struct child kids[] = {
    { CZ_P_VEHICLE_SIM,   "/proc/boot/vehicle_sim",   SCHED_FIFO, CZ_PRIO_VEHICLE_SIM, 0, -1, -1, 0, 0 },
    { CZ_P_WHEEL_SENSORS, "/proc/boot/wheel_sensors", SCHED_RR,   CZ_PRIO_SENSORS,     -1, -1, -1, 0, 1 },
    { CZ_P_CLUSTER,       "/proc/boot/cluster",       SCHED_RR,   CZ_PRIO_CLUSTER,     -1, -1, -1, 0, 0 },
    { CZ_P_BRAKE,         "/proc/boot/brake_ctl",     SCHED_FIFO, CZ_PRIO_BRAKE,       -2, -1, -1, 0, 1 },
    { CZ_P_STEER,         "/proc/boot/steer_ctl",     SCHED_FIFO, CZ_PRIO_STEER,       -3, -1, -1, 0, 1 },
    { CZ_P_TELEMETRY,     "/proc/boot/telemetry",     SCHED_RR,   CZ_PRIO_TELEMETRY,   -1, -1, -1, 0, 0 },
    { CZ_P_INFOTAINMENT,  "/proc/boot/infotainment",  SCHED_RR,   CZ_PRIO_INFOTAINMENT, -1,
      CZ_INFOTAINMENT_UID, CZ_INFOTAINMENT_GID, CZ_INFOTAINMENT_AS_LIMIT, 0 },
};
#define NKIDS ((int)(sizeof(kids) / sizeof(kids[0])))

static struct cz_bus *bus;
static int chid, self_coid;
static char **child_env;
static rcvid_t cmd_rcvid = -1;        /* infotainment command thread, reply-blocked */
static int boot_next;                  /* index of the next child to start at boot */
static uint64_t boot_wait_since;
static uint32_t infot_prev_mode;
static uint64_t infot_mode_since;

/* Exit statuses collected by the reaper thread. */
static pthread_mutex_t reap_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { pid_t pid; int code; int status; } reaped[32];
static int reaped_n;

static struct child *by_pid(pid_t pid)
{
    for (int i = 0; i < NKIDS; i++) if (kids[i].pid == pid && pid > 0) return &kids[i];
    return NULL;
}

static struct child *by_id(int id)
{
    for (int i = 0; i < NKIDS; i++) if (kids[i].id == id) return &kids[i];
    return NULL;
}

static void publish(struct child *c)
{
    struct cz_proc *p = &bus->procs[c->id];
    atomic_store(&p->pid, c->pid);
    atomic_store(&p->state, (uint32_t)c->state);
    atomic_store(&p->restarts, c->restarts);
    p->prio = c->prio;
    p->policy = c->policy == SCHED_FIFO ? 'f' : (c->policy == SCHED_RR ? 'r' : 'o');
    p->critical = (uint8_t)c->critical;
    p->runmask = c->cpu >= 0 ? (1ull << c->cpu) : 0;
    p->uid = c->uid >= 0 ? (uint32_t)c->uid : 0;
    p->as_limit = c->as_limit;
}

static int resolve_cpu(int cpu)
{
    int n = cz_num_cpus();
    if (cpu == -2) return n - 1;              /* brake: last CPU */
    if (cpu == -3) return n >= 3 ? n - 2 : 0; /* steering: second to last */
    return cpu;
}

static int spawn_child(struct child *c)
{
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    uint32_t flags = POSIX_SPAWN_SETSCHEDULER;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = c->prio;
    posix_spawnattr_setschedpolicy(&attr, c->policy);
    posix_spawnattr_setschedparam(&attr, &sp);
    if (c->cpu >= 0) {
        flags |= POSIX_SPAWN_EXPLICIT_CPU;
        pthread_spawnattr_setrunmask_np(&attr, 1ull << c->cpu);
    }
    if (c->uid >= 0) {
        flags |= POSIX_SPAWN_SETCRED;
        posix_spawnattr_setcred(&attr, (uid_t)c->uid, (gid_t)c->gid);
    }
    /* setxflags stores the POSIX and QNX flags together; set them all at once. */
    posix_spawnattr_setxflags(&attr, flags);

    const char *name = cz_proc_name(c->id);
    char *argv[] = { (char *)name, NULL };
    pid_t pid = -1;
    c->t_spawn = cz_now_ns();
    int rc = posix_spawn(&pid, c->path, NULL, &attr, argv, child_env);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        cz_log(WHO, "posix_spawn %s: %s", c->path, strerror(rc));
        c->state = CZ_PS_DEAD;
        publish(c);
        return -1;
    }
    if (c->as_limit) {
        struct rlimit rl = { (rlim_t)c->as_limit, (rlim_t)c->as_limit };
        if (prlimit(pid, RLIMIT_AS, &rl, NULL) == -1)
            cz_log(WHO, "prlimit(%s, RLIMIT_AS): %s", name, strerror(errno));
    }
    /* No core files: dumper would otherwise write the whole address space of a
     * crashed process before it is torn down, which delays death notification
     * by hundreds of milliseconds (seconds for a 190 MB leak). */
    struct rlimit core = { 0, 0 };
    if (prlimit(pid, RLIMIT_CORE, &core, NULL) == -1)
        cz_log(WHO, "prlimit(%s, RLIMIT_CORE): %s", name, strerror(errno));
    c->pid = pid;
    c->scoid = -1;
    c->state = CZ_PS_STARTING;
    publish(c);
    cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_SPAWN, c->id, pid, c->prio, (int64_t)c->t_spawn, 0, name);
    return 0;
}

static const char *signame(int s)
{
    switch (s) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGKILL: return "SIGKILL";
    case SIGBUS:  return "SIGBUS";
    case SIGTERM: return "SIGTERM";
    case SIGILL:  return "SIGILL";
    default:      return "signal";
    }
}

static void on_death(struct child *c, int path)
{
    if (c->state != CZ_PS_RUNNING && c->state != CZ_PS_STARTING) return;   /* already handled */
    uint64_t now = cz_now_ns();
    c->t_detect = now;
    c->state = CZ_PS_DEAD;
    if (c->id == CZ_P_INFOTAINMENT) cmd_rcvid = -1;
    int64_t since_fault = c->t_fault ? (int64_t)(now - c->t_fault) : -1;
    cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_DEATH, c->id, c->pid, path, since_fault, 0, detect_name[path]);
    cz_trace_mark("CZ guardian: %s pid %d died (%s)", cz_proc_name(c->id), c->pid, detect_name[path]);
    if (since_fault >= 0)
        cz_log(WHO, "%s (pid %d) died; detected via %s %.3f ms after the fault", cz_proc_name(c->id),
               c->pid, detect_name[path], (double)since_fault / 1e6);
    else
        cz_log(WHO, "%s (pid %d) died; detected via %s", cz_proc_name(c->id), c->pid, detect_name[path]);
    pid_t old = c->pid;
    c->prev_pid = old;
    c->restarts++;
    if (spawn_child(c) == 0) {
        c->restart_pending = 1;
        cz_log(WHO, "restarting %s: new pid %d (old %d)", cz_proc_name(c->id), c->pid, old);
    }
}

static void on_ready(struct child *c)
{
    uint64_t now = cz_now_ns();
    c->t_ready = now;
    c->state = CZ_PS_RUNNING;
    struct cz_proc *p = &bus->procs[c->id];
    atomic_store(&p->last_ready_ns, now);
    cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_READY, c->id, c->pid, 0, (int64_t)(now - c->t_spawn), 0, cz_proc_name(c->id));
    if (c->restart_pending) {
        int64_t from_fault = c->t_fault ? (int64_t)(now - c->t_fault) : -1;
        int64_t from_detect = (int64_t)(now - c->t_detect);
        uint64_t rec = from_fault >= 0 && c->fault != CZ_FAULT_LEAK ? (uint64_t)from_fault : (uint64_t)from_detect;
        atomic_store(&p->last_recovery_ns, rec);
        cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_RESTART, c->id, c->pid, c->fault, from_fault, from_detect,
                rec <= CZ_RECOVERY_BUDGET_NS ? "within budget" : "over budget");
        cz_trace_mark("CZ guardian: %s ready again, restart %lld ns", cz_proc_name(c->id), (long long)from_detect);
        if (from_fault >= 0 && c->fault != CZ_FAULT_LEAK)
            cz_log(WHO, "%s restored: %.3f ms from fault to ready (restart %.3f ms)",
                   cz_proc_name(c->id), (double)from_fault / 1e6, (double)from_detect / 1e6);
        else
            cz_log(WHO, "%s restored: %.3f ms from death to ready", cz_proc_name(c->id), (double)from_detect / 1e6);
        c->restart_pending = 0;
        c->fault = 0;
        c->t_fault = 0;
    }
    publish(c);
}

static void *reaper(void *arg)
{
    (void)arg;
    cz_set_sched(SCHED_FIFO, CZ_PRIO_GUARDIAN);
    for (;;) {
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        if (waitid(P_ALL, 0, &si, WEXITED) == -1) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&reap_mx);
        if (reaped_n < 32) {
            reaped[reaped_n].pid = si.si_pid;
            reaped[reaped_n].code = si.si_code;
            reaped[reaped_n].status = si.si_status;
            reaped_n++;
        }
        pthread_mutex_unlock(&reap_mx);
        MsgSendPulse(self_coid, CZ_PRIO_GUARDIAN, CZ_PULSE_REAPED, si.si_pid);
    }
    return NULL;
}

static void handle_reaped(void)
{
    pthread_mutex_lock(&reap_mx);
    int n = reaped_n;
    typeof(reaped[0]) local[32];
    memcpy(local, reaped, sizeof(local[0]) * (size_t)n);
    reaped_n = 0;
    pthread_mutex_unlock(&reap_mx);
    for (int i = 0; i < n; i++) {
        int sig = (local[i].code == CLD_EXITED) ? -local[i].status : local[i].status;
        /* Find the child by pid: either the current incarnation or the one just replaced. */
        int id = -1;
        for (int k = 0; k < NKIDS; k++) {
            if (kids[k].pid == local[i].pid || kids[k].prev_pid == local[i].pid) id = kids[k].id;
        }
        char txt[24];
        if (sig > 0) snprintf(txt, sizeof(txt), "%s", signame(sig));
        else snprintf(txt, sizeof(txt), "exit %d", -sig);
        if (id >= 0) {
            struct child *c = by_id(id);
            atomic_store(&bus->procs[id].last_signal, sig);
            if (c->pid == local[i].pid) on_death(c, DETECT_REAPER);
        }
        cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_EXIT_STATUS, id >= 0 ? id : CZ_NPROC, local[i].pid, sig, 0, 0, txt);
    }
}

static int inject(int fault, int arg_ms, uint64_t *t_out)
{
    struct child *it = by_id(CZ_P_INFOTAINMENT);
    struct child *ws = by_id(CZ_P_WHEEL_SENSORS);
    uint64_t t = cz_now_ns();
    switch (fault) {
    case CZ_FAULT_CRASH:
    case CZ_FAULT_STORM:
    case CZ_FAULT_LEAK:
    case CZ_FAULT_FLOOD: {
        if (it->state != CZ_PS_RUNNING || cmd_rcvid == -1) return EBUSY;
        if (fault == CZ_FAULT_CRASH || fault == CZ_FAULT_LEAK) {
            it->fault = fault;
            it->t_fault = t;
        }
        struct cz_reply cmd = { 0 };
        cmd.cmd = fault;
        cmd.arg = arg_ms;
        cmd.t_ns = t;
        cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_FAULT_INJECT, CZ_P_INFOTAINMENT, it->pid, fault, arg_ms, 0, cz_fault_name(fault));
        cz_trace_mark("CZ fault inject: %s", cz_fault_name(fault));
        cz_log(WHO, "injecting fault '%s' into infotainment (pid %d)", cz_fault_name(fault), it->pid);
        rcvid_t r = cmd_rcvid;
        cmd_rcvid = -1;
        MsgReply(r, EOK, &cmd, sizeof(cmd));
        break;
    }
    case CZ_FAULT_KILL_SENSOR:
        if (ws->state != CZ_PS_RUNNING) return EBUSY;
        ws->fault = fault;
        ws->t_fault = t;
        cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_FAULT_INJECT, CZ_P_WHEEL_SENSORS, ws->pid, fault, 0, 0, "kill_sensor");
        cz_trace_mark("CZ fault inject: SIGKILL wheel_sensors pid %d", ws->pid);
        cz_log(WHO, "injecting fault 'kill_sensor': SIGKILL wheel_sensors (pid %d)", ws->pid);
        kill(ws->pid, SIGKILL);
        break;
    default:
        return EINVAL;
    }
    *t_out = t;
    return 0;
}

static void build_env(void)
{
    extern char **environ;
    int n = 0;
    while (environ[n]) n++;
    child_env = calloc((size_t)n + 2, sizeof(char *));
    int k = 0;
    for (int i = 0; i < n; i++) if (strncmp(environ[i], "CZ_GUARDIAN=", 12) != 0) child_env[k++] = environ[i];
    char *g = malloc(64);
    snprintf(g, 64, "CZ_GUARDIAN=%d:%d", getpid(), chid);
    child_env[k++] = g;
    child_env[k] = NULL;
}

static void boot_step(void)
{
    while (boot_next < NKIDS) {
        struct child *c = &kids[boot_next];
        if (c->state == CZ_PS_STOPPED) {
            spawn_child(c);
            boot_wait_since = cz_now_ns();
            return;
        }
        if (c->state == CZ_PS_STARTING && cz_now_ns() - boot_wait_since < BOOT_TIMEOUT_NS) return;
        if (c->state == CZ_PS_STARTING) cz_log(WHO, "%s did not report ready in time, continuing", cz_proc_name(c->id));
        boot_next++;
        if (boot_next == NKIDS) {
            cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_BOOT, CZ_P_GUARDIAN, getpid(), NKIDS,
                    (int64_t)(cz_now_ns() - bus->boot_ns), 0, "system up");
            cz_log(WHO, "all %d processes up in %.1f ms", NKIDS, (double)(cz_now_ns() - bus->boot_ns) / 1e6);
        }
    }
}

int main(void)
{
    if (cz_set_sched(SCHED_FIFO, CZ_PRIO_GUARDIAN) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));
    for (int i = 0; i < NKIDS; i++) kids[i].cpu = resolve_cpu(kids[i].cpu);

    bus = cz_bus_create();
    if (!bus) { cz_log(WHO, "cannot create %s: %s", CZ_BUS_NAME, strerror(errno)); return 1; }

    chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (chid == -1) { cz_log(WHO, "ChannelCreate: %s", strerror(errno)); return 1; }
    self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    bus->guardian_pid = getpid();
    bus->guardian_chid = chid;
    struct cz_proc *gp = &bus->procs[CZ_P_GUARDIAN];
    atomic_store(&gp->pid, getpid());
    atomic_store(&gp->state, CZ_PS_RUNNING);
    gp->prio = CZ_PRIO_GUARDIAN;
    gp->policy = 'f';
    gp->critical = 1;

    /* Process-death notifications from the process manager, pid in the value. */
    struct sigevent dev;
    SIGEV_PULSE_INIT(&dev, self_coid, CZ_PRIO_GUARDIAN, CZ_PULSE_DEATH, 0);
    SIGEV_MAKE_UPDATEABLE(&dev);
    if (MsgRegisterEvent(&dev, SYSMGR_COID) == -1)
        cz_log(WHO, "MsgRegisterEvent: %s", strerror(errno));
    if (procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH, &dev) == -1)
        cz_log(WHO, "procmgr_event_notify_add: %s", strerror(errno));

    /* 10 Hz housekeeping tick (boot sequencing, fault end detection). */
    uint64_t start = cz_align_up(cz_now_ns() + 10000000ull, 1000000ull);
    cz_periodic_pulse_timer(self_coid, CZ_PRIO_GUARDIAN, CZ_PULSE_TICK, start, 100000000ull, NULL);

    build_env();
    pthread_t rt;
    pthread_create(&rt, NULL, reaper, NULL);

    cz_log(WHO, "Crumple Zone on QNX OS 8.0, %d CPUs, guardian pid %d channel %d", cz_num_cpus(), getpid(), chid);
    for (int i = 0; i < NKIDS; i++) {
        struct child *c = &kids[i];
        char cpu[16];
        if (c->cpu >= 0) snprintf(cpu, sizeof(cpu), "%d", c->cpu);
        else snprintf(cpu, sizeof(cpu), "any");
        cz_log(WHO, "  %-13s %s %2d  cpu %-3s%s%s", cz_proc_name(c->id),
               c->policy == SCHED_FIFO ? "FIFO" : "RR  ", c->prio, cpu,
               c->uid >= 0 ? "  uid 1500" : "", c->as_limit ? "  RLIMIT_AS 192 MB" : "");
        publish(c);
    }
    boot_step();

    union {
        struct _pulse pulse;
        struct cz_hdr hdr;
        struct cz_msg_hello hello;
        struct cz_msg_ready ready;
        struct cz_msg_heartbeat hb;
        struct cz_msg_fault fault;
        uint16_t type;
        char raw[512];
    } msg;

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) continue;

        if (rcvid == 0) {
            struct child *c;
            switch (msg.pulse.code) {
            case CZ_PULSE_DEATH:
                if ((c = by_pid(msg.pulse.value.sival_int))) on_death(c, DETECT_PROCMGR);
                break;
            case _PULSE_CODE_DISCONNECT:
                for (int i = 0; i < NKIDS; i++) {
                    if (kids[i].scoid == msg.pulse.scoid) {
                        kids[i].scoid = -1;
                        on_death(&kids[i], DETECT_DISCONNECT);
                    }
                }
                ConnectDetach(msg.pulse.scoid);
                break;
            case CZ_PULSE_REAPED:
                handle_reaped();
                break;
            case CZ_PULSE_TICK:
                if (boot_next < NKIDS) boot_step();
                break;
            default:
                break;
            }
            continue;
        }

        struct cz_reply rep = { 0 };
        switch (msg.hdr.type) {
        case CZ_MSG_HELLO: {
            struct child *c = by_id(msg.hdr.proc);
            if (c && c->pid == msg.hello.pid) c->scoid = info.scoid;
            MsgReply(rcvid, EOK, &rep, sizeof(rep));
            break;
        }
        case CZ_MSG_READY: {
            struct child *c = by_id(msg.hdr.proc);
            MsgReply(rcvid, EOK, &rep, sizeof(rep));
            if (c && c->pid == msg.ready.pid) {
                on_ready(c);
                if (boot_next < NKIDS && &kids[boot_next] == c) { boot_next++; boot_step(); }
            }
            break;
        }
        case CZ_MSG_CMD_WAIT:
            if (msg.hdr.proc == CZ_P_INFOTAINMENT) {
                if (cmd_rcvid != -1) MsgError(cmd_rcvid, EINTR);
                cmd_rcvid = rcvid;             /* reply later, when a fault is injected */
            } else {
                MsgError(rcvid, EINVAL);
            }
            break;
        case CZ_MSG_HEARTBEAT: {
            struct cz_infot_status *s = &bus->infot;
            uint64_t now = cz_now_ns();
            cz_seq_write_begin(&s->seq);
            s->mode = msg.hb.mode;
            s->alloc_mb = msg.hb.alloc_mb;
            s->flood_sent = msg.hb.flood_sent;
            s->media_tracks = msg.hb.media_tracks;
            s->heartbeat_ns = now;
            cz_seq_write_end(&s->seq);
            if (msg.hb.mode != infot_prev_mode) {
                if (infot_prev_mode == CZ_FAULT_STORM || infot_prev_mode == CZ_FAULT_FLOOD) {
                    cz_emit(bus, CZ_EVS_GUARDIAN, CZ_EV_FAULT_END, CZ_P_INFOTAINMENT, by_id(CZ_P_INFOTAINMENT)->pid,
                            (int)infot_prev_mode, (int64_t)(now - infot_mode_since), (int64_t)msg.hb.flood_sent,
                            cz_fault_name((int)infot_prev_mode));
                    cz_trace_mark("CZ fault end: %s", cz_fault_name((int)infot_prev_mode));
                }
                infot_prev_mode = msg.hb.mode;
                infot_mode_since = now;
            }
            MsgReply(rcvid, EOK, &rep, sizeof(rep));
            break;
        }
        case CZ_MSG_FAULT: {
            uint64_t t = 0;
            rep.status = inject(msg.fault.fault, msg.fault.arg_ms, &t);
            rep.t_ns = t;
            MsgReply(rcvid, EOK, &rep, sizeof(rep));
            break;
        }
        default:
            if (msg.type == _IO_CONNECT) MsgReply(rcvid, EOK, NULL, 0);
            else MsgError(rcvid, ENOSYS);
            break;
        }
    }
    return 0;
}
