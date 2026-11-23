/*
 * wheel_sensors: chassis sensor driver, written as a QNX resource manager.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Registers /dev/vehicle/wheel_speed, /dev/vehicle/pedal and
 * /dev/vehicle/steering in the pathname space. Controllers open() them and
 * read() fixed-size binary samples; each read() is a MsgSend() that this
 * driver answers with MsgReply() through the iofunc/dispatch framework, and
 * the handler runs at the client's priority (priority inheritance), so the
 * 1 kHz brake loop is served at brake priority. Reading with any other size,
 * for example `cat /dev/vehicle/wheel_speed` in the shell, returns one line
 * of text.
 */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/iofunc.h>      /* before dispatch.h so RESMGR_OCB_T is iofunc_ocb_t */
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/stat.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_rt.h"

#define WHO "wheel_sensors"

enum { DEV_WHEEL, DEV_PEDAL, DEV_STEER, NDEV };

struct dev_attr {
    iofunc_attr_t attr;      /* must be first: the framework casts to iofunc_attr_t */
    int kind;
};

static struct cz_hw *hw;
static struct dev_attr devs[NDEV];
static uint32_t served[NDEV];

static void snapshot(struct cz_hw *h, struct cz_hw *out)
{
    uint32_t s;
    do {
        s = cz_seq_read_begin(&h->sseq);
        memcpy((char *)out + offsetof(struct cz_hw, sample_count),
               (char *)h + offsetof(struct cz_hw, sample_count),
               offsetof(struct cz_hw, brake_cmd_bits) - offsetof(struct cz_hw, sample_count));
    } while (cz_seq_read_retry(&h->sseq, s));
}

static int plausible(float v, float lo, float hi)
{
    return v == v && v >= lo && v <= hi;   /* v == v rejects NaN */
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    struct dev_attr *d = (struct dev_attr *)ocb->attr;
    size_t want = _IO_READ_GET_NBYTES(msg);
    struct cz_hw snap;
    snapshot(hw, &snap);
    served[d->kind]++;

    union {
        struct cz_wheel_sample w;
        struct cz_pedal_sample p;
        struct cz_steer_sample s;
        char text[200];
    } out;
    size_t bin_size = 0;
    memset(&out, 0, sizeof(out));

    switch (d->kind) {
    case DEV_WHEEL:
        out.w.sample_ns = snap.sample_ns;
        out.w.seq = snap.sample_count;
        for (int i = 0; i < 4; i++) {
            out.w.speed[i] = snap.wheel_speed[i];
            if (!plausible(snap.wheel_speed[i], 0.0f, 90.0f)) out.w.status = CZ_SENSOR_IMPLAUSIBLE;
        }
        bin_size = sizeof(out.w);
        break;
    case DEV_PEDAL:
        out.p.sample_ns = snap.sample_ns;
        out.p.seq = snap.sample_count;
        out.p.pedal = snap.pedal;
        if (!plausible(snap.pedal, 0.0f, 1.0f)) out.p.status = CZ_SENSOR_IMPLAUSIBLE;
        bin_size = sizeof(out.p);
        break;
    default:
        out.s.sample_ns = snap.sample_ns;
        out.s.seq = snap.sample_count;
        out.s.torque = snap.steer_torque;
        out.s.angle = snap.steer_angle;
        out.s.rate = snap.steer_rate;
        out.s.speed = 0.25f * (snap.wheel_speed[0] + snap.wheel_speed[1] + snap.wheel_speed[2] + snap.wheel_speed[3]);
        if (!plausible(snap.steer_torque, -50.0f, 50.0f)) out.s.status = CZ_SENSOR_IMPLAUSIBLE;
        bin_size = sizeof(out.s);
        break;
    }

    if (want == bin_size) {
        /* Binary sample for controllers: no offset, every read is a fresh sample. */
        _IO_SET_READ_NBYTES(ctp, bin_size);
        return _RESMGR_PTR(ctp, &out, bin_size);
    }

    /* Human-readable form, one line per open(). */
    if (ocb->offset > 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    char line[200];
    int n;
    if (d->kind == DEV_WHEEL)
        n = snprintf(line, sizeof(line), "seq=%u t=%.6fs FL=%.2f FR=%.2f RL=%.2f RR=%.2f m/s\n",
                     snap.sample_count, (double)snap.sample_ns / 1e9, snap.wheel_speed[0],
                     snap.wheel_speed[1], snap.wheel_speed[2], snap.wheel_speed[3]);
    else if (d->kind == DEV_PEDAL)
        n = snprintf(line, sizeof(line), "seq=%u t=%.6fs pedal=%.3f throttle=%.3f\n",
                     snap.sample_count, (double)snap.sample_ns / 1e9, snap.pedal, snap.throttle);
    else
        n = snprintf(line, sizeof(line), "seq=%u t=%.6fs torque=%.2f Nm angle=%.4f rad\n",
                     snap.sample_count, (double)snap.sample_ns / 1e9, snap.steer_torque, snap.steer_angle);
    size_t nb = (size_t)n < want ? (size_t)n : want;
    ocb->offset += nb;
    _IO_SET_READ_NBYTES(ctp, nb);
    return _RESMGR_PTR(ctp, line, nb);
}

/* One dispatch channel and one service thread per consumer CPU: brake_ctl's
 * devices are served by a thread that lives on the brake CPU, the steering
 * device by a thread on the steering CPU. A read() is then a same-CPU
 * MsgSend/MsgReply handoff instead of a cross-CPU wakeup. */
struct server {
    const char *name;
    int cpu;
    dispatch_t *dpp;
};

static dispatch_t *make_dispatch(void)
{
    /* Our own channel so we can add _NTO_CHF_INHERIT_RUNMASK: the receiving
     * thread takes on the sender's runmask, so it always runs on the client's
     * CPU. UNBLOCK and DISCONNECT are required for a resource manager channel. */
    int chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_INHERIT_RUNMASK);
    if (chid == -1) return NULL;
    return dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);
}

static void *serve(void *arg)
{
    struct server *sv = arg;
    cz_set_sched(SCHED_RR, CZ_PRIO_SENSORS);
    cz_pin_cpu(sv->cpu);
    dispatch_context_t *ctp = dispatch_context_alloc(sv->dpp);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            cz_log(WHO, "%s dispatch_block: %s", sv->name, strerror(errno));
            return NULL;
        }
        dispatch_handler(ctp);
    }
    return NULL;
}

int main(void)
{
    if (cz_set_sched(SCHED_RR, CZ_PRIO_SENSORS) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));

    for (int tries = 0; !(hw = cz_hw_open(0)); tries++) {
        if (tries > 200) { cz_log(WHO, "no hardware at %s", CZ_HW_NAME); return 1; }
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }

    int gcoid = cz_guardian_connect(CZ_P_WHEEL_SENSORS);
    if (gcoid == -1) cz_log(WHO, "guardian connect: %s (running unsupervised)", strerror(errno));

    int ncpu = cz_num_cpus();
    static struct server brake_srv, steer_srv;
    brake_srv.name = "brake";
    brake_srv.cpu = ncpu - 1;
    steer_srv.name = "steer";
    steer_srv.cpu = ncpu >= 3 ? ncpu - 2 : 0;
    brake_srv.dpp = make_dispatch();
    steer_srv.dpp = make_dispatch();
    if (!brake_srv.dpp || !steer_srv.dpp) { cz_log(WHO, "dispatch: %s", strerror(errno)); return 1; }

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;

    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t iofuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &iofuncs);
    iofuncs.read = io_read;
    iofuncs.read64 = io_read;

    static const char *paths[NDEV] = { CZ_PATH_WHEEL, CZ_PATH_PEDAL, CZ_PATH_STEER };
    for (int i = 0; i < NDEV; i++) {
        iofunc_attr_init(&devs[i].attr, S_IFCHR | 0444, NULL, NULL);
        devs[i].kind = i;
        dispatch_t *dpp = (i == DEV_STEER) ? steer_srv.dpp : brake_srv.dpp;
        if (resmgr_attach(dpp, &rattr, paths[i], _FTYPE_ANY, 0, &cfuncs, &iofuncs, &devs[i].attr) == -1) {
            cz_log(WHO, "resmgr_attach %s: %s", paths[i], strerror(errno));
            return 1;
        }
    }

    pthread_t th;
    pthread_create(&th, NULL, serve, &steer_srv);
    if (gcoid != -1) cz_guardian_ready(gcoid, CZ_P_WHEEL_SENSORS);
    cz_log(WHO, "serving %s, %s (cpu %d) and %s (cpu %d), pid %d", paths[0], paths[1], brake_srv.cpu,
           paths[2], steer_srv.cpu, getpid());
    serve(&brake_srv);
    return 1;
}
