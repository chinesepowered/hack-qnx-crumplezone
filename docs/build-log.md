# Build log

Decisions and problems, newest first.

## 2026-11-23: timing on QEMU TCG

- The cloud session has no `/dev/kvm`, so QEMU runs in TCG (pure emulation). Measured the platform
  before tuning anything, with `qnx/src/rtbench.c` on the target and a small cyclictest-style probe on
  the host:
  - host itself (a cloud VM, about 1% steal time): `clock_nanosleep` 1 ms loop at SCHED_FIFO,
    pinned: p50 36 us, p99 151 us, p99.9 1.7 ms, max 3.6 ms
  - QNX timer wake-up under TCG: floor about 100 us, p99 several hundred us
  - MsgSend round trip between two threads of one process on one CPU: p50 11.5 us; to another CPU:
    p50 112 us; between two processes (resource manager read): p50 about 110 us. The cross-process
    cost is TCG flushing its software TLB on every address-space switch (TCG has no PCID).
- Changes that came out of the measurements:
  - Periodic interval timers drift under TCG (mean interval 1008 to 1046 us for a 1 ms timer).
    The loops now arm a one-shot high-resolution timer (`TIMER_TOLERANCE`) at an absolute release
    time every period, so releases stay on a fixed grid and lateness is measured against it.
  - Latency accounting fixed: the first version indexed releases by "latest release before the
    wake-up", which hides lateness beyond one period. Each wake-up now serves the release it was
    armed for; skipped releases count as misses.
  - `wheel_sensors` serves the brake devices and the steering device on separate channels, each
    with its own thread on the consumer's CPU, plus `_NTO_CHF_INHERIT_RUNMASK`, so a `read()` is a
    same-CPU handoff instead of a cross-CPU wake-up.
  - The brake loop reads wheel speed every period (1 kHz) and the pedal every fourth (250 Hz): one
    cross-process round trip in most periods instead of two.
  - `procnto -h` (no halt in idle) via an mkqnximage `options` snippet: a halted emulated CPU needs a
    host thread wake-up before it sees an interrupt.
  - Host side (`scripts/host-rt.sh`): QEMU runs with 3 vCPUs, each vCPU thread pinned to its own host
    core at SCHED_FIFO 10, the QEMU main loop at SCHED_FIFO 20; the fourth host core is left for the
    dashboard and recorder. QNX still does all scheduling inside the guest.
  - Result: brake wake-up latency p50 dropped from about 530 us to about 100 us; the tail is set by
    the host (about 1% of 1 ms deadlines are missed at baseline with no fault at all). That noise is
    reported as measured, and the kernel traces separate it from QNX scheduling (see results/).
- QNX 8.0 facts that shaped the design (all checked in the 8.0 docs, see docs/qnx-features.md):
  `ClockPeriod()` can only read the tick in 8.0 (setting it is ENOTSUP), `TIMER_PRECISE` has no
  effect, high-resolution timers come from `TIMER_TOLERANCE` below the tick; runmasks must match a
  cluster (single CPUs always do); HAM is deprecated (QPLMS replaces it) and APS is discontinued.

## 2026-11-23: image and boot

- `mkqnximage --type=qemu` builds the image; its own runner needs a host bridge (root, brctl,
  dnsmasq), so `image/run-qemu.sh` boots the same `ifs.bin` and disk with QEMU user-mode networking
  and port forwards (7300 telemetry, 2222 ssh). Custom content goes through an mkqnximage extra
  directory: `ifs_files` (our binaries in /proc/boot), `post_start` (start the guardian),
  `definitions` (clock source) and `options` (procnto flags).
- `-cpu max` under TCG makes sshd die with SIGSEGV: OpenSSL's AVX2 code paths. Bisected with
  `OPENSSL_ia32cap` (masking only leaf-7 AVX2 fixes it). `-cpu IvyBridge-IBRS` (AVX, no AVX2) boots
  QNX in about 7 s with working sshd. `-cpu Westmere` does not boot QNX 8.0.
- `RESMGR_OCB_T` defaults to `void` unless `<sys/iofunc.h>` is included before `<sys/dispatch.h>`.
- `struct itimerspec` in QNX is `{ it_value, it_interval }`; positional initialisers silently
  swapped them in the first benchmark.

## 2026-11-23: SDP install

- `-installBaseline com.qnx.qnx800` first failed with "run with -mirrorBaseline first", then with a
  missing `com.qnx.qnx800.target`; `-mirror` (all metadata) followed by `-installBaseline` worked.
  The SDP ships the 8.0 documentation offline under `target/qnx/usr/help`, used to verify APIs.
- Baseline is `com.qnx.qnx800` (8.0.0); patch sets up to 8.0.6 exist but were not needed.
