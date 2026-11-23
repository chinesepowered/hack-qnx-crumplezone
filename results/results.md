# Measured results

Recorded 2026-11-23 21:00 UTC on QNX OS 8.0.0 (x86_64) in QEMU TCG with 3 vCPUs, by `make results`.
Every number here was measured inside QNX by the processes themselves and streamed over the
telemetry socket; nothing is simulated on the host. Raw data: `results.json`. Kernel traces: `traces/`.

Each fault window is compared with an equal-length baseline window taken immediately before the
fault. Under TCG the emulator and the shared cloud host already cause deadline misses with no fault
at all (see "Platform noise" below), so the baseline column matters as much as the fault column.

## Recovery

| Fault | Process | Cause | Detected via | Fault to detection (ms) | Restart, detection to ready (ms) | Fault to ready (ms) | Within 50 ms budget |
|---|---|---|---|---|---|---|---|
| Crash infotainment | infotainment | SIGSEGV | channel disconnect | 5.04 | 7.09 | 12.13 | yes |
| CPU storm | - | no restart needed | - | - | - | - | - |
| Memory leak | infotainment | SIGABRT | channel disconnect | - | 6.96 | - | yes |
| Message flood | - | no restart needed | - | - | - | - | - |
| Kill sensor driver | wheel_sensors | SIGKILL | channel disconnect | 2.84 | 8.56 | 11.40 | yes |

For the memory leak, "fault to ready" is not meaningful (the leak runs until the limit is hit); the restart time is.

## Brake loop (1 kHz, SCHED_FIFO 60, CPU 2)

| Fault | Window (ms) | Misses, fault | Misses, baseline | Latency p50 / p99 / max, fault (us) | Same, baseline (us) | Exec p99, fault / baseline (us) | Off-CPU iterations |
|---|---|---|---|---|---|---|---|
| Crash infotainment | 3000 | 45 / 2988 | 38 / 2986 | 144.1 / 649.8 / 4789.5 | 137.7 / 719.8 / 3045.7 | 424.9 / 346 | 0 |
| CPU storm | 7000 | 119 / 6944 | 129 / 6959 | 116.2 / 602.7 / 12803.4 | 142 / 838.9 / 3461.1 | 367.8 / 378 | 0 |
| Memory leak | 8500 | 157 / 8451 | 164 / 8434 | 144.4 / 808.8 / 5996.1 | 146.9 / 792.1 / 8035.1 | 432.6 / 418 | 0 |
| Message flood | 7000 | 110 / 6952 | 160 / 6930 | 132.9 / 602.1 / 5955 | 142.8 / 876.8 / 13752.7 | 365.5 / 376.5 | 0 |
| Kill sensor driver | 3000 | 42 / 2982 | 64 / 2983 | 137.9 / 556.4 / 3056.1 | 143.5 / 846.7 / 4063.3 | 380 / 396 | 0 |

## Steering loop (500 Hz, SCHED_FIFO 55, CPU 1)

| Fault | Misses, fault | Misses, baseline | Latency p99 / max, fault (us) | Same, baseline (us) |
|---|---|---|---|---|
| Crash infotainment | 9 / 1498 | 8 / 1499 | 915.8 / 6544.3 | 1172.7 / 4092.1 |
| CPU storm | 17 / 3492 | 27 / 3495 | 706.6 / 15515.8 | 1200.4 / 6544.3 |
| Memory leak | 14 / 4248 | 9 / 4249 | 898.4 / 5310.5 | 736.9 / 3436 |
| Message flood | 19 / 3496 | 37 / 3491 | 962.7 / 4401.1 | 1457.5 / 13003.8 |
| Kill sensor driver | 6 / 1500 | 3 / 1500 | 1108.7 / 2659.9 | 931.4 / 2741.2 |

## Fault-specific measurements

- **Crash infotainment**: brake sensor-link losses: 0 (stale-sample entries caused by emulator pauses of the simulated plant: 0).
- **CPU storm**: CPU load peaked at 100 / 100 / 100 % (CPU 0 / 1 / 2); brake sensor-link losses: 0 (stale-sample entries caused by emulator pauses of the simulated plant: 3).
- **Memory leak**: infotainment peaked at 188 MB of its 192 MB address-space limit, then died alone; brake sensor-link losses: 0 (stale-sample entries caused by emulator pauses of the simulated plant: 0).
- **Message flood**: 99746 flood messages were delivered to the cluster; its display latency peaked at 486 us (baseline 480 us); brake sensor-link losses: 0 (stale-sample entries caused by emulator pauses of the simulated plant: 0).
- **Kill sensor driver**: brake entered a safe state 1.25 ms after the kill (reason: read error (driver gone)), held it for 21.0 ms, then resumed normal control.

## Kernel traces (second pass, tracelogger running)

`brake ready to running` is the time between the kernel making the brake thread READY and running it: pure
scheduler latency, measured from THREADY/THRUNNING events. It excludes timer delivery, which is where the
emulator adds its delay. Infotainment time on CPU 2 shows how long fault threads ran on the brake core; it is
approximate, because QNX 8.0 can log a scheduling event on a CPU other than the one the thread runs on. The
brake's own check of its CPU (off-CPU iterations, above) is exact.

| Fault | Trace | Brake wake-ups | Ready to running p50 / p99, before fault (us) | After fault (us) | Waits over 100 us: CPU 2 silent (emulator) / CPU 2 busy | Longest wait (us) | Longest emulator stall (us) | Infotainment on CPU 2 (ms) |
|---|---|---|---|---|---|---|---|---|
| Crash infotainment | [crash.kev.gz](traces/crash.kev.gz), [excerpt](traces/crash.excerpt.txt) | 6675 | 16.2 / 79.9 | 16.83 / 73.21 | 16 / 22 | 497.54 | 340.4 | 4 |
| CPU storm | [storm.kev.gz](traces/storm.kev.gz), [excerpt](traces/storm.excerpt.txt) | 11143 | 17.14 / 82.26 | 11.69 / 63.02 | 24 / 27 | 4882.42 | 4882.4 | 1949.15 |
| Memory leak | [leak.kev.gz](traces/leak.kev.gz), [excerpt](traces/leak.excerpt.txt) | 13435 | 17.22 / 129.64 | 16.93 / 85.28 | 44 / 66 | 2787.95 | 2788 | 17.45 |
| Message flood | [flood.kev.gz](traces/flood.kev.gz), [excerpt](traces/flood.excerpt.txt) | 11426 | 21.8 / 86.97 | 14.92 / 88.7 | 20 / 70 | 1966.2 | 1966.2 | 507.59 |
| Kill sensor driver | [kill_sensor.kev.gz](traces/kill_sensor.kev.gz), [excerpt](traces/kill_sensor.excerpt.txt) | 6760 | 16.75 / 65.65 | 17.86 / 76.34 | 9 / 11 | 1160.43 | 1160.4 | 4.29 |

A wait counts as an emulator stall when the brake CPU logged no trace event at all for most of it while the
other CPUs kept logging: the emulated CPU was not executing, so no software on it could have run.

Read a trace with the SDP host tool: `gunzip -k results/traces/crash.kev.gz && traceprinter -f results/traces/crash.kev`.

## Platform noise

With no fault at all, the brake loop missed 555 of 28292 deadlines across the baseline windows above
(19.6 per 1000 periods). That is the emulator and the host, not QNX: QEMU here is TCG
(pure emulation, no KVM in the cloud session), and the host is itself a shared cloud VM.
A 1 ms SCHED_FIFO timer loop on the bare host (`scripts/hostlat.c`, 5000 wake-ups, host CPU 3) measured
p50 37.2 us, p99 502.5 us, p99.9 7067.4 us, max 9981 us of lateness before QEMU or QNX are involved.
The kernel traces above show where the time goes. Once the kernel makes the brake thread ready, it runs within
tens of microseconds (p99 67.55 to 91.26 us across the traces) before and during every fault. Waits over
100 us are at most 0.8% of wake-ups, and in 4 of the 5 traces the longest wait is a stall in which the
brake's emulated CPU logged nothing at all. The rest of the lateness comes before the thread is made ready, in
timer expiry and interrupt delivery, which the emulator slows down (about 100 us even on an idle target, see
`docs/build-log.md`).
On KVM the same image runs the same code, and `make test` additionally asserts zero deadline misses; that could
not be exercised in this session (no KVM).

## The run shown in the video

`demo/video.mp4` is a screen recording of a separate live run (2026-11-23 20:40 UTC, same image, same
platform). Its numbers, as shown on screen and spoken in the narration (raw data: `demo/video-run.json`):

| Fault | Recovery | Brake periods run | Brake misses, fault / just before | Notes |
|---|---|---|---|---|
| Crash infotainment | SIGSEGV, 17.1 ms fault to ready | 2993 | 28 / 59 |  |
| CPU storm | no restart needed | 6813 | 307 / 167 | CPU peak 100 / 100 / 100 % |
| Memory leak | SIGABRT, 8.7 ms restart | 8472 | 134 / 164 | peak 188 MB |
| Message flood | no restart needed | 6913 | 168 / 388 | 114699 messages, display latency max 493 us |
| Kill sensor driver | SIGKILL, 37.7 ms fault to ready | 2995 | 46 / 60 | brake detected the loss 3.03 ms after the kill, safe state 46.0 ms |

The incident report generated in the video had all 20 of its numbers verified against the event log.
