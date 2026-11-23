# Build log

Decisions and problems, newest first.

## 2026-11-23: final measurements

- The last smoke test before the final results failed one check in the crash run: the brake had 2963 iterations
  and 32 skipped releases against 3000 expected. The five missing releases were skipped in a gap that straddled
  the edge of the measurement window: the samples on both sides of the gap lay outside the window, so those
  releases were counted neither as run nor as skipped. Skipped releases are now assigned to a window by their own
  release time (`skippedBetween()` in `dashboard/src/shared/runs.ts`, with a unit test), so every window accounts
  for every release exactly; the re-run passed with, for example, 2971 run + 29 skipped = 3000. Miss counts now
  include such edge gaps too.
- The trace table now also lists the longest READY to RUNNING wait per fault, and the text claims only what that
  table shows: in 4 of the 5 traces the longest wait is a stall in which the brake's CPU logged nothing at all.
  `results.ts --render` rewrites `results/results.md` from `results/results.json` without measuring again.

## 2026-11-23: video

- Recorded from the live run with the Chrome DevTools screencast (every painted frame, JPEG) while a scripted
  chaos run plays on the target; rendered to H.264 1920x1080 30 fps with ffmpeg. Narration is written after the
  run from the numbers that run produced, voiced by ElevenLabs Jessica (`cgSgspJ2msm6clMCkdW9`,
  `eleven_multilingual_v2`); every request is counted in `build/tts-ledger.json` and identical lines are cached,
  so a re-record only pays for the sentences whose numbers changed.
- First take: by the sensor-kill scene the page was 5 to 8 s behind the target (uptime advanced 78 s in 83 s of
  video) because the CPU storm left Chromium a single host core and the charts issued about 2,200 `fillRect`
  calls per frame. Fixed by drawing each chart layer as one batched path, slowing DOM refreshes, and anchoring
  every narration clip to the moment the page actually displayed the event.
- The 200 us bound on sensor reads tripped now and then on TCG noise (a correct but spurious SAFE_HOLD for a few
  ms). Raised to 400 us, still well inside the 1 ms period.

## 2026-11-23: evaluation method

- Platform noise on this host is bursty: equal-length baseline windows ranged from 36 to 304 brake misses within
  minutes, so comparing a fault window with the one window before it gave false alarms in both directions. The
  verdict now compares the fault window with the noisiest fault-free window of the same length in the previous
  minute (and a Poisson bound on the immediate baseline). The incident report is told the same.
- `make test` asserts zero deadline misses only on KVM. Under TCG it asserts what is meaningful there: every fault
  injected, restarts within 250 ms, the brake never off its CPU, never lost its sensor link during infotainment
  faults, never paused its output for more than 50 ms, and detected a killed driver in the next period.
- Kernel traces: QNX 8.0 can log a scheduling event on a CPU other than the one the thread runs on (the trace
  showed the brake "running" on CPU 1 while its own `SchedGetCpuNum()` counter never saw it leave CPU 2), so the
  analysis does not attribute per-CPU running time from THRUNNING events. It measures READY to RUNNING latency
  and flags waits in which the brake CPU logged nothing at all (the emulated CPU was frozen).
- Memory-leak stalls of up to 27 ms were host page faults: QEMU backs guest RAM lazily, so the first touch of
  190 MB stalled other vCPUs. `-mem-prealloc` removed them (worst gap 4.7 ms).

## 2026-11-23: recovery time

- Crash detection took 210 ms and a 190 MB leak 12 s because `dumper` wrote a core file before teardown. The
  guardian now sets `RLIMIT_CORE` to 0 on every child with `prlimit()`: detection 5 ms.
- Spawn to ready took about 40 ms under TCG, mostly dynamic linking. Static linking (everything except telemetry,
  which needs the shared-only libsocket) brought restarts to about 8 to 11 ms.
- Controllers reconnect to a restarted driver on `PROCMGR_EVENT_PATHSPACE` instead of polling `open()` every
  millisecond.
- `procnto -h` (no idle halt) cut wake-up latency further but the target hung twice: two CPUs looping in
  `x86_64_as_inv` (TLB invalidation) while the third spun in the idle loop (QEMU monitor `info registers`
  against the procnto symbol file). Removed; 16 back-to-back faults ran clean afterwards.

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
    host thread wake-up before it sees an interrupt. (Later removed: it led to a hang, see "recovery time".)
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
