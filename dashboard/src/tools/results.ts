// Crumple Zone: measured run of every fault, written to results/.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// Pass 1 (untraced) produces the headline numbers. Pass 2 repeats every fault
// with tracelogger running on the target and keeps one kernel trace per fault
// (results/traces/<fault>.kev.gz, readable with traceprinter), an excerpt
// around the fault marker, and an analysis that separates scheduler latency
// from emulator timer delivery. Tracing adds overhead under TCG, which is why
// the headline numbers come from the untraced pass.

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { FAULTS, FAULT_LABELS, type FaultName } from '../shared/types.js';
import { runFault, sleep, type Recorder, type RunResult } from '../shared/runs.js';
import { accelerator, log, repo, scpFrom, session, ssh, sshBackground, traceprinter } from './qnx.js';
import { analyseTrace, excerpt, type TraceAnalysis } from './trace.js';

const pexec = promisify(execFile);
const outDir = path.join(repo, 'results');
const traceDir = path.join(outDir, 'traces');
const workDir = path.join(repo, 'build', 'traces');

const TRACE_SECONDS: Record<FaultName, number> = { crash: 3, storm: 5, leak: 6, flood: 5, kill_sensor: 3 };

function pidOf(rec: Recorder, name: string): number {
  return rec.procs.at(-1)?.msg.p.find((p) => p.name === name)?.pid ?? 0;
}

async function tracedRun(tele: Parameters<typeof runFault>[0], rec: Recorder, f: FaultName): Promise<{ run: RunResult; trace: TraceAnalysis | null }> {
  const secs = TRACE_SECONDS[f];
  const remote = `/tmp/cz_${f}.kev`;
  const brakePid = pidOf(rec, 'brake_ctl');
  const infotBefore = pidOf(rec, 'infotainment');
  const traceDone = sshBackground(`rm -f ${remote}; tracelogger -s ${secs} -w -F 1 -f ${remote} >/dev/null 2>&1`);
  // tracelogger creates its file when logging starts; under emulation ssh and
  // tracelogger take a second or more to get going
  for (let i = 0; i < 20; i++) {
    await sleep(500);
    if ((await ssh(`ls ${remote} 2>/dev/null || true`)).includes(remote)) break;
  }
  await sleep(700);
  const run = await runFault(tele, rec, f, { durationMs: 3000, windowMs: Math.min(3000, (secs - 1.5) * 1000) });
  await traceDone;
  await sleep(500);
  fs.mkdirSync(workDir, { recursive: true });
  fs.mkdirSync(traceDir, { recursive: true });
  const kev = path.join(workDir, `${f}.kev`);
  const txt = path.join(workDir, `${f}.txt`);
  try {
    await scpFrom(remote, kev);
    fs.writeFileSync(path.join(traceDir, `${f}.kev.gz`), zlib.gzipSync(fs.readFileSync(kev), { level: 9 }));
    await traceprinter(kev, txt);
    const faultPids = [infotBefore, pidOf(rec, 'infotainment')].filter((p) => p > 0);
    const trace = await analyseTrace(txt, { brakePid, brakeTid: 1, brakePrio: 60, brakeCpu: 2, faultPids });
    fs.writeFileSync(path.join(traceDir, `${f}.excerpt.txt`), await excerpt(txt, 'CZ fault inject'));
    return { run, trace };
  } catch (e) {
    log(`  trace for ${f} failed: ${e instanceof Error ? e.message : e}`);
    return { run, trace: null };
  }
}

function fmt(v: number | null | undefined, d = 1): string {
  return v === null || v === undefined || !Number.isFinite(v) ? '-' : v.toFixed(d);
}

interface HostNoise { cpu: number; n: number; p50_us: number; p99_us: number; p999_us: number; max_us: number }

async function hostNoise(): Promise<HostNoise | null> {
  try {
    const bin = path.join(repo, 'build', 'host', 'hostlat');
    fs.mkdirSync(path.dirname(bin), { recursive: true });
    await pexec('gcc', ['-O2', '-o', bin, path.join(repo, 'scripts', 'hostlat.c')]);
    const { stdout } = await pexec(bin, ['3', '5000'], { timeout: 30000 });
    return JSON.parse(stdout.trim()) as HostNoise;
  } catch {
    return null;
  }
}

function markdown(accel: string, runs: RunResult[], traced: { run: RunResult; trace: TraceAnalysis | null }[], when: string, host: HostNoise | null): string {
  const L: string[] = [];
  L.push('# Measured results');
  L.push('');
  L.push(`Recorded ${when} on QNX OS 8.0.0 (x86_64) in QEMU ${accel} with 3 vCPUs, by \`make results\`.`);
  L.push('Every number here was measured inside QNX by the processes themselves and streamed over the');
  L.push('telemetry socket; nothing is simulated on the host. Raw data: `results.json`. Kernel traces: `traces/`.');
  L.push('');
  L.push('Each fault window is compared with an equal-length baseline window taken immediately before the');
  L.push('fault. Under TCG the emulator and the shared cloud host already cause deadline misses with no fault');
  L.push('at all (see "Platform noise" below), so the baseline column matters as much as the fault column.');
  L.push('');
  L.push('## Recovery');
  L.push('');
  L.push('| Fault | Process | Cause | Detected via | Fault to detection (ms) | Restart, detection to ready (ms) | Fault to ready (ms) | Within 50 ms budget |');
  L.push('|---|---|---|---|---|---|---|---|');
  for (const r of runs) {
    const rc = r.recovery;
    L.push(`| ${FAULT_LABELS[r.fault]} | ${rc?.process ?? '-'} | ${rc?.cause ?? 'no restart needed'} | ${rc?.detection_path ?? '-'} | ${fmt(rc?.fault_to_detect_ms, 2)} | ${fmt(rc?.detect_to_ready_ms, 2)} | ${fmt(rc?.fault_to_ready_ms, 2)} | ${rc ? (rc.within_budget ? 'yes' : 'no') : '-'} |`);
  }
  L.push('');
  L.push('For the memory leak, "fault to ready" is not meaningful (the leak runs until the limit is hit); the restart time is.');
  L.push('');
  L.push('## Brake loop (1 kHz, SCHED_FIFO 60, CPU 2)');
  L.push('');
  L.push('| Fault | Window (ms) | Misses, fault | Misses, baseline | Latency p50 / p99 / max, fault (us) | Same, baseline (us) | Exec p99, fault / baseline (us) | Off-CPU iterations |');
  L.push('|---|---|---|---|---|---|---|---|');
  for (const r of runs) {
    const f = r.brake.fault, b = r.brake.baseline;
    L.push(`| ${FAULT_LABELS[r.fault]} | ${r.window_ms} | ${f.deadline_misses} / ${f.iterations} | ${b.deadline_misses} / ${b.iterations} | ${f.latency_us.p50} / ${f.latency_us.p99} / ${f.latency_us.max} | ${b.latency_us.p50} / ${b.latency_us.p99} / ${b.latency_us.max} | ${f.exec_us.p99} / ${b.exec_us.p99} | ${r.brake_off_cpu_iterations} |`);
  }
  L.push('');
  L.push('## Steering loop (500 Hz, SCHED_FIFO 55, CPU 1)');
  L.push('');
  L.push('| Fault | Misses, fault | Misses, baseline | Latency p99 / max, fault (us) | Same, baseline (us) |');
  L.push('|---|---|---|---|---|');
  for (const r of runs) {
    const f = r.steer.fault, b = r.steer.baseline;
    L.push(`| ${FAULT_LABELS[r.fault]} | ${f.deadline_misses} / ${f.iterations} | ${b.deadline_misses} / ${b.iterations} | ${f.latency_us.p99} / ${f.latency_us.max} | ${b.latency_us.p99} / ${b.latency_us.max} |`);
  }
  L.push('');
  L.push('## Fault-specific measurements');
  L.push('');
  for (const r of runs) {
    const parts: string[] = [];
    if (r.fault === 'kill_sensor') parts.push(`brake entered a safe state ${fmt(r.safety.brake_first_detect_ms, 2)} ms after the kill (reason: ${r.safety.brake_safe_reason}), held it for ${fmt(r.safety.brake_safe_duration_ms, 1)} ms, then resumed normal control`);
    if (r.fault === 'leak') parts.push(`infotainment peaked at ${r.infotainment.peak_alloc_mb} MB of its 192 MB address-space limit, then died alone`);
    if (r.fault === 'flood') parts.push(`${r.infotainment.flood_messages} flood messages were delivered to the cluster; its display latency peaked at ${r.cluster.fault_display_latency_max_us} us (baseline ${r.cluster.baseline_display_latency_max_us} us)`);
    if (r.fault === 'storm') parts.push(`CPU load peaked at ${r.cpu_peak_pct.join(' / ')} % (CPU 0 / 1 / 2)`);
    if (r.fault !== 'kill_sensor') parts.push(`brake sensor-link losses: ${r.safety.brake_link_lost_entries} (stale-sample entries caused by emulator pauses of the simulated plant: ${r.safety.brake_stale_entries})`);
    L.push(`- **${FAULT_LABELS[r.fault]}**: ${parts.join('; ')}.`);
  }
  L.push('');
  L.push('## Kernel traces (second pass, tracelogger running)');
  L.push('');
  L.push('`brake ready to running` is the time between the kernel making the brake thread READY and running it: pure');
  L.push('scheduler latency, measured from THREADY/THRUNNING events. It excludes timer delivery, which is where the');
  L.push('emulator adds its delay. Infotainment time on CPU 2 shows how long fault threads ran on the brake core; it is');
  L.push('approximate, because QNX 8.0 can log a scheduling event on a CPU other than the one the thread runs on. The');
  L.push("brake's own check of its CPU (off-CPU iterations, above) is exact.");
  L.push('');
  L.push('| Fault | Trace | Brake wake-ups | Ready to running p50 / p99, before fault (us) | After fault (us) | Waits over 100 us: CPU 2 silent (emulator) / CPU 2 busy | Longest wait (us) | Longest emulator stall (us) | Infotainment on CPU 2 (ms) |');
  L.push('|---|---|---|---|---|---|---|---|---|');
  for (const { run, trace } of traced) {
    if (!trace) { L.push(`| ${FAULT_LABELS[run.fault]} | failed | | | | | | | |`); continue; }
    const b = trace.brake_ready_to_running_before_fault_us, a = trace.brake_ready_to_running_after_fault_us;
    L.push(`| ${FAULT_LABELS[run.fault]} | [${run.fault}.kev.gz](traces/${run.fault}.kev.gz), [excerpt](traces/${run.fault}.excerpt.txt) | ${trace.brake_wakeups} | ${b.p50} / ${b.p99} | ${a.p50} / ${a.p99} | ${trace.platform_stalls} / ${trace.busy_waits_over_100us} | ${trace.brake_ready_to_running_us.max} | ${trace.platform_stall_max_us} | ${trace.fault_threads_on_brake_cpu_ms} |`);
  }
  L.push('');
  L.push('A wait counts as an emulator stall when the brake CPU logged no trace event at all for most of it while the');
  L.push('other CPUs kept logging: the emulated CPU was not executing, so no software on it could have run.');
  L.push('');
  L.push('Read a trace with the SDP host tool: `gunzip -k results/traces/crash.kev.gz && traceprinter -f results/traces/crash.kev`.');
  L.push('');
  L.push('## Platform noise');
  L.push('');
  const base = runs.reduce((a, r) => ({ m: a.m + r.brake.baseline.deadline_misses, n: a.n + r.brake.baseline.iterations }), { m: 0, n: 0 });
  L.push(`With no fault at all, the brake loop missed ${base.m} of ${base.n} deadlines across the baseline windows above`);
  L.push(`(${((1000 * base.m) / Math.max(1, base.n)).toFixed(1)} per 1000 periods). That is the emulator and the host, not QNX: QEMU here is TCG`);
  L.push('(pure emulation, no KVM in the cloud session), and the host is itself a shared cloud VM.');
  if (host) {
    L.push(`A 1 ms SCHED_FIFO timer loop on the bare host (\`scripts/hostlat.c\`, ${host.n} wake-ups, host CPU ${host.cpu}) measured`);
    L.push(`p50 ${host.p50_us} us, p99 ${host.p99_us} us, p99.9 ${host.p999_us} us, max ${host.max_us} us of lateness before QEMU or QNX are involved.`);
  }
  const tr = traced.map((t) => t.trace).filter((t): t is TraceAnalysis => !!t);
  if (tr.length) {
    const p99 = tr.map((t) => t.brake_ready_to_running_us.p99);
    const share = Math.max(...tr.map((t) => (t.platform_stalls + t.busy_waits_over_100us) / Math.max(1, t.brake_wakeups)));
    const silentMax = tr.filter((t) => t.brake_ready_to_running_us.max > 100 && t.platform_stall_max_us >= t.brake_ready_to_running_us.max - 0.1).length;
    L.push('The kernel traces above show where the time goes. Once the kernel makes the brake thread ready, it runs within');
    L.push(`tens of microseconds (p99 ${Math.min(...p99)} to ${Math.max(...p99)} us across the traces) before and during every fault. Waits over`);
    L.push(`100 us are at most ${(100 * share).toFixed(1)}% of wake-ups, and in ${silentMax} of the ${tr.length} traces the longest wait is a stall in which the`);
    L.push("brake's emulated CPU logged nothing at all. The rest of the lateness comes before the thread is made ready, in");
    L.push('timer expiry and interrupt delivery, which the emulator slows down (about 100 us even on an idle target, see');
    L.push('`docs/build-log.md`).');
  }
  L.push('On KVM the same image runs the same code, and `make test` additionally asserts zero deadline misses; that could');
  L.push('not be exercised in this session (no KVM).');
  L.push('');
  const vfile = path.join(repo, 'demo', 'video-run.json');
  if (fs.existsSync(vfile)) {
    const v = JSON.parse(fs.readFileSync(vfile, 'utf8')) as { recorded: string; runs: Record<string, RunResult>; reportCount: number };
    L.push('## The run shown in the video');
    L.push('');
    L.push(`\`demo/video.mp4\` is a screen recording of a separate live run (${v.recorded.slice(0, 16).replace('T', ' ')} UTC, same image, same`);
    L.push('platform). Its numbers, as shown on screen and spoken in the narration (raw data: `demo/video-run.json`):');
    L.push('');
    L.push('| Fault | Recovery | Brake periods run | Brake misses, fault / just before | Notes |');
    L.push('|---|---|---|---|---|');
    for (const f of FAULTS) {
      const r = v.runs[f];
      if (!r) continue;
      const rc = r.recovery;
      const rec = rc ? `${rc.cause}, ${fmt(rc.fault_to_ready_ms ?? rc.detect_to_ready_ms, 1)} ms ${rc.fault_to_ready_ms !== null ? 'fault to ready' : 'restart'}` : 'no restart needed';
      const note = f === 'kill_sensor' ? `brake detected the loss ${fmt(r.safety.brake_first_detect_ms, 2)} ms after the kill, safe state ${fmt(r.safety.brake_safe_duration_ms, 1)} ms`
        : f === 'leak' ? `peak ${r.infotainment.peak_alloc_mb} MB` : f === 'flood' ? `${r.infotainment.flood_messages} messages, display latency max ${r.cluster.fault_display_latency_max_us} us`
        : f === 'storm' ? `CPU peak ${r.cpu_peak_pct.join(' / ')} %` : '';
      L.push(`| ${FAULT_LABELS[f]} | ${rec} | ${r.brake.fault.iterations} | ${r.brake.fault.deadline_misses} / ${r.brake.baseline.deadline_misses} | ${note} |`);
    }
    L.push('');
    L.push(`The incident report generated in the video had all ${v.reportCount} of its numbers verified against the event log.`);
    L.push('');
  }
  return L.join('\n');
}

/** `results.ts --render`: rewrite results.md from results.json without measuring again. */
function render(): void {
  const d = JSON.parse(fs.readFileSync(path.join(outDir, 'results.json'), 'utf8')) as {
    recorded: string; platform: { accel: string }; host_noise: HostNoise | null; runs: RunResult[];
    traced: { fault: FaultName; run: RunResult; trace: TraceAnalysis | null }[];
  };
  fs.writeFileSync(path.join(outDir, 'results.md'), markdown(d.platform.accel, d.runs, d.traced, d.recorded, d.host_noise));
  log('rewrote results/results.md from results/results.json');
}

async function main(): Promise<void> {
  if (process.argv.includes('--render')) return render();
  const accel = accelerator();
  const only = process.argv.slice(2).filter((a) => (FAULTS as readonly string[]).includes(a)) as FaultName[];
  const faults = only.length ? only : [...FAULTS];
  log(`results run on QEMU ${accel}: ${faults.join(', ')}`);
  await ssh('true');
  const host = await hostNoise();
  log(`host noise probe: ${JSON.stringify(host)}`);
  const { tele, rec, close } = await session();
  await sleep(9000);

  const runs: RunResult[] = [];
  for (const f of faults) {
    log(`pass 1: ${f}`);
    runs.push(await runFault(tele, rec, f, { durationMs: 4000 }));
    await sleep(f === 'leak' ? 7000 : 5000);
  }
  const traced: { run: RunResult; trace: TraceAnalysis | null }[] = [];
  if (!process.env.CZ_NO_TRACE) {
    for (const f of faults) {
      log(`pass 2 (traced): ${f}`);
      traced.push(await tracedRun(tele, rec, f));
      await sleep(f === 'leak' ? 7000 : 5000);
    }
  }
  close();

  const when = new Date().toISOString().slice(0, 16).replace('T', ' ') + ' UTC';
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, 'results.json'), JSON.stringify({
    recorded: when, platform: { os: rec.hello ? `${rec.hello.system} ${rec.hello.release}` : 'QNX 8.0.0', accel, vcpus: 3 }, host_noise: host,
    runs, traced: traced.map((t) => ({ fault: t.run.fault, run: t.run, trace: t.trace })),
  }, null, 1));
  fs.writeFileSync(path.join(outDir, 'results.md'), markdown(accel, runs, traced, when, host));
  log('wrote results/results.md, results/results.json and results/traces/');
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
