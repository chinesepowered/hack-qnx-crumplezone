# QNX OS 8.0 features used, verified, and where

Every item below was checked against the QNX SDP 8.0 documentation (the online docs at
https://www.qnx.com/developers/docs/8.0/ and the same 8.0 docs shipped offline in the SDP under
`target/qnx/usr/help`) and against the SDP 8.0 headers before we built on it. Page names are relative to
`https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.<guide>/topic/`.

| Feature | 8.0 doc page | Where we use it |
|---|---|---|
| Channels, connections, message passing | `lib_ref` c/channelcreate, c/connectattach, m/msgsend, m/msgreceive, m/msgreply | `qnx/src/guardian.c` (`ChannelCreate`, `MsgReceive`, `MsgReply`, `MsgError`), `qnx/common/cz_child.c` (`ConnectAttach`, `MsgSend` HELLO/READY), `qnx/src/cluster.c`, `qnx/src/infotainment.c` |
| Pulses, timer pulses | `lib_ref` m/msgsendpulse, t/timer_create, s/sigevent | `qnx/common/cz_rt.c` `cz_oneshot_pulse_timer()`; `MsgSendPulse()` doorbells in `qnx/src/brake_ctl.c` |
| High-resolution timers | `prog` timing_tolerance ("High-resolution timers"), `lib_ref` t/timer_settime | `cz_oneshot_pulse_timer()`: `timer_settime(TIMER_TOLERANCE, 1 ns)` on the unarmed timer, then `TIMER_ABSTIME` per period (`cz_timer_arm_abs()`) |
| Clock period | `lib_ref` c/clockperiod, `prog` timing | Read only in 8.0 (setting it is ENOTSUP); `qnx/src/rtbench.c` prints it (1 ms) |
| Kernel timeouts | `lib_ref` t/timertimeout | `bounded_read()` in `qnx/src/brake_ctl.c` and `qnx/src/steer_ctl.c`: `_NTO_TIMEOUT_SEND \| _NTO_TIMEOUT_REPLY`, 400 us |
| Resource manager framework | `resmgr` guide, read_write_Sample_IO_READ; `lib_ref` r/resmgr_attach, d/dispatch_create_channel, i/iofunc_* | `qnx/src/wheel_sensors.c`: `/dev/vehicle/wheel_speed`, `/dev/vehicle/pedal`, `/dev/vehicle/steering` |
| Runmask inheritance on a channel | `lib_ref` c/channelcreate (`_NTO_CHF_INHERIT_RUNMASK`) | `make_dispatch()` in `qnx/src/wheel_sensors.c` |
| Priority inheritance in message passing | `sys_arch` ipc_Priority_inheritance_messages | design of `wheel_sensors` (served at the client's priority) and `cluster` (flood served at infotainment's priority) |
| Disconnect notification | `lib_ref` c/channelcreate (`_NTO_CHF_DISCONNECT`, `_PULSE_CODE_DISCONNECT`) | guardian channel in `qnx/src/guardian.c` |
| Process-death events | `lib_ref` p/procmgr_event_notify, p/procmgr_event_notify_add (`PROCMGR_EVENT_PROCESS_DEATH`, `SIGEV_MAKE_UPDATEABLE`, `MsgRegisterEvent(SYSMGR_COID)`) | `main()` in `qnx/src/guardian.c` |
| Pathname-space events | same pages (`PROCMGR_EVENT_PATHSPACE`) | `link_thread()` in `qnx/src/brake_ctl.c` and `qnx/src/steer_ctl.c` reconnect the moment a restarted driver registers its paths |
| Scheduling | `sys_arch` kernel_Scheduling_priority; `lib_ref` p/pthread_setschedparam | SCHED_FIFO 60 / 55 for the loops; plan in `qnx/common/cz.h` |
| Processor affinity, clusters | `prog` multicore_processor_affinity; `lib_ref` t/threadctl (`_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT`, `RMSK_SET`) | `cz_pin_cpu()` in `qnx/common/cz_rt.c`; storm threads use `_NTO_TCTL_RUNMASK` |
| Spawn attributes | `lib_ref` p/posix_spawn, p/posix_spawnattr_setxflags, p/pthread_spawnattr_setrunmask_np, p/posix_spawnattr_setcred | `spawn_child()` in `qnx/src/guardian.c`: `POSIX_SPAWN_SETSCHEDULER`, `POSIX_SPAWN_EXPLICIT_CPU`, `POSIX_SPAWN_SETCRED` (infotainment runs as uid 1500) |
| Resource limits on another process | `lib_ref` p/prlimit, s/setrlimit | `spawn_child()`: `prlimit(RLIMIT_AS, 192 MB)` on infotainment, `prlimit(RLIMIT_CORE, 0)` on every child |
| Abilities | `lib_ref` p/procmgr_ability; `PROCMGR_AID_HIGH_RESOLUTION_TIMER`, `PROCMGR_AID_PUBLIC_CHANNEL`, `PROCMGR_AID_PATHSPACE` | High-resolution timers and path registration rely on root's abilities; infotainment, as uid 1500, has no write access to the shared bus |
| name_attach / name_open | `lib_ref` n/name_attach, n/name_open | `qnx/src/cluster.c`, `qnx/src/brake_ctl.c`, `qnx/src/infotainment.c` |
| Per-process CPU time, procfs | `lib_ref` c/clock_getcpuclockid, c/clockid; `prog` process_DCMD_PROC_INFO (`/proc/<pid>/ctl`) | `qnx/src/telemetry.c`: CPU % per process, per-CPU load from the idle threads' clocks, private memory per process |
| Kernel event tracing | `utilities` t/tracelogger, t/traceprinter; `lib_ref` t/traceevent (`_NTO_TRACE_INSERTUSRSTREVENT`) | `cz_trace_mark()` in `qnx/common/cz_rt.c`; traces captured by `dashboard/src/tools/results.ts`, analysed by `dashboard/src/tools/trace.ts` |
| Image building | `utilities` m/mkqnximage; `building` guide | `image/build-image.sh`, snippets in `image/mkqnximage/snippets/` (`ifs_files`, `post_start`, `definitions`, `options`) |
| io-sock networking | `io_sock` guide; `utilities` d/devs-vtnet_pci.so | `qnx/src/telemetry.c` TCP server on port 7300 (virtio-net in QEMU) |
| Startup clock source | `utilities` s/startup-x86, s/startup_options (`-z[z]`, `-f`) | `image/mkqnximage/snippets/definitions.zz_crumple.in` (LAPIC timer by default) |

## Considered and not used

- **High Availability Manager (HAM)**: still documented in 8.0 (`com.qnx.doc.ham`, `ham_attach()`), but the
  8.0 discontinuation notice lists HAM as deprecated in favour of the QNX Process Launch and Monitor Service
  (QPLMS, `plms`). The baseline install has no `ham` binary. QPLMS ships in the SDP (`plms`, `plmsctrl`,
  `libplms`), and it is what a production system would use. We kept the guardian on raw kernel primitives so
  every step of a recovery (fault, detection, spawn, ready) is timestamped on one clock and shown on screen.
- **Adaptive partitioning scheduler (APS)**: discontinued in 8.0 (`SchedCtl()` in 8.0 only supports
  processor online/offline/status). Isolation here comes from priorities, CPU runmasks, address spaces,
  credentials and resource limits instead.
- **Interrupt service routines**: removed in 8.0 (`InterruptAttach()` is gone; interrupt service threads via
  `InterruptAttachThread()`/`InterruptAttachEvent()` replace them). This demo has no real hardware
  interrupts; the per-CPU clock interrupt threads (`clock_cpu_N`, priority 254) are visible in the traces.
- **`ClockPeriod()` tuning**: not possible in 8.0 (read only; the tick is set with `procnto -C`). We use
  high-resolution timers instead of shrinking the tick.
- **`TIMER_PRECISE`**: accepted by `timer_settime()` but documented as having no effect in this release.
- **`procnto -h`** (no halt in idle): documented; it trimmed emulator wake-up latency but led to a hang in
  the TLB-invalidation path under QEMU TCG in our runs, so the image does not use it (see build-log.md).

## 8.0 behaviour we hit and documented

- A nonzero runmask must match a cluster (every single CPU is one), else `EINVAL`.
- `posix_spawnattr_setxflags()` stores the POSIX and QNX flags together, so all flags are set in one call.
- `name_attach()` creates a public channel, which needs `PROCMGR_AID_PUBLIC_CHANNEL`.
- In a kernel trace, a scheduling event can be logged by a CPU other than the one the thread runs on; the
  brake loop therefore counts its own off-CPU iterations (`off_cpu`, always 0 in our runs).
- `RESMGR_OCB_T` is `void` unless `<sys/iofunc.h>` is included before `<sys/dispatch.h>`.
- `struct itimerspec` is `{ it_value, it_interval }` in QNX.
