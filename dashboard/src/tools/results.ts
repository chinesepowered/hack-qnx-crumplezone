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
import { FAULTS, FAULT_LABELS, type FaultName } from '../shared/types.js';
import { runFault, sleep, type Recorder, type RunResult } from '../shared/runs.js';
import { accelerator, log, repo, scpFrom, session, ssh, sshBackground, traceprinter } from './qnx.js';
import { analyseTrace, excerpt, type TraceAnalysis } from './trace.js';

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

function markdown(accel: string, runs: RunResult[], traced: { run: RunResult; trace: TraceAnalysis | null }[], when: string): string {
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
    if (r.fault !== 'kill_sensor') parts.push(`brake safe-state entries: ${r.safety.brake_safe_entries}`);
    L.push(`- **${FAULT_LABELS[r.fault]}**: ${parts.join('; ')}.`);
  }
  L.push('');
  L.push('## Kernel traces (second pass, tracelogger running)');
  L.push('');
  L.push('`brake ready to running` is the time between the kernel making the brake thread READY and running it: pure');
  L.push('scheduler latency, measured from THREADY/THRUNNING events. It excludes timer delivery, which is where the');
  L.push('emulator adds its delay. Infotainment time on CPU 2 shows how long fault threads ran on the brake core.');
  L.push('');
  L.push('| Fault | Trace | Brake wake-ups | Ready to running p50 / p99, before fault (us) | After fault (us) | Waits over 100 us: CPU 2 silent (emulator) / CPU 2 busy | Longest emulator stall (us) | Infotainment on CPU 2 (ms) |');
  L.push('|---|---|---|---|---|---|---|---|');
  for (const { run, trace } of traced) {
    if (!trace) { L.push(`| ${FAULT_LABELS[run.fault]} | failed | | | | | | |`); continue; }
    const b = trace.brake_ready_to_running_before_fault_us, a = trace.brake_ready_to_running_after_fault_us;
    L.push(`| ${FAULT_LABELS[run.fault]} | [${run.fault}.kev.gz](traces/${run.fault}.kev.gz), [excerpt](traces/${run.fault}.excerpt.txt) | ${trace.brake_wakeups} | ${b.p50} / ${b.p99} | ${a.p50} / ${a.p99} | ${trace.platform_stalls} / ${trace.busy_waits_over_100us} | ${trace.platform_stall_max_us} | ${trace.fault_threads_on_brake_cpu_ms} |`);
  }
  L.push('');
  L.push('A wait counts as an emulator stall when the brake CPU logged no trace event at all for most of it while the');
  L.push('other CPUs kept logging: the emulated CPU was not executing, so no software on it could have run.');
  L.push('');
  L.push('Read a trace with the SDP host tool: `gunzip -k results/traces/crash.kev.gz && traceprinter -f results/traces/crash.kev`.');
  L.push('');
  return L.join('\n');
}

async function main(): Promise<void> {
  const accel = accelerator();
  const only = process.argv.slice(2).filter((a) => (FAULTS as readonly string[]).includes(a)) as FaultName[];
  const faults = only.length ? only : [...FAULTS];
  log(`results run on QEMU ${accel}: ${faults.join(', ')}`);
  await ssh('true');
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
    recorded: when, platform: { os: rec.hello ? `${rec.hello.system} ${rec.hello.release}` : 'QNX 8.0.0', accel, vcpus: 3 },
    runs, traced: traced.map((t) => ({ fault: t.run.fault, run: t.run, trace: t.trace })),
  }, null, 1));
  fs.writeFileSync(path.join(outDir, 'results.md'), markdown(accel, runs, traced, when));
  log('wrote results/results.md, results/results.json and results/traces/');
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
