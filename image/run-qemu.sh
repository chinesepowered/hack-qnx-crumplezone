#!/bin/bash
# Crumple Zone: boot the image in QEMU x86_64 (KVM when /dev/kvm is usable,
# otherwise multi-threaded TCG).
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
#
#   image/run-qemu.sh            interactive: QNX console on this terminal (Ctrl-a x quits)
#   image/run-qemu.sh --bg       background: console on tcp 127.0.0.1:4555, logged to build/qemu-console.log
#   image/run-qemu.sh --stop     stop a background instance
#
# User-mode networking forwards host 127.0.0.1:7300 to the telemetry server
# (io-sock) and 127.0.0.1:2222 to sshd, so nothing needs root on the host.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK="$REPO/build/image"
PIDFILE="$REPO/build/qemu.pid"
LOG="$REPO/build/qemu-console.log"
SMP="${CZ_SMP:-3}"
TELEMETRY_PORT="${CZ_TELEMETRY_PORT:-7300}"
SSH_PORT="${CZ_SSH_PORT:-2222}"
CONSOLE_PORT="${CZ_CONSOLE_PORT:-4555}"

if [ "${1:-}" = "--stop" ]; then
    if [ -f "$PIDFILE" ]; then kill "$(cat "$PIDFILE")" 2>/dev/null || true; rm -f "$PIDFILE"; fi
    exit 0
fi

[ -f "$WORK/output/ifs.bin" ] || { echo "no image: run make image first" >&2; exit 1; }

mkdir -p "$REPO/build"
if [ -w /dev/kvm ]; then
    ACCEL=(-accel kvm -cpu host)
    echo "accelerator: KVM"
    echo KVM > "$REPO/build/qemu-accel.txt"
else
    # TCG. IvyBridge: AVX but no AVX2. With -cpu max, OpenSSL's AVX2 code paths
    # crash under QEMU 8.2 TCG (sshd dies with SIGSEGV); IvyBridge avoids them.
    ACCEL=(-accel tcg,thread=multi -cpu IvyBridge-IBRS)
    echo "accelerator: TCG (no usable /dev/kvm)"
    echo TCG > "$REPO/build/qemu-accel.txt"
fi

# -mem-prealloc backs all guest RAM up front: otherwise the host takes a page
# fault the first time the guest touches each page (190 MB during the memory
# leak fault), and those host-side stalls freeze other vCPUs too.
ARGS=(-name crumple,debug-threads=on -smp "$SMP" -m 2G -mem-prealloc "${ACCEL[@]}"
      -drive "file=$WORK/output/disk-qemu.vmdk,if=ide,id=drv0"
      -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${TELEMETRY_PORT}-:7300,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22"
      -device virtio-net-pci,netdev=net0
      -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
      -kernel "$WORK/output/ifs.bin" -display none)

if [ "${1:-}" = "--bg" ]; then
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "already running (pid $(cat "$PIDFILE"))"
        exit 0
    fi
    rm -f "$LOG"
    qemu-system-x86_64 "${ARGS[@]}" \
        -chardev "socket,id=ser0,host=127.0.0.1,port=${CONSOLE_PORT},server=on,wait=off,logfile=$LOG" \
        -serial chardev:ser0 -monitor "unix:$REPO/build/qemu-monitor.sock,server=on,wait=off" \
        -pidfile "$PIDFILE" -daemonize 2>"$REPO/build/qemu-stderr.log"
    echo "QEMU started (pid $(cat "$PIDFILE")); console tcp 127.0.0.1:${CONSOLE_PORT}, log $LOG"
    [ "${CZ_HOST_RT:-1}" = 1 ] && "$REPO/scripts/host-rt.sh" "$(cat "$PIDFILE")" || true
    for _ in $(seq 1 120); do
        grep -a -q "streaming on tcp port" "$LOG" 2>/dev/null && { echo "Crumple Zone is up"; exit 0; }
        sleep 1
    done
    echo "timed out waiting for the demo to come up; see $LOG" >&2
    exit 1
else
    exec qemu-system-x86_64 "${ARGS[@]}" -serial mon:stdio
fi
