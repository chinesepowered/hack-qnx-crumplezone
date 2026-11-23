/*
 * Crumple Zone: name tables shared by all processes (portable C).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "cz.h"

static const char *const proc_names[CZ_NPROC] = {
    "vehicle_sim", "wheel_sensors", "brake_ctl", "steer_ctl",
    "cluster", "telemetry", "infotainment", "guardian",
};

static const char *const fault_names[CZ_FAULT_MAX] = {
    "none", "crash", "storm", "leak", "flood", "kill_sensor",
};

static const char *const kind_names[CZ_EV_KIND_MAX] = {
    "none", "boot", "spawn", "ready", "death", "restart", "fault_inject", "fault_end",
    "safe_enter", "safe_exit", "deadline_miss", "stats_reset", "trace", "exit_status", "info",
};

static const char *const src_names[CZ_NEVSRC] = {
    "guardian", "brake_ctl", "steer_ctl", "cluster", "telemetry", "wheel_sensors",
};

static const char *const mode_names[] = { "NORMAL", "SAFE_HOLD", "SAFE_STOP", "RESUMING" };

const char *cz_proc_name(int id)
{
    return (id >= 0 && id < CZ_NPROC) ? proc_names[id] : "?";
}

const char *cz_fault_name(int f)
{
    return (f >= 0 && f < CZ_FAULT_MAX) ? fault_names[f] : "?";
}

const char *cz_ev_kind_name(int k)
{
    return (k >= 0 && k < CZ_EV_KIND_MAX) ? kind_names[k] : "?";
}

const char *cz_ev_src_name(int s)
{
    return (s >= 0 && s < CZ_NEVSRC) ? src_names[s] : "?";
}

const char *cz_mode_name(int m)
{
    return (m >= 0 && m < 4) ? mode_names[m] : "?";
}
