/*
 * cluster: instrument cluster feed (speed, warnings), medium criticality.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * One server thread on a channel registered with name_attach(). It receives:
 *   - CZ_PULSE_BRAKE doorbells from brake_ctl (priority 31, every 10 ms)
 *   - a 60 Hz frame timer pulse
 *   - CZ_MSG_MEDIA messages from infotainment (MsgSend, client priority 8)
 * The kernel queues messages and pulses on the channel in priority order and
 * the receiving thread runs at the priority of what it received, so a flood
 * of infotainment messages is served at infotainment's priority and only in
 * time nobody more important wants: the sender is blocked in SEND or REPLY
 * state until the cluster gets to it, which is back-pressure for free.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cz.h"
#include "cz_bus.h"
#include "cz_child.h"
#include "cz_msg.h"
#include "cz_rt.h"

#define WHO "cluster"

union rx {
    struct _pulse pulse;
    struct cz_hdr hdr;
    struct cz_msg_media media;
    uint16_t type;
    char raw[sizeof(struct cz_msg_media) + 64];
};

int main(void)
{
    if (cz_set_sched(SCHED_RR, CZ_PRIO_CLUSTER) == -1)
        cz_log(WHO, "sched: %s", strerror(errno));

    struct cz_bus *bus;
    while (!(bus = cz_bus_open(1))) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    int gcoid = cz_guardian_connect(CZ_P_CLUSTER);

    name_attach_t *att = name_attach(NULL, CZ_CLUSTER_NAME, 0);
    if (!att) { cz_log(WHO, "name_attach: %s", strerror(errno)); return 1; }
    int self = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    uint64_t start = cz_align_up(cz_now_ns() + 5000000ull, 1000000ull);
    if (cz_periodic_pulse_timer(self, CZ_PRIO_CLUSTER, CZ_PULSE_FRAME, start, CZ_CLUSTER_PERIOD_NS, NULL) == -1)
        cz_log(WHO, "frame timer: %s", strerror(errno));

    if (gcoid != -1) cz_guardian_ready(gcoid, CZ_P_CLUSTER);
    cz_log(WHO, "registered /dev/name/local/%s, 60 Hz frames", CZ_CLUSTER_NAME);

    struct cz_cluster_status *C = &bus->cluster;
    uint64_t frames = 0, pulses = 0, media = 0, reset_gen = atomic_load(&bus->reset_req);
    uint32_t lat_last = 0, lat_max = 0, fjit_max = 0;
    uint64_t prev_frame = 0;
    uint32_t checksum = 0;
    static union rx msg;

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) continue;
        uint64_t now = cz_now_ns();

        uint64_t rq = atomic_load_explicit(&bus->reset_req, memory_order_relaxed);
        if (rq != reset_gen) { reset_gen = rq; lat_max = 0; fjit_max = 0; }

        if (rcvid == 0) {
            switch (msg.pulse.code) {
            case CZ_PULSE_BRAKE: {
                uint32_t sent_us = (uint32_t)msg.pulse.value.sival_int;
                lat_last = (uint32_t)(now / 1000ull) - sent_us;
                if (lat_last > lat_max) lat_max = lat_last;
                pulses++;
                break;
            }
            case CZ_PULSE_FRAME: {
                if (prev_frame) {
                    int64_t d = (int64_t)(now - prev_frame) - (int64_t)CZ_CLUSTER_PERIOD_NS;
                    uint32_t a = (uint32_t)((d < 0 ? -d : d) / 1000);
                    if (a > fjit_max) fjit_max = a;
                }
                prev_frame = now;
                frames++;
                struct cz_loop_status b, s;
                cz_loop_read_status(&bus->brake, &b);
                cz_loop_read_status(&bus->steer, &s);
                uint32_t warn = 0;
                if (b.abs_mask) warn |= 1u;
                if (b.mode != CZ_MODE_NORMAL) warn |= 2u;
                if (s.mode != CZ_MODE_NORMAL) warn |= 4u;
                cz_seq_write_begin(&C->seq);
                C->frames = frames;
                C->brake_pulses = pulses;
                C->media_msgs = media;
                C->disp_lat_last_us = lat_last;
                C->disp_lat_max_win_us = lat_max;
                C->frame_jit_max_win_us = fjit_max;
                C->reset_gen = reset_gen;
                C->speed_kmh = b.speed_kmh;
                C->warnings = warn;
                cz_seq_write_end(&C->seq);
                break;
            }
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(msg.pulse.scoid);
                break;
            default:
                break;
            }
            continue;
        }

        if (msg.type == _IO_CONNECT) {            /* from name_open() */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.type > _IO_BASE && msg.type <= _IO_MAX) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        if (msg.hdr.type == CZ_MSG_MEDIA) {
            /* Parse the media update: cheap, bounded work. */
            uint32_t len = msg.media.len < sizeof(msg.media.payload) ? msg.media.len : sizeof(msg.media.payload);
            for (uint32_t i = 0; i < len; i += 16) checksum = checksum * 31u + (uint8_t)msg.media.payload[i];
            media++;
            struct cz_reply r = { 0 };
            r.arg = (int32_t)checksum;
            MsgReply(rcvid, EOK, &r, sizeof(r));
            continue;
        }
        MsgError(rcvid, ENOSYS);
    }
    return 0;
}
