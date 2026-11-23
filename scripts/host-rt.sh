#!/bin/bash
# Crumple Zone: keep host scheduling noise out of the guest's timing (TCG only).
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
#
# Under TCG every guest CPU is a host thread. If the host deschedules that
# thread, the guest sees it as latency that QNX never caused. This pins each
# QEMU vCPU thread ("CPU n/TCG", named by -name ...,debug-threads=on) to its
# own host core with SCHED_FIFO 10, and gives the QEMU main loop, which
# delivers timer interrupts, SCHED_FIFO 20. The highest host core is left for
# the dashboard and the recorder. All scheduling inside the guest is QNX's.
# Needs CAP_SYS_NICE; prints a warning and does nothing if it is missing.
set -u
QPID="${1:?usage: host-rt.sh <qemu-pid>}"
if ! chrt -f 1 true 2>/dev/null; then
    echo "host-rt: no permission for SCHED_FIFO on the host; timing will include host noise" >&2
    exit 0
fi
chrt -f -p 20 "$QPID" >/dev/null
for t in /proc/"$QPID"/task/*; do
    tid=$(basename "$t")
    comm=$(cat "$t/comm" 2>/dev/null)
    if [[ "$comm" =~ ^CPU\ ([0-9]+)/ ]]; then
        cpu=${BASH_REMATCH[1]}
        taskset -p -c "$cpu" "$tid" >/dev/null
        chrt -f -p 10 "$tid" >/dev/null
        echo "host-rt: vCPU $cpu (tid $tid) -> host core $cpu, SCHED_FIFO 10"
    fi
done
echo "host-rt: QEMU main loop (pid $QPID) SCHED_FIFO 20"
