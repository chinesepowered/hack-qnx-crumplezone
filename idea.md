# Crumple Zone: project brief

Build **Crumple Zone**, a software-defined vehicle demo on QNX OS 8.0 that shows a crash in the infotainment system
cannot touch the brakes: the safety loop keeps its timing while the crashed part is restarted in milliseconds. Entry for
the QNX Global Hackathon. Read this whole brief first; it is the only context you get. Do everything in this session:
install the QNX toolchain, build, boot QNX in QEMU, test, record and render the video.

## 1. The hackathon

- QNX Global Hackathon (Devpost `qnx`). Deadline **Tue Dec 15 2026, 11:00 AM PT**. Category: **Automotive &
  Mobility**. Also targets the bonus **Best use of QNX-specific Technologies ($3,500)**. Prizes in the category: $4,000
  best, $2,000 runner-up; grand prize $10,000; 3 honorable mentions at $1,000.
- Judging criteria (equal weight):
  - **Technological Implementation**: how well it is built and how effectively it uses QNX 8.0; real engineering, a
    working non-trivial implementation that genuinely builds for or runs on QNX.
  - **Design**: a complete, coherent product experience, not just a proof of concept.
  - **Potential Impact**: a credible, specific case for a real problem in a market QNX serves, and why a real-time
    foundation like QNX is the right choice.
  - **Innovation & AI Creativity**: a creative, non-obvious use of QNX and genuine understanding of the problem space.
- Submission needs: a public repo with an open-source license (AGPL-3.0) and build and run instructions naming the QNX
  version, the target (QEMU) and the toolchain; a public YouTube video under 3 minutes showing it running on QNX; and
  a written explanation that points reviewers to the exact QNX-specific files, functions and output.
- Serious, industry-credible tone throughout. No mascots, no jokes.

## 2. The product

**The problem.** Carmakers are consolidating infotainment, instrument cluster and vehicle control onto one computer.
When a media app leaks memory or a third-party plugin crashes, it must never delay a brake command. Proving that is
what mixed-criticality systems are about, and it is QNX's home market.

**The demo.** One QEMU x86_64 machine running QNX 8.0 with these processes:

| Process | Role | Priority |
|---|---|---|
| `brake_ctl` | 1 kHz brake-by-wire control loop: reads wheel speed and pedal, computes and publishes brake pressure | highest (FIFO) |
| `steer_ctl` | 500 Hz steering assist loop | high (FIFO) |
| `wheel_sensors` | simulated wheel-speed and pedal sensors exposed as a resource manager (`/dev/vehicle/...`) | high |
| `cluster` | instrument cluster feed: speed, warnings | medium |
| `infotainment` | media and navigation; has a chaos mode | low |
| `guardian` | supervisor: detects process death and restarts it, records recovery time | high |
| `telemetry` | streams metrics and events over TCP (io-sock) to the dashboard | medium |

**The chaos panel.** A web dashboard (runs on the host side of the QEMU network, served from the cloud session for the
recording) shows live: each control loop's period, jitter and worst-case latency in microseconds, deadline misses, CPU
per process, and an event timeline. Buttons inject faults into the non-critical side:

1. Crash infotainment (segfault): `guardian` restarts it; show the recovery time.
2. CPU storm: infotainment spins every core at its own priority; the brake loop's jitter stays flat.
3. Memory leak: infotainment allocates until its limit; it dies alone and comes back.
4. Message flood: infotainment spams the cluster; QNX message passing back-pressure keeps the control loops unaffected.
5. Kill a sensor driver: brake control detects stale data within one period and enters a safe state; `guardian`
   restarts the driver; normal control resumes.

Every result on screen is measured on QNX, never simulated in the browser. The video's headline numbers (worst-case
brake loop latency under each fault, recovery times) come from the real run and are written to `results/`.

**AI incident report (Innovation & AI Creativity).** After each chaos run, an "Incident report" button sends the
structured event log (not raw text) to an LLM (W&B Inference, OpenAI-compatible, model `Qwen/Qwen3.8-27B`) that writes a
short report an automotive safety engineer would recognise: what failed, what the guardian did, timing impact on each
safety loop with the measured numbers, and whether any safety requirement was violated. The model only summarises;
every number in the report is checked against the log by code before display, and a report with an unverified number
is rejected and regenerated once, then shown with the mismatch flagged.

## 3. QNX technology to use (and show)

Use what QNX 8.0 actually provides. Verify each item in the QNX 8.0 docs (https://www.qnx.com/developers/docs/8.0/)
before building on it, and record what you used and where in `docs/qnx-features.md`.

- **Neutrino message passing**: `ChannelCreate`, `ConnectAttach`, `MsgSend`/`MsgReceive`/`MsgReply` between sensors,
  controllers and cluster; pulses for timer ticks (`timer_create` with a pulse event).
- **Resource manager** for the sensors (`/dev/vehicle/wheel_speed`, `/dev/vehicle/pedal`) using the resource manager
  framework (`iofunc_*`, `dispatch_*`).
- **Real-time scheduling**: SCHED_FIFO priorities; CPU affinity via `ThreadCtl(_NTO_TCTL_RUNMASK...)` so the brake
  loop owns a core; clock period tuned with `ClockPeriod()` if needed.
- **Process isolation and restart**: separate processes, death notification to `guardian` (for example a channel with
  `_NTO_CHF_COID_DISCONNECT`/`_NTO_CHF_DISCONNECT`, or `procmgr_event_notify()`), restart measured end to end. If the
  High Availability Manager or adaptive partitioning exist in SDP 8.0, consider them, but only if the docs confirm
  they are supported in 8.0; ISRs are gone in 8.0 (interrupt service threads replace them), so do not rely on old 7.x
  examples.
- **Resource limits** on infotainment (`setrlimit`) so its memory leak kills only itself.
- **Kernel event tracing** (`tracelogger`, `traceprinter`) to prove the timing claims; ship one trace per fault in
  `results/`.
- **Image building**: `mkqnximage` for the QEMU x86_64 image, plus a buildfile so the image boots straight into the
  demo.
- **Networking**: io-sock TCP for telemetry.

## 4. Setting up QNX in this session

Credentials for the free non-commercial license are in your environment as `MYQNX_USER` and `MYQNX_PASSWORD` (see the
paste message). The license is already deployed to that account. Verified install steps on Linux:

1. `curl --cookie-jar ~/.qnx/myqnxcookies.auth --form "userlogin=$MYQNX_USER" --form "password=$MYQNX_PASSWORD" -X POST https://www.qnx.com/account/login.html -o /dev/null`
2. `curl -L --cookie ~/.qnx/myqnxcookies.auth https://www.qnx.com/download/download/79441/qnx-setup-2.0.4-202501021438-linux.run -o qnx-setup-lin.run`
   (MD5 must be `bca161a97ab3e14189cf8e7c8227fb7d`)
3. `chmod a+x qnx-setup-lin.run && ./qnx-setup-lin.run force-override disable-auto-start agree-to-license-terms ~/qnxinstall`
4. `~/qnxinstall/qnxsoftwarecenter/qnxsoftwarecenter_clt -syncLicenseKeys -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" -activateAll -listLicenseKeys`
5. Install the SDP 8.0 baseline with `qnxsoftwarecenter_clt -installBaseline <id> -destination ~/qnx800` (the baseline
   id is probably `com.qnx.qnx800`; confirm it with the tool) plus the packages `mkqnximage` and QEMU support need.
   Then `source ~/qnx800/qnxsdp-env.sh`.

**License rules you must follow:**
- The license allows **at most two activated machines at a time**, and another session may be using one. **Before
  you end this session, run `qnxsoftwarecenter_clt -myqnx.user=... -myqnx.password=... -deactivateAll`**, and
  reactivate next time. If activation fails because the limit is reached, say so in your reply and stop.
- Never commit anything from the SDP (headers, libraries, binaries, images) or the license key; the repo holds only
  your source, buildfiles and scripts. `qnx800/`, `~/.qnx`, images and the installer stay out of git.
- Use the software only for this non-commercial hackathon project.

Install QEMU in the session (`apt-get install qemu-system-x86`); use KVM if `/dev/kvm` exists, otherwise TCG. The
timing numbers must be measured inside QNX either way; state in the README which one the recorded numbers came from.

## 5. Engineering rules

- C (or C++) with `qcc`; host-side dashboard in TypeScript (pnpm). Scripts: `make image`, `make run` (boots QEMU and
  the demo), `make test` (host unit tests plus an on-target smoke test in QEMU), `make record`.
- Tests: unit tests for the control math and the statistics; an automated QEMU run that injects every fault and asserts
  the brake loop's deadline misses stay at zero and the restart happens.
- Git identity `chinesepowered <22500229+chinesepowered@users.noreply.github.com>`, no Co-Authored-By lines; commit
  and push to `main` after every working milestone. Add `LICENSE` (AGPL-3.0).
- No em dashes anywhere. Keys only in `.env` (git-ignored); check the full history for keys before the final push.
- Keep `docs/build-log.md` (decisions, problems, newest first) and `docs/needs-user.md` (only what truly needs the
  owner).

## 6. Deliverables

1. Source, buildfile and scripts that rebuild the image and run the demo from a clean checkout plus the SDP.
2. `README.md` written for judges in this order: a 3-4 sentence summary with the video link on its own line; the
   problem and our solution; a table of the QNX technologies used (technology, what it does here, file); a short
   section per technology pointing to files and functions; an architecture diagram (Mermaid in the README); build and
   run instructions (QNX SDP 8.0, QEMU x86_64, `qcc` toolchain); measured results. Never open with "Inspiration".
3. `docs/devpost.md` in the same order (text diagram instead of Mermaid), plus answers for the form: build and run
   instructions; "how the project meets the main technology requirement" naming each QNX product and the exact files,
   functions and demo moments; AI tools used (Claude Code, W&B Inference with Qwen, ElevenLabs).
4. `results/`: the numbers table and one kernel trace per fault.
5. `demo/video.mp4`: under 2:45, 1920x1080, H.264, recorded in this session from the real QEMU run (the dashboard plus
   the QNX console). Narration by ElevenLabs voice **Jessica** (`cgSgspJ2msm6clMCkdW9`, model
   `eleven_multilingual_v2`, a young female voice); keep ElevenLabs use under 6,000 characters. No copyrighted music.
   Open with the problem and the moment the infotainment crashes while the brake trace stays flat.
6. `demo/thumb.png` and 5 or 6 stills in `docs/screenshots/`.

## 7. Timeline

Working end to end in QEMU by **Mon Dec 7**; faults, AI report and polish by **Thu Dec 10**; video and docs by
**Fri Dec 11**. The owner submits by Dec 14.
