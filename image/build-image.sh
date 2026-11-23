#!/bin/bash
# Crumple Zone: build the QNX OS 8.0 QEMU x86_64 image with mkqnximage.
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
#
# Requires the SDP environment (source ~/qnx800/qnxsdp-env.sh) and the target
# binaries in build/x86_64 (make -C qnx). Produces build/image/output/ifs.bin
# and build/image/output/disk-qemu.vmdk. Nothing from the SDP is copied into
# the repository; the image lives under build/, which git ignores.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
: "${QNX_TARGET:?source the QNX SDP 8.0 environment first (qnxsdp-env.sh)}"
BIN="$REPO/build/x86_64"
EXTRA="$REPO/build/mkqnximage-extra"
WORK="$REPO/build/image"

for b in guardian vehicle_sim wheel_sensors brake_ctl steer_ctl cluster telemetry infotainment czctl; do
    [ -x "$BIN/$b" ] || { echo "missing $BIN/$b: run make -C qnx first" >&2; exit 1; }
done

rm -rf "$EXTRA"
mkdir -p "$EXTRA/snippets"
sed "s#@CZ_BIN@#$BIN#g" "$REPO/image/mkqnximage/snippets/ifs_files.crumple.in" > "$EXTRA/snippets/ifs_files.crumple"
cp "$REPO/image/mkqnximage/snippets/post_start.crumple" "$REPO/image/mkqnximage/snippets/profile.crumple" "$EXTRA/snippets/"

# System clock source. lapic: the local APIC timer (startup-x86 default), which
# lets high-resolution timers fire between ticks. hpet: mkqnximage's QEMU default (-zz).
case "${CZ_CLOCK:-lapic}" in
    lapic) STARTUP="startup-x86 -D8250..115200" ;;
    hpet)  STARTUP="startup-x86 -D8250..115200 -zz" ;;
    *) echo "CZ_CLOCK must be lapic or hpet" >&2; exit 1 ;;
esac
sed "s#@CZ_STARTUP@#$STARTUP#" "$REPO/image/mkqnximage/snippets/definitions.zz_crumple.in" > "$EXTRA/snippets/definitions.zz_crumple"

# procnto -h keeps idle CPUs spinning instead of halting (see options.crumple.in).
PROC_OPTS=""
[ "${CZ_NOHALT:-1}" = 1 ] && PROC_OPTS="-h"
sed "s#@CZ_PROC_OPTIONS@#$PROC_OPTS#" "$REPO/image/mkqnximage/snippets/options.crumple.in" > "$EXTRA/snippets/options.crumple"

SSH_KEY="${CZ_SSH_KEY:-$HOME/.ssh/id_ed25519.pub}"
if [ ! -f "$SSH_KEY" ]; then
    mkdir -p "$(dirname "$SSH_KEY")"
    ssh-keygen -q -t ed25519 -N '' -f "${SSH_KEY%.pub}"
fi

mkdir -p "$WORK"
cd "$WORK"
mkqnximage --type=qemu --arch=x86_64 --cpu=4 --ram=2G --hostname=crumple \
    --ssh-ident="$SSH_KEY" --extra-dirs="$EXTRA" --data-size=256 \
    --noprompt --force --build
echo "image: $WORK/output/ifs.bin + $WORK/output/disk-qemu.vmdk"
