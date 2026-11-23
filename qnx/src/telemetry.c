/*
 * telemetry: streams measurements and events over TCP (io-sock) to the
 * host-side dashboard and accepts operator commands.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Medium criticality, SCHED_RR 20. Reads the bus (lock-free, never blocks a
 * writer) at 20 Hz and sends newline-delimited JSON:
 *   hello   platform info
 *   loop    brake/steer status plus every iteration sample since last send
 *   ev      events from all producers (guardian, controllers, ...)
 *   proc    per-process state, CPU %, memory, restarts; per-CPU load
 *   cluster instrument cluster stats, infot infotainment heartbeat
 * Commands (one per line): fault <crash|storm|leak|flood|kill_sensor> [ms],
 * reset, ping. Faults are forwarded to the guardian with MsgSend().
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"

#define WHO "telemetry"
#define MAX_CLIENTS 4
#define OUT_MAX (4u * 1024u * 1024u)

struct client {
    int fd;
    char in[1024];
    size_t inlen;
    char *out;
    size_t outlen, outcap;
    uint64_t tail_brake, tail_steer;
    uint64_t tail_ev[CZ_NEVSRC];
};

static struct cz_bus *bus;
static struct client cl[MAX_CLIENTS];
static int gcoid = -1;

static void out_append(struct client *c, const char *s, size_t n)
{
    if (c->fd < 0) return;
    if (c->outlen + n > c->outcap) {
        size_t cap = c->outcap ? c->outcap : 65536;
        while (cap < c->outlen + n) cap *= 2;
        if (cap > OUT_MAX) { close(c->fd); c->fd = -1; c->outlen = 0; return; }
        char *nb = realloc(c->out, cap);
        if (!nb) return;
        c->out = nb;
        c->outcap = cap;
    }
    memcpy(c->out + c->outlen, s, n);
    c->outlen += n;
}

static void out_printf(struct client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_printf(struct client *c, const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out_append(c, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void flush_client(struct client *c)
{
    while (c->fd >= 0 && c->outlen > 0) {
        ssize_t n = send(c->fd, c->out, c->outlen, 0);
        if (n > 0) {
            memmove(c->out, c->out + n, c->outlen - (size_t)n);
            c->outlen -= (size_t)n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            close(c->fd);
            c->fd = -1;
            c->outlen = 0;
        }
    }
}

static void json_text(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (size_t i = 0; src[i] && k + 2 < cap; i++) {
        char ch = src[i];
        if (ch == '"' || ch == '\\' || (unsigned char)ch < 0x20) ch = ' ';
        dst[k++] = ch;
    }
    dst[k] = '\0';
}

static void send_loop(struct client *c, const char *id, struct cz_loop *L, uint64_t *tail)
{
    struct cz_loop_status st;
    cz_loop_read_status(L, &st);
    uint64_t head = atomic_load_explicit(&L->head, memory_order_acquire);
    if (head - *tail > CZ_SAMPLES - 64) *tail = head > CZ_SAMPLES - 64 ? head - (CZ_SAMPLES - 64) : 0;
    out_printf(c, "{\"t\":\"loop\",\"id\":\"%s\",\"ts\":%llu,\"st\":{\"period_ns\":%u,\"cpu\":%d,\"prio\":%d,"
               "\"mode\":%u,\"iter\":%llu,\"miss_total\":%llu,\"miss_win\":%llu,\"overruns\":%llu,"
               "\"lat_last\":%u,\"lat_max_win\":%u,\"lat_max_total\":%u,\"jit_max_win\":%u,\"exec_max_win\":%u,"
               "\"resp_max_win\":%u,\"lat_mean_win\":%.1f,\"win_n\":%llu,\"stale_total\":%llu,\"safe_entries\":%llu,"
               "\"reset_gen\":%llu,\"speed_kmh\":%.2f,\"true_kmh\":%.2f,\"pedal\":%.3f,\"p\":[%.1f,%.1f,%.1f,%.1f],"
               "\"slip\":[%.3f,%.3f,%.3f,%.3f],\"abs\":%u,\"mu\":%.2f,\"torque\":%.2f,\"assist\":%.2f,\"angle\":%.4f},\"s\":[",
               id, (unsigned long long)cz_now_ns(), st.period_ns, st.cpu, st.prio, st.mode,
               (unsigned long long)st.iterations, (unsigned long long)st.misses_total,
               (unsigned long long)st.misses_win, (unsigned long long)st.overruns_total, st.lat_last_ns,
               st.lat_max_win_ns, st.lat_max_total_ns, st.jit_max_win_ns, st.exec_max_win_ns, st.resp_max_win_ns,
               st.lat_mean_win_ns, (unsigned long long)st.win_count, (unsigned long long)st.stale_total,
               (unsigned long long)st.safe_entries, (unsigned long long)st.reset_gen, st.speed_kmh,
               st.true_speed_kmh, st.pedal, st.pressure[0], st.pressure[1], st.pressure[2], st.pressure[3],
               st.slip[0], st.slip[1], st.slip[2], st.slip[3], st.abs_mask, st.mu, st.steer_torque,
               st.assist_torque, st.steer_angle);
    int first = 1, count = 0;
    char buf[96];
    while (*tail < head && count < 1500) {
        struct cz_sample s = L->s[*tail & (CZ_SAMPLES - 1)];
        int n = snprintf(buf, sizeof(buf), "%s[%llu,%u,%u,%d,%u,%u,%u]", first ? "" : ",",
                         (unsigned long long)s.n, s.lat_ns, s.exec_ns, s.jitter_ns, s.flags, s.mode, s.io_ns);
        out_append(c, buf, (size_t)n);
        first = 0;
        (*tail)++;
        count++;
    }
    out_append(c, "]}\n", 3);
}

static void send_events(struct client *c)
{
    struct cz_event ev[64];
    for (int src = 0; src < CZ_NEVSRC; src++) {
        uint64_t lost = 0;
        int n;
        while ((n = cz_events_read(&bus->ev[src], &c->tail_ev[src], ev, 64, &lost)) > 0) {
            for (int i = 0; i < n; i++) {
                char txt[32];
                json_text(txt, sizeof(txt), ev[i].text);
                out_printf(c, "{\"t\":\"ev\",\"ts\":%llu,\"src\":\"%s\",\"kind\":\"%s\",\"proc\":\"%s\",\"pid\":%d,"
                           "\"a\":%d,\"v0\":%lld,\"v1\":%lld,\"text\":\"%s\"}\n",
                           (unsigned long long)ev[i].ts_ns, cz_ev_src_name(src), cz_ev_kind_name(ev[i].kind),
                           ev[i].proc < CZ_NPROC ? cz_proc_name(ev[i].proc) : "-", ev[i].pid, ev[i].arg0,
                           (long long)ev[i].v0, (long long)ev[i].v1, txt);
            }
        }
    }
}

/* ---- per-process CPU and memory ---- */
static uint64_t cpu_prev[CZ_NPROC], cpu_prev_t;
static pid_t cpu_prev_pid[CZ_NPROC];
static uint64_t idle_prev[64];
static int ncpu;

static uint64_t proc_cpu_ns(pid_t pid)
{
    clockid_t cid;
    if (clock_getcpuclockid(pid, &cid) != 0) return 0;
    struct timespec ts;
    if (clock_gettime(cid, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t proc_mem_kb(pid_t pid, uint32_t *threads)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ctl", pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) return 0;
    procfs_info info;
    memset(&info, 0, sizeof(info));
    uint64_t kb = 0;
    if (devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), NULL) == EOK) {
        kb = info.private_mem / 1024u;
        if (threads) *threads = info.num_threads;
    }
    close(fd);
    return kb;
}

static uint64_t idle_ns(int cpu)
{
    /* procnto's first ncpu threads are the per-CPU idle threads. */
    clockid_t cid = ClockId(1, cpu + 1);
    if (cid == -1) return 0;
    struct timespec ts;
    if (clock_gettime(cid, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static char proc_json[8192];

static void build_proc_json(void)
{
    uint64_t now = cz_now_ns();
    double dt = cpu_prev_t ? (double)(now - cpu_prev_t) : 0.0;
    size_t k = 0;
    struct stat st;
    uint64_t freemem = 0;
    if (stat("/proc", &st) == 0) freemem = (uint64_t)st.st_size;
    k += (size_t)snprintf(proc_json + k, sizeof(proc_json) - k,
                          "{\"t\":\"proc\",\"ts\":%llu,\"freemem\":%llu,\"cpus\":[",
                          (unsigned long long)now, (unsigned long long)freemem);
    for (int c = 0; c < ncpu && c < 64; c++) {
        uint64_t idle = idle_ns(c);
        double load = (dt > 0 && idle_prev[c]) ? 100.0 * (1.0 - (double)(idle - idle_prev[c]) / dt) : 0.0;
        if (load < 0) load = 0;
        if (load > 100) load = 100;
        idle_prev[c] = idle;
        k += (size_t)snprintf(proc_json + k, sizeof(proc_json) - k, "%s%.1f", c ? "," : "", load);
    }
    k += (size_t)snprintf(proc_json + k, sizeof(proc_json) - k, "],\"p\":[");
    for (int i = 0; i < CZ_NPROC; i++) {
        struct cz_proc *p = &bus->procs[i];
        pid_t pid = atomic_load(&p->pid);
        uint32_t state = atomic_load(&p->state);
        double pct = 0.0;
        uint64_t mem = 0;
        uint32_t thr = 0;
        if (pid > 0 && state != CZ_PS_DEAD) {
            uint64_t cpu = proc_cpu_ns(pid);
            if (dt > 0 && cpu_prev_pid[i] == pid && cpu >= cpu_prev[i]) pct = 100.0 * (double)(cpu - cpu_prev[i]) / dt;
            cpu_prev[i] = cpu;
            cpu_prev_pid[i] = pid;
            mem = proc_mem_kb(pid, &thr);
        }
        static const char *sn[] = { "stopped", "starting", "running", "dead" };
        k += (size_t)snprintf(proc_json + k, sizeof(proc_json) - k,
                              "%s{\"name\":\"%s\",\"pid\":%d,\"state\":\"%s\",\"prio\":%d,\"pol\":\"%c\",\"crit\":%u,"
                              "\"mask\":%llu,\"uid\":%u,\"aslimit\":%llu,\"restarts\":%u,\"sig\":%d,\"rec_ns\":%llu,"
                              "\"cpu\":%.2f,\"mem_kb\":%llu,\"thr\":%u}",
                              i ? "," : "", p->name, pid, sn[state & 3], p->prio, p->policy ? p->policy : 'o',
                              p->critical, (unsigned long long)p->runmask, p->uid, (unsigned long long)p->as_limit,
                              atomic_load(&p->restarts), atomic_load(&p->last_signal),
                              (unsigned long long)atomic_load(&p->last_recovery_ns), pct,
                              (unsigned long long)mem, thr);
    }
    snprintf(proc_json + k, sizeof(proc_json) - k, "]}\n");
    cpu_prev_t = now;
}

static void send_status(struct client *c)
{
    struct cz_cluster_status cs;
    uint32_t s;
    do { s = cz_seq_read_begin(&bus->cluster.seq); memcpy(&cs, &bus->cluster, sizeof(cs)); }
    while (cz_seq_read_retry(&bus->cluster.seq, s));
    out_printf(c, "{\"t\":\"cluster\",\"frames\":%llu,\"pulses\":%llu,\"media\":%llu,\"lat_us\":%u,"
               "\"lat_max_us\":%u,\"fjit_max_us\":%u,\"speed\":%.1f,\"warn\":%u}\n",
               (unsigned long long)cs.frames, (unsigned long long)cs.brake_pulses,
               (unsigned long long)cs.media_msgs, cs.disp_lat_last_us, cs.disp_lat_max_win_us,
               cs.frame_jit_max_win_us, cs.speed_kmh, cs.warnings);
    struct cz_infot_status is;
    do { s = cz_seq_read_begin(&bus->infot.seq); memcpy(&is, &bus->infot, sizeof(is)); }
    while (cz_seq_read_retry(&bus->infot.seq, s));
    uint64_t now = cz_now_ns();
    out_printf(c, "{\"t\":\"infot\",\"mode\":\"%s\",\"alloc_mb\":%u,\"flood_sent\":%llu,\"tracks\":%llu,\"hb_age_ms\":%.1f}\n",
               cz_fault_name((int)is.mode), is.alloc_mb, (unsigned long long)is.flood_sent,
               (unsigned long long)is.media_tracks,
               is.heartbeat_ns ? (double)(now - is.heartbeat_ns) / 1e6 : -1.0);
}

static int fault_from_name(const char *n)
{
    for (int f = 1; f < CZ_FAULT_MAX; f++) if (strcmp(n, cz_fault_name(f)) == 0) return f;
    return -1;
}

static void handle_line(struct client *c, char *line)
{
    char cmd[32] = "", arg[32] = "";
    int ms = 0;
    sscanf(line, "%31s %31s %d", cmd, arg, &ms);
    if (strcmp(cmd, "ping") == 0) {
        out_printf(c, "{\"t\":\"ack\",\"cmd\":\"ping\",\"ok\":true,\"ts\":%llu}\n", (unsigned long long)cz_now_ns());
    } else if (strcmp(cmd, "reset") == 0) {
        atomic_fetch_add(&bus->reset_req, 1);
        cz_emit(bus, CZ_EVS_TELEMETRY, CZ_EV_STATS_RESET, CZ_P_TELEMETRY, getpid(), 0, 0, 0, "window reset");
        out_printf(c, "{\"t\":\"ack\",\"cmd\":\"reset\",\"ok\":true,\"ts\":%llu}\n", (unsigned long long)cz_now_ns());
    } else if (strcmp(cmd, "fault") == 0) {
        int f = fault_from_name(arg);
        struct cz_reply r = { 0 };
        r.status = EINVAL;
        if (f > 0 && gcoid != -1) {
            struct cz_msg_fault m = { .hdr = { .type = CZ_MSG_FAULT, .proc = CZ_P_TELEMETRY }, .fault = f, .arg_ms = ms };
            if (MsgSend(gcoid, &m, sizeof(m), &r, sizeof(r)) == -1) r.status = errno;
        }
        out_printf(c, "{\"t\":\"ack\",\"cmd\":\"fault\",\"fault\":\"%s\",\"ok\":%s,\"err\":\"%s\",\"t_fault\":%llu}\n",
                   f > 0 ? cz_fault_name(f) : "?", r.status == 0 ? "true" : "false",
                   r.status == 0 ? "" : strerror(r.status), (unsigned long long)r.t_ns);
    } else if (cmd[0]) {
        out_printf(c, "{\"t\":\"ack\",\"cmd\":\"%.16s\",\"ok\":false,\"err\":\"unknown command\"}\n", cmd);
    }
}

static void read_client(struct client *c)
{
    ssize_t n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, 0);
    if (n <= 0) {
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close(c->fd);
        c->fd = -1;
        return;
    }
    c->inlen += (size_t)n;
    c->in[c->inlen] = '\0';
    char *start = c->in, *nl;
    while ((nl = strchr(start, '\n'))) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r') nl[-1] = '\0';
        handle_line(c, start);
        start = nl + 1;
    }
    size_t rest = c->inlen - (size_t)(start - c->in);
    memmove(c->in, start, rest);
    c->inlen = rest;
    if (c->inlen >= sizeof(c->in) - 1) c->inlen = 0;   /* overlong line: drop */
}

static void accept_client(int lfd)
{
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd < 0) { slot = i; break; }
    if (slot < 0) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct client *c = &cl[slot];
    c->fd = fd;
    c->inlen = 0;
    c->outlen = 0;
    uint64_t hb = atomic_load(&bus->brake.head), hs = atomic_load(&bus->steer.head);
    c->tail_brake = hb > 3000 ? hb - 3000 : 0;
    c->tail_steer = hs > 1500 ? hs - 1500 : 0;
    for (int s = 0; s < CZ_NEVSRC; s++) {
        uint64_t h = atomic_load(&bus->ev[s].head);
        c->tail_ev[s] = h > CZ_EVENTS ? h - CZ_EVENTS : 0;
    }
    struct utsname u;
    uname(&u);
    out_printf(c, "{\"t\":\"hello\",\"v\":1,\"system\":\"%s\",\"release\":\"%s\",\"version\":\"%s\",\"machine\":\"%s\","
               "\"node\":\"%s\",\"ncpu\":%d,\"cps\":%llu,\"boot_ns\":%llu,\"now_ns\":%llu,\"brake_period_ns\":%u,"
               "\"steer_period_ns\":%u}\n",
               u.sysname, u.release, u.version, u.machine, u.nodename, ncpu,
               (unsigned long long)bus->cycles_per_sec, (unsigned long long)bus->boot_ns,
               (unsigned long long)cz_now_ns(), CZ_BRAKE_PERIOD_NS, CZ_STEER_PERIOD_NS);
    cz_log(WHO, "dashboard connected (slot %d)", slot);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (cz_set_sched(SCHED_RR, CZ_PRIO_TELEMETRY) == -1) cz_log(WHO, "sched: %s", strerror(errno));
    while (!(bus = cz_bus_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    ncpu = cz_num_cpus();
    gcoid = cz_guardian_connect(CZ_P_TELEMETRY);
    for (int i = 0; i < MAX_CLIENTS; i++) cl[i].fd = -1;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(CZ_TELEMETRY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    while (bind(lfd, (struct sockaddr *)&a, sizeof(a)) == -1) {
        cz_log(WHO, "bind: %s, retrying", strerror(errno));
        sleep(1);
    }
    listen(lfd, 4);
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
    if (gcoid != -1) cz_guardian_ready(gcoid, CZ_P_TELEMETRY);
    cz_log(WHO, "streaming on tcp port %d (io-sock)", CZ_TELEMETRY_PORT);

    uint64_t next = cz_now_ns(), tick = 0;
    for (;;) {
        struct pollfd pf[MAX_CLIENTS + 1];
        int np = 0;
        pf[np].fd = lfd; pf[np].events = POLLIN; np++;
        int map[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cl[i].fd < 0) continue;
            pf[np].fd = cl[i].fd;
            pf[np].events = POLLIN | (cl[i].outlen ? POLLOUT : 0);
            map[np - 1] = i;
            np++;
        }
        uint64_t now = cz_now_ns();
        int timeout = now >= next ? 0 : (int)((next - now) / 1000000ull) + 1;
        int r = poll(pf, (nfds_t)np, timeout);
        if (r > 0) {
            if (pf[0].revents & POLLIN) accept_client(lfd);
            for (int k = 1; k < np; k++) {
                struct client *c = &cl[map[k - 1]];
                if (c->fd < 0) continue;
                if (pf[k].revents & (POLLIN | POLLHUP | POLLERR)) read_client(c);
                if (c->fd >= 0 && (pf[k].revents & POLLOUT)) flush_client(c);
            }
        }
        now = cz_now_ns();
        if (now < next) continue;
        next += 50000000ull;                    /* 20 Hz */
        if (now > next + 500000000ull) next = now + 50000000ull;
        tick++;
        int any = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) any = 1;
        if (!any) continue;
        if (tick % 10 == 0) build_proc_json();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = &cl[i];
            if (c->fd < 0) continue;
            send_loop(c, "brake", &bus->brake, &c->tail_brake);
            send_loop(c, "steer", &bus->steer, &c->tail_steer);
            send_events(c);
            if (tick % 2 == 0) send_status(c);
            if (tick % 10 == 0) out_append(c, proc_json, strlen(proc_json));
            flush_client(c);
        }
    }
    return 0;
}
