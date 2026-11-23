/*
 * Crumple Zone: helpers for supervised processes.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "cz_child.h"
#include "cz.h"
#include "cz_msg.h"
#include "cz_rt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <unistd.h>

int cz_guardian_connect(int proc_id)
{
    const char *g = getenv("CZ_GUARDIAN");
    if (!g) { errno = ENOENT; return -1; }
    int gpid = 0, gchid = 0;
    if (sscanf(g, "%d:%d", &gpid, &gchid) != 2) { errno = EINVAL; return -1; }
    int coid = ConnectAttach(0, gpid, gchid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) return -1;
    struct cz_msg_hello m;
    memset(&m, 0, sizeof(m));
    m.hdr.type = CZ_MSG_HELLO;
    m.hdr.proc = proc_id;
    m.pid = getpid();
    struct cz_reply r;
    if (MsgSend(coid, &m, sizeof(m), &r, sizeof(r)) == -1) {
        int e = errno;
        ConnectDetach(coid);
        errno = e;
        return -1;
    }
    return coid;
}

int cz_guardian_ready(int coid, int proc_id)
{
    struct cz_msg_ready m;
    memset(&m, 0, sizeof(m));
    m.hdr.type = CZ_MSG_READY;
    m.hdr.proc = proc_id;
    m.pid = getpid();
    m.ready_ns = cz_now_ns();
    struct cz_reply r;
    return MsgSend(coid, &m, sizeof(m), &r, sizeof(r)) == -1 ? -1 : 0;
}
