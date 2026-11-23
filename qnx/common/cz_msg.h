/*
 * Crumple Zone: Neutrino message formats.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Message types start above _IO_MAX so they can never be confused with the
 * resource manager I/O messages the kernel and libc generate.
 */
#ifndef CZ_MSG_H
#define CZ_MSG_H

#include <stdint.h>
#include <sys/iomsg.h>

#define CZ_MSG_BASE        (_IO_MAX + 0x100)

/* Child -> guardian */
#define CZ_MSG_HELLO       (CZ_MSG_BASE + 1)  /* register this connection, reply carries config */
#define CZ_MSG_READY       (CZ_MSG_BASE + 2)  /* initialisation complete */
#define CZ_MSG_CMD_WAIT    (CZ_MSG_BASE + 3)  /* infotainment command thread: reply-blocked until a command */
#define CZ_MSG_HEARTBEAT   (CZ_MSG_BASE + 4)  /* infotainment status */
/* Operator (telemetry, czctl) -> guardian */
#define CZ_MSG_FAULT       (CZ_MSG_BASE + 10) /* inject a fault */
#define CZ_MSG_STATUS      (CZ_MSG_BASE + 11) /* query process table */
/* Infotainment -> cluster */
#define CZ_MSG_MEDIA       (CZ_MSG_BASE + 20)

/* Pulse codes (user pulse codes must be >= _PULSE_CODE_MINAVAIL) */
#define CZ_PULSE_TICK      (_PULSE_CODE_MINAVAIL + 1)
#define CZ_PULSE_BRAKE     (_PULSE_CODE_MINAVAIL + 2)  /* brake_ctl -> cluster doorbell */
#define CZ_PULSE_STEER     (_PULSE_CODE_MINAVAIL + 3)
#define CZ_PULSE_DEATH     (_PULSE_CODE_MINAVAIL + 4)  /* procmgr process-death notification */
#define CZ_PULSE_REAPED    (_PULSE_CODE_MINAVAIL + 5)  /* reaper thread collected an exit status */
#define CZ_PULSE_FRAME     (_PULSE_CODE_MINAVAIL + 6)

#define CZ_CLUSTER_NAME    "crumple_cluster"

struct cz_hdr {
    uint16_t type;
    uint16_t subtype;
    int32_t  proc;       /* enum cz_proc_id of the sender */
};

struct cz_msg_hello {
    struct cz_hdr hdr;
    int32_t pid;
    int32_t pad;
};

struct cz_msg_ready {
    struct cz_hdr hdr;
    int32_t pid;
    int32_t pad;
    uint64_t ready_ns;
};

struct cz_msg_heartbeat {
    struct cz_hdr hdr;
    uint32_t mode;
    uint32_t alloc_mb;
    uint64_t flood_sent;
    uint64_t media_tracks;
};

struct cz_msg_fault {
    struct cz_hdr hdr;
    int32_t fault;       /* enum cz_fault */
    int32_t arg_ms;
};

struct cz_reply {
    int32_t status;      /* 0 ok, else errno */
    int32_t cmd;         /* for CZ_MSG_CMD_WAIT: enum cz_fault to execute */
    int32_t arg;
    int32_t pad;
    uint64_t t_ns;       /* fault injection timestamp */
};

struct cz_msg_media {
    struct cz_hdr hdr;
    uint32_t track;
    uint32_t len;
    char payload[1024];  /* album art chunk, metadata, ... */
};

#endif /* CZ_MSG_H */
