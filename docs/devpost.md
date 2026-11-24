# Crumple Zone: Devpost submission text

Category: **Automotive & Mobility**. Also entered for **Best use of QNX-specific Technologies**.

## Summary

Crumple Zone is a software-defined vehicle computer on QNX OS 8.0 that shows a crash in the infotainment system
cannot touch the brakes. A 1 kHz brake-by-wire loop, a 500 Hz steering loop, a sensor driver, an instrument
cluster and an infotainment stack run as isolated processes on one QEMU x86_64 machine, supervised by a guardian
that restarts a failed process in milliseconds. A chaos panel injects five faults into the non-critical side and
shows, from numbers measured inside QNX, that the safety loops keep running while the guardian recovers; an AI
incident report summarises each run, and code checks every number in it against the event log.

Video: https://youtu.be/2PXPmrBxjRM

Repository: https://github.com/chinesepowered/hack-qnx-crumplezone

## The problem and our solution

Carmakers are consolidating infotainment, the instrument cluster and vehicle control onto one computer. A media
app that leaks memory, a plugin that crashes, or a runaway thread must never delay a brake command. Proving that
is what mixed-criticality engineering is about, and it is QNX's home market.

Crumple Zone gives the vehicle a crumple zone in software: the non-critical side may fail, and the failure is
absorbed there.

- Spatial isolation: every function is its own QNX process. Infotainment runs as uid 1500 with a 192 MB
  address-space limit imposed from outside by the guardian (`prlimit`), and cannot write the shared state.
- Temporal isolation: the brake loop is the highest-priority SCHED_FIFO thread pinned to its own CPU, released by
  a high-resolution timer on an absolute grid; every call it makes into another process is bounded by
  `TimerTimeout()`; it never blocks on anything less critical than itself.
- Fast, measured recovery: the guardian detects death three ways (process-death event, channel disconnect,
  `waitid`), restarts with the original priority, CPU, uid and limits, and timestamps fault, detection, spawn and
  ready on one clock.
- Evidence: each loop's every iteration is plotted live; each fault is compared with an equal-length window just
  before it; one kernel trace per fault separates QNX scheduling from emulator delay.

## QNX technologies used (technology, what it does, file)

- Neutrino message passing (`ChannelCreate`, `ConnectAttach`, `MsgSend`, `MsgReceive`, `MsgReply`): guardian
  protocol, long-poll fault commands, cluster media messages. `qnx/src/guardian.c`, `qnx/common/cz_child.c`,
  `qnx/src/cluster.c`
- Pulses and high-resolution timers (`timer_create` with a pulse, `TIMER_TOLERANCE`, `TIMER_ABSTIME`,
  `MsgSendPulse`): loop releases off the tick on an absolute grid; non-blocking doorbells.
  `qnx/common/cz_rt.c`, `qnx/src/brake_ctl.c`
- Kernel timeouts (`TimerTimeout` with `_NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY`): every sensor read bounded to
  400 us. `bounded_read()` in `qnx/src/brake_ctl.c`
- Resource manager framework (`resmgr_attach`, `dispatch_*`, `iofunc_*`) and `_NTO_CHF_INHERIT_RUNMASK`: the
  sensor driver `/dev/vehicle/wheel_speed`, `/pedal`, `/steering`, served on the client's CPU.
  `qnx/src/wheel_sensors.c`
- SCHED_FIFO priorities, CPU runmasks (`ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT)`), priority inheritance
  and priority-ordered channels. `cz_pin_cpu()` in `qnx/common/cz_rt.c`, `qnx/src/cluster.c`
- Process death notification (`procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH)`, `_NTO_CHF_DISCONNECT`) and
  pathname-space events (`PROCMGR_EVENT_PATHSPACE`). `qnx/src/guardian.c`, `link_thread()` in `qnx/src/brake_ctl.c`
- `posix_spawn` QNX extensions (`POSIX_SPAWN_SETCRED`, `POSIX_SPAWN_EXPLICIT_CPU`) and `prlimit`
  (`RLIMIT_AS`, `RLIMIT_CORE`). `spawn_child()` in `qnx/src/guardian.c`
- Kernel event tracing (`tracelogger`, `traceprinter`, `TraceEvent` user events). `cz_trace_mark()` in
  `qnx/common/cz_rt.c`, `dashboard/src/tools/trace.ts`, `results/traces/`
- procfs and CPU-time clocks (`DCMD_PROC_INFO`, `clock_getcpuclockid`, `ClockId`), io-sock TCP.
  `qnx/src/telemetry.c`
- Image building: `mkqnximage` with snippets, `startup-x86` clock source, procnto options. `image/`

## Architecture

```
 Linux host                                   QEMU x86_64, 3 vCPU: QNX OS 8.0
 +-------------------------+                 +-----------------------------------------------------------+
 | Dashboard (TypeScript)  |  JSON, TCP 7300 | telemetry (io-sock, RR 20) <---- crumple_bus (shared mem) |
 | chaos panel, charts,    |<----------------|      |  MsgSend FAULT                ^    ^    ^    ^       |
 | QNX console, AI report  |---------------->|      v                               |    |    |    |       |
 +-----------+-------------+  fault commands | guardian (FIFO 50): posix_spawn, prlimit, death events,   |
             |                               |   restart; SIGKILL fault ---> wheel_sensors (resmgr)       |
             | structured event log          |                                  /dev/vehicle/*  ^   ^     |
             v                               |  brake_ctl (1 kHz, FIFO 60, CPU 2) -- read() ----+   |     |
 +-------------------------+                 |  steer_ctl (500 Hz, FIFO 55, CPU 1) -- read() -------+     |
 | W&B Inference, Qwen3.8  |                 |  brake_ctl -- MsgSendPulse --> cluster (RR 30)              |
 | incident report, every  |                 |  infotainment (RR 8, uid 1500, 192 MB) -- MsgSend --> cluster|
 | number checked by code  |                 |  vehicle_sim (stands in for the car, CPU 0): registers     |
 +-------------------------+                 +-----------------------------------------------------------+
```

## Measured results

Recorded with `make results` on QNX OS 8.0.0 (x86_64, 3 CPUs) (full tables, raw data and kernel traces
in `results/`):

- Crash infotainment (SIGSEGV): detected 5.0 ms after the fault, ready again 12.1 ms after it.
- CPU storm (every core at 100%, the brake's included): no restart needed; the brake's scheduler latency (kernel
  trace, p50 / p99) was 11.69 / 63.02 us during the storm and 17.14 / 82.26 us before it.
- Memory leak: peaked at 188 MB of the 192 MB limit, died alone (SIGABRT), restarted in 7.0 ms.
- Message flood: 99,746 messages; instrument cluster display latency peak 486 us (480 us just before).
- Kill sensor driver (SIGKILL): brake detected the loss 1.25 ms after the kill, held a safe command
  for 21.0 ms; driver ready again 11.4 ms after the kill.
- Brake scheduler latency (READY to RUNNING, kernel traces): p50 13 to 17 us, p99 68 to 91 us before and
  during the faults; zero iterations off its pinned CPU.


## Form answers

### Build and run instructions

Requires QNX SDP 8.0 (baseline `com.qnx.qnx800`, `qcc` gcc 12.2 for x86_64, `mkqnximage`), QEMU x86_64
(`qemu-system-x86`, 8.2 used), Node.js 20+ with pnpm, gcc and make on Linux.

1. `source ~/qnx800/qnxsdp-env.sh`
2. `make image`: builds the ten QNX binaries with `qcc` and the QEMU x86_64 image with `mkqnximage`
3. `make run`: boots QEMU (KVM if available, otherwise TCG) and starts the dashboard at http://127.0.0.1:8080
4. `make test`: host unit tests, then an on-target smoke test that injects every fault
5. `make results`: measured run plus one kernel trace per fault into `results/`
6. `make record`: records and renders the demo video (needs an ElevenLabs key in `.env`)

The image boots straight into the demo. QNX console: TCP 127.0.0.1:4555 (also in the dashboard); ssh:
127.0.0.1:2222.

### How the project meets the main technology requirement

Crumple Zone builds for and runs on **QNX OS 8.0** (QNX SDP 8.0, `qcc`, `mkqnximage`, QEMU x86_64 target). Every
measurement on screen is taken by QNX processes and streamed over **io-sock**. QNX products and features, with the
exact files, functions and demo moments:

- **QNX Neutrino microkernel message passing**: `MsgSend`/`MsgReceive`/`MsgReply` in `qnx/src/guardian.c`
  (`main()` loop, `inject()`), `qnx/common/cz_child.c` (`cz_guardian_connect()`), `qnx/src/cluster.c`. Demo moment:
  message flood, where four infotainment threads flood the cluster and are served at their own priority.
- **Resource manager framework**: `qnx/src/wheel_sensors.c` (`io_read()`, `make_dispatch()`, `main()`). Demo
  moment: `cat /dev/vehicle/wheel_speed` on the QNX console, then the driver is killed and the brake detects it in
  the next period.
- **Real-time scheduling**: SCHED_FIFO, runmasks with `ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT)`
  (`cz_pin_cpu()` in `qnx/common/cz_rt.c`), high-resolution timers with `TIMER_TOLERANCE`
  (`cz_oneshot_pulse_timer()`), `TimerTimeout()` (`bounded_read()` in `qnx/src/brake_ctl.c`). Demo moment: CPU storm,
  where all three CPUs are at 100% and the brake trace does not change.
- **Process model and supervision**: `posix_spawn` with `POSIX_SPAWN_SETCRED` and `POSIX_SPAWN_EXPLICIT_CPU`,
  `prlimit`, `procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH)`, `_NTO_CHF_DISCONNECT`
  (`spawn_child()`, `on_death()`, `on_ready()` in `qnx/src/guardian.c`). Demo moments: infotainment crash and
  memory leak, each restarted in milliseconds with the time shown.
- **Kernel event tracing**: `tracelogger`, `traceprinter`, `TraceEvent` user events (`cz_trace_mark()`); one trace
  per fault in `results/traces/` analysed by `dashboard/src/tools/trace.ts`.
- **mkqnximage and the IFS**: `image/build-image.sh`, `image/mkqnximage/snippets/` (the guardian starts from
  `post_startup.sh`, so the image boots straight into the demo).

### AI tools used

- **Claude Code** (Anthropic): wrote the code, scripts and documentation in this repository with the author,
  ran the builds, tests and measurements, and recorded the video in a cloud session.
- **W&B Inference with Qwen3.8-27B** (`Qwen/Qwen3.8-27B`, OpenAI-compatible API): writes the incident report from
  the structured event log of each chaos run (`dashboard/src/server/report.ts`). Every number it writes is checked
  against the log by `dashboard/src/shared/verify.ts`; a report with an unverified number is regenerated once and
  otherwise shown with the mismatch flagged. Fallback model: `zai-org/GLM-5.3-Flash`.
- **ElevenLabs** (voice Jessica, model `eleven_multilingual_v2`): the video narration.
