/*
 * Crumple Zone: shared definitions for every process in the demo.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The system is split into processes of different criticality. They share:
 *   - /crumple_hw  : simulated chassis hardware registers (owned by vehicle_sim)
 *   - /crumple_bus : telemetry bus (owned by guardian), one single-writer region
 *                    per producer, read by telemetry and cluster
 * Control commands and supervision use Neutrino message passing (see cz_msg.h).
 */
#ifndef CZ_H
#define CZ_H

#include <stdint.h>
#include <stdatomic.h>

#define CZ_BUS_NAME     "/crumple_bus"
#define CZ_HW_NAME      "/crumple_hw"
#define CZ_BUS_MAGIC    0x435a4255u /* "CZBU" */
#define CZ_HW_MAGIC     0x435a4857u /* "CZHW" */
#define CZ_BUS_VERSION  3u

#define CZ_SAMPLES      8192u   /* per control loop sample ring (power of two) */
#define CZ_EVENTS       512u    /* per producer event ring (power of two) */
#define CZ_TELEMETRY_PORT 7300

/* ---------- Scheduling plan (priorities are QNX priorities, 1..255) ---------- */
#define CZ_PRIO_VEHICLE_SIM   63  /* stands in for physics, not part of the ECU */
#define CZ_PRIO_BRAKE         60  /* SCHED_FIFO, owns the last CPU */
#define CZ_PRIO_SENSORS       58  /* resmgr threads also inherit client priority */
#define CZ_PRIO_STEER         55  /* SCHED_FIFO */
#define CZ_PRIO_GUARDIAN      50
#define CZ_PRIO_BRAKE_PULSE   31  /* priority of the brake -> cluster pulses */
#define CZ_PRIO_CLUSTER       30
#define CZ_PRIO_TELEMETRY     20
#define CZ_PRIO_INFOTAINMENT   8  /* below the shell (10), so the console stays usable */

#define CZ_BRAKE_PERIOD_NS    1000000u   /* 1 kHz */
#define CZ_STEER_PERIOD_NS    2000000u   /* 500 Hz */
#define CZ_PLANT_PERIOD_NS    1000000u   /* 1 kHz physics */
#define CZ_CLUSTER_PERIOD_NS 16666667u   /* 60 Hz frames */

/* Memory budget the guardian imposes on infotainment with prlimit(RLIMIT_AS). */
#define CZ_INFOTAINMENT_AS_LIMIT (192u * 1024u * 1024u)
#define CZ_INFOTAINMENT_UID 1500
#define CZ_INFOTAINMENT_GID 1500

/* Recovery budget used for pass/fail reporting (ms). */
#define CZ_RECOVERY_BUDGET_NS (50u * 1000000u)

/* ---------- Process identities ---------- */
enum cz_proc_id {
    CZ_P_VEHICLE_SIM = 0,
    CZ_P_WHEEL_SENSORS,
    CZ_P_BRAKE,
    CZ_P_STEER,
    CZ_P_CLUSTER,
    CZ_P_TELEMETRY,
    CZ_P_INFOTAINMENT,
    CZ_P_GUARDIAN,
    CZ_NPROC
};

/* Event producers: each owns one ring in the bus (single writer). */
enum cz_ev_src {
    CZ_EVS_GUARDIAN = 0,
    CZ_EVS_BRAKE,
    CZ_EVS_STEER,
    CZ_EVS_CLUSTER,
    CZ_EVS_TELEMETRY,
    CZ_EVS_SENSORS,
    CZ_NEVSRC
};

enum cz_ev_kind {
    CZ_EV_NONE = 0,
    CZ_EV_BOOT,          /* guardian brought the system up */
    CZ_EV_SPAWN,         /* process spawned: pid */
    CZ_EV_READY,         /* process reported ready: v0 = spawn->ready ns */
    CZ_EV_DEATH,         /* process died: arg0 = signal (or -exit code), v0 = fault->detect ns or -1 */
    CZ_EV_RESTART,       /* restart complete: v0 = fault->ready ns, v1 = detect->ready ns */
    CZ_EV_FAULT_INJECT,  /* arg0 = enum cz_fault, v0 = arg (ms) */
    CZ_EV_FAULT_END,     /* arg0 = enum cz_fault */
    CZ_EV_SAFE_ENTER,    /* control loop entered safe state: arg0 = reason, v0 = age of last good sample ns */
    CZ_EV_SAFE_EXIT,     /* normal control resumed: v0 = time spent in safe state ns */
    CZ_EV_DEADLINE_MISS, /* v0 = response time ns, v1 = iteration */
    CZ_EV_STATS_RESET,
    CZ_EV_TRACE,         /* arg0 = 1 start / 0 stop */
    CZ_EV_EXIT_STATUS,   /* reaper: arg0 = signal or -exit code, pid */
    CZ_EV_INFO,          /* free text */
    CZ_EV_KIND_MAX
};

enum cz_fault {
    CZ_FAULT_NONE = 0,
    CZ_FAULT_CRASH,        /* infotainment segfault */
    CZ_FAULT_STORM,        /* infotainment spins every core */
    CZ_FAULT_LEAK,         /* infotainment leaks until RLIMIT_AS */
    CZ_FAULT_FLOOD,        /* infotainment floods the cluster with messages */
    CZ_FAULT_KILL_SENSOR,  /* SIGKILL the wheel sensor driver */
    CZ_FAULT_MAX
};

enum cz_proc_state {
    CZ_PS_STOPPED = 0,
    CZ_PS_STARTING,
    CZ_PS_RUNNING,
    CZ_PS_DEAD
};

/* Brake loop modes (also used by steering where it applies). */
enum cz_mode {
    CZ_MODE_NORMAL = 0,
    CZ_MODE_SAFE_HOLD,   /* inputs stale: hold last valid command */
    CZ_MODE_SAFE_STOP,   /* inputs stale for too long: controlled stop */
    CZ_MODE_RESUMING     /* inputs back, waiting for N consecutive good samples */
};

enum cz_safe_reason {
    CZ_SAFE_NONE = 0,
    CZ_SAFE_READ_ERROR,  /* read() on the sensor fd failed (driver gone) */
    CZ_SAFE_TIMEOUT,     /* TimerTimeout() expired: driver hung */
    CZ_SAFE_STALE,       /* data arrived but the sample is too old */
    CZ_SAFE_NO_DRIVER    /* no connection to the driver */
};

/* Sample flags */
#define CZ_SF_MISS   0x0001u  /* deadline missed */
#define CZ_SF_STALE  0x0002u  /* inputs stale this iteration */
#define CZ_SF_SAFE   0x0004u  /* in a safe mode this iteration */
#define CZ_SF_OVERRUN 0x0008u /* timer overrun detected */

/* ---------- Simulated chassis hardware (/crumple_hw) ---------- */
struct cz_hw {
    uint32_t magic;
    uint32_t version;
    /* Sensor registers, written by vehicle_sim under a seqlock. */
    _Atomic uint32_t sseq;
    uint32_t sample_count;
    uint64_t sample_ns;           /* CLOCK_MONOTONIC when sampled */
    float wheel_speed[4];         /* m/s at the rim: FL FR RL RR */
    float pedal;                  /* brake pedal 0..1 */
    float throttle;               /* 0..1 (driver model) */
    float steer_torque;           /* driver torque at the column, Nm */
    float steer_angle;            /* rad */
    float steer_rate;             /* rad/s */
    float true_speed;             /* m/s, ground truth for display only */
    float mu;                     /* road friction, ground truth for display only */
    float sim_time;               /* s */
    /* Actuator registers. Each is written by exactly one controller. */
    _Atomic uint32_t brake_cmd_bits[4]; /* float bar, stored as raw bits */
    _Atomic uint64_t brake_cmd_ns;
    _Atomic uint32_t steer_cmd_bits;    /* float Nm assist torque */
    _Atomic uint64_t steer_cmd_ns;
};

/* ---------- Telemetry bus (/crumple_bus) ---------- */
struct cz_sample {
    uint64_t n;          /* iteration index since loop start */
    uint64_t t_ns;       /* wake-up time */
    uint32_t lat_ns;     /* wake-up minus scheduled release */
    uint32_t exec_ns;    /* compute time after wake-up */
    uint32_t io_ns;      /* time spent in sensor reads (resource manager round trips) */
    int32_t  jitter_ns;  /* period minus nominal period */
    uint16_t flags;
    uint16_t mode;
};

struct cz_loop_status {
    uint64_t start_ns;          /* release of iteration 0: release(n) = start_ns + n * period_ns */
    uint32_t period_ns;
    int32_t  cpu;
    int32_t  prio;
    uint32_t mode;
    uint64_t iterations;
    uint64_t misses_total;      /* deadline misses since boot */
    uint64_t misses_win;        /* since last stats reset */
    uint64_t overruns_total;
    uint32_t lat_last_ns;
    uint32_t lat_max_win_ns;
    uint32_t lat_max_total_ns;
    uint32_t jit_max_win_ns;    /* max |period - nominal| */
    uint32_t exec_max_win_ns;
    uint32_t resp_max_win_ns;   /* max (completion - release) */
    double   lat_mean_win_ns;
    uint64_t win_count;
    uint64_t stale_total;       /* iterations with stale inputs */
    uint64_t off_cpu;           /* iterations observed running on a CPU other than the pinned one */
    uint64_t safe_entries;
    uint64_t reset_gen;
    uint64_t publish_ns;
    /* Vehicle / control values for display */
    float speed_kmh;
    float vref;
    float pedal;
    float pressure[4];
    float slip[4];
    uint32_t abs_mask;
    float steer_torque;
    float assist_torque;
    float steer_angle;
    float true_speed_kmh;
    float mu;
};

struct cz_loop {
    _Atomic uint32_t seq;
    struct cz_loop_status st;
    _Atomic uint64_t head;
    struct cz_sample s[CZ_SAMPLES];
};

struct cz_event {
    uint64_t ts_ns;
    uint16_t kind;
    uint16_t proc;
    int32_t  pid;
    int32_t  arg0;
    int32_t  pad;
    int64_t  v0;
    int64_t  v1;
    char     text[24];
};

struct cz_evring {
    _Atomic uint64_t head;
    struct cz_event ev[CZ_EVENTS];
};

struct cz_proc {
    char name[16];
    _Atomic int32_t pid;
    _Atomic uint32_t state;
    _Atomic uint32_t restarts;
    _Atomic int32_t last_signal;
    _Atomic uint64_t last_recovery_ns;   /* fault (or detection) -> ready */
    _Atomic uint64_t last_ready_ns;
    int32_t prio;
    char policy;                          /* 'f' FIFO, 'r' RR, 'o' other */
    uint8_t critical;                     /* 1 = safety relevant */
    uint16_t pad;
    uint64_t runmask;                     /* 0 = any CPU */
    uint32_t uid;
    uint32_t pad2;
    uint64_t as_limit;                    /* RLIMIT_AS imposed by guardian, 0 = none */
};

struct cz_cluster_status {
    _Atomic uint32_t seq;
    uint64_t frames;
    uint64_t brake_pulses;
    uint64_t media_msgs;
    uint32_t disp_lat_last_us;            /* brake publish -> cluster receipt */
    uint32_t disp_lat_max_win_us;
    uint32_t frame_jit_max_win_us;
    uint32_t pad;
    uint64_t reset_gen;
    float speed_kmh;
    uint32_t warnings;                    /* bitmask: 1 ABS, 2 brake safe mode, 4 steer safe */
};

struct cz_infot_status {                  /* written by guardian from heartbeats */
    _Atomic uint32_t seq;
    uint32_t mode;                        /* enum cz_fault currently executing */
    uint32_t alloc_mb;
    uint32_t pad;
    uint64_t flood_sent;
    uint64_t heartbeat_ns;
    uint64_t media_tracks;
};

struct cz_bus {
    uint32_t magic;
    uint32_t version;
    uint64_t boot_ns;
    int32_t  guardian_pid;
    int32_t  guardian_chid;
    int32_t  ncpu;
    uint32_t pad;
    uint64_t cycles_per_sec;
    _Atomic uint64_t reset_req;           /* bump to reset all windowed stats */
    struct cz_proc procs[CZ_NPROC];
    struct cz_loop brake;
    struct cz_loop steer;
    struct cz_cluster_status cluster;
    struct cz_infot_status infot;
    struct cz_evring ev[CZ_NEVSRC];
};

static inline float cz_bits_to_f(uint32_t b) { union { uint32_t u; float f; } x; x.u = b; return x.f; }
static inline uint32_t cz_f_to_bits(float f) { union { uint32_t u; float f; } x; x.f = f; return x.u; }

const char *cz_proc_name(int id);
const char *cz_fault_name(int f);
const char *cz_ev_kind_name(int k);
const char *cz_ev_src_name(int s);
const char *cz_mode_name(int m);

#endif /* CZ_H */
