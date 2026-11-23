// Crumple Zone: telemetry recorder and chaos-run bookkeeping, shared by the
// dashboard server, the smoke test and the results runner.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

import { summarize, round, poissonUpper999, type Summary } from './stats.js';
import {
  FAULT_LABELS, SF_MISS, SF_OVERRUN, SF_SAFE, SF_STALE,
  type AckMsg, type ClusterMsg, type EventMsg, type FaultName, type HelloMsg, type InfotMsg,
  type LoopId, type LoopStatus, type ProcMsg, type TelemetryMsg,
} from './types.js';

export interface SampleRec {
  n: number;
  t: number; // release time, guest ns
  lat: number;
  exec: number;
  jit: number;
  flags: number;
  mode: number;
  io: number;
}

export interface Snapshot<T> {
  guest: number;
  msg: T;
}

export class Recorder {
  hello: HelloMsg | null = null;
  status: Partial<Record<LoopId, LoopStatus>> = {};
  samples: Record<LoopId, SampleRec[]> = { brake: [], steer: [] };
  events: EventMsg[] = [];
  procs: Snapshot<ProcMsg>[] = [];
  cluster: Snapshot<ClusterMsg>[] = [];
  infot: Snapshot<InfotMsg>[] = [];
  guestNow = 0;
  private lastN: Record<LoopId, number> = { brake: -1, steer: -1 };
  private seen = new Set<string>();
  constructor(public keepNs = 300e9) {}

  ingest(m: TelemetryMsg): void {
    switch (m.t) {
      case 'hello':
        this.hello = m;
        this.guestNow = Math.max(this.guestNow, m.now_ns);
        break;
      case 'loop': {
        this.status[m.id] = m.st;
        this.guestNow = Math.max(this.guestNow, m.ts);
        const arr = this.samples[m.id];
        for (const s of m.s) {
          const [n, lat, exec, jit, flags, mode, io] = s;
          if (n <= this.lastN[m.id] && arr.length) continue; // replayed backlog after reconnect
          this.lastN[m.id] = n;
          arr.push({ n, t: m.st.start_ns + n * m.st.period_ns, lat, exec, jit, flags, mode, io: io ?? 0 });
        }
        this.trim(arr);
        break;
      }
      case 'ev': {
        const key = `${m.ts}:${m.src}:${m.kind}:${m.pid}:${m.v0}`;
        if (this.seen.has(key)) break;
        this.seen.add(key);
        this.events.push(m);
        this.guestNow = Math.max(this.guestNow, m.ts);
        if (this.events.length > 20000) this.events.splice(0, 5000);
        break;
      }
      case 'proc':
        this.guestNow = Math.max(this.guestNow, m.ts);
        this.procs.push({ guest: m.ts, msg: m });
        if (this.procs.length > 1200) this.procs.shift();
        break;
      case 'cluster':
        this.cluster.push({ guest: this.guestNow, msg: m });
        if (this.cluster.length > 3000) this.cluster.shift();
        break;
      case 'infot':
        this.infot.push({ guest: this.guestNow, msg: m });
        if (this.infot.length > 3000) this.infot.shift();
        break;
      default:
        break;
    }
  }

  private trim(arr: SampleRec[]): void {
    if (arr.length < 2) return;
    const cutoff = arr[arr.length - 1].t - this.keepNs;
    let k = 0;
    while (k < arr.length && arr[k].t < cutoff) k++;
    if (k > 10000) arr.splice(0, k);
  }

  window(id: LoopId, t0: number, t1: number): SampleRec[] {
    const arr = this.samples[id];
    // binary search for t0
    let lo = 0, hi = arr.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (arr[mid].t < t0) lo = mid + 1; else hi = mid;
    }
    const out: SampleRec[] = [];
    for (let i = lo; i < arr.length && arr[i].t < t1; i++) out.push(arr[i]);
    return out;
  }

  eventsBetween(t0: number, t1: number): EventMsg[] {
    return this.events.filter((e) => e.ts >= t0 && e.ts < t1);
  }
}

export interface LoopWindow {
  iterations: number;
  expected: number;
  deadline_misses: number;
  skipped_releases: number;
  stale_iterations: number;
  safe_iterations: number;
  latency_us: Summary;
  exec_us: Summary;
  sensor_io_us: Summary;
  jitter_max_us: number;
}

export function summarizeWindow(rec: Recorder, id: LoopId, t0: number, t1: number): LoopWindow {
  const w = rec.window(id, t0, t1);
  const st = rec.status[id];
  const period = st?.period_ns ?? (id === 'brake' ? 1e6 : 2e6);
  const lat = w.map((s) => s.lat / 1e3);
  const exec = w.map((s) => s.exec / 1e3);
  const io = w.map((s) => s.io / 1e3);
  let jmax = 0, misses = 0, skipped = 0, stale = 0, safe = 0;
  for (let i = 0; i < w.length; i++) {
    const s = w[i];
    if (i > 0 && Math.abs(s.jit) > jmax) jmax = Math.abs(s.jit);
    if (s.flags & SF_MISS) misses++;
    if (s.flags & SF_STALE) stale++;
    if (s.flags & SF_SAFE) safe++;
    if (i > 0 && s.n > w[i - 1].n + 1) skipped += s.n - w[i - 1].n - 1;
  }
  const r = (x: Summary): Summary => ({
    n: x.n, mean: round(x.mean, 1), p50: round(x.p50, 1), p99: round(x.p99, 1), p999: round(x.p999, 1), max: round(x.max, 1),
  });
  return {
    iterations: w.length,
    expected: Math.round((t1 - t0) / period),
    deadline_misses: misses + skipped,
    skipped_releases: skipped,
    stale_iterations: stale,
    safe_iterations: safe,
    latency_us: r(summarize(lat)),
    exec_us: r(summarize(exec)),
    sensor_io_us: r(summarize(io)),
    jitter_max_us: round(jmax / 1e3, 1),
  };
}

export interface Recovery {
  process: string;
  cause: string;
  detection_path: string;
  fault_to_detect_ms: number | null;
  detect_to_ready_ms: number;
  fault_to_ready_ms: number | null;
  within_budget: boolean;
  new_pid: number;
  old_pid: number;
}

export interface RunResult {
  id: string;
  fault: FaultName;
  label: string;
  ok: boolean;
  error?: string;
  t_fault_ns: number;
  window_ms: number;
  brake: { baseline: LoopWindow; fault: LoopWindow };
  steer: { baseline: LoopWindow; fault: LoopWindow };
  brake_off_cpu_iterations: number; // brake iterations that ran off the pinned CPU during the run
  brake_miss_excess: boolean; // more misses than the baseline rate explains (99.9% Poisson bound)
  steer_miss_excess: boolean;
  recovery: Recovery | null;
  safety: {
    brake_safe_entries: number;
    brake_first_detect_ms: number | null;
    brake_safe_duration_ms: number | null;
    brake_safe_reason: string | null;
    steer_safe_entries: number;
  };
  cluster: { baseline_display_latency_max_us: number; fault_display_latency_max_us: number; media_messages: number };
  infotainment: { peak_alloc_mb: number; flood_messages: number; restarts: number };
  cpu_peak_pct: number[];
  events: EventMsg[];
}

export const RECOVERY_BUDGET_MS = 50;

const SAFE_REASONS = ['none', 'read error (driver gone)', 'timeout (driver hung)', 'stale sample', 'no driver connection'];

export interface Link {
  send(line: string): void;
  nextAck(cmd: string, timeoutMs: number): Promise<AckMsg>;
}

export const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

export interface RunOptions {
  windowMs?: number;
  durationMs?: number;
  onPhase?: (phase: string) => void;
}

export function defaultWindowMs(f: FaultName): number {
  switch (f) {
    case 'storm':
    case 'flood':
      return 7000;
    case 'leak':
      return 6000;
    default:
      return 3000;
  }
}

/**
 * Inject one fault and measure it against an equal-length baseline window
 * taken from the recorded history immediately before the injection, so the
 * fault goes in the moment it is requested.
 */
export async function runFault(link: Link, rec: Recorder, fault: FaultName, opts: RunOptions = {}): Promise<RunResult> {
  const windowMs = opts.windowMs ?? defaultWindowMs(fault);
  const durationMs = opts.durationMs ?? 5000;
  const phase = opts.onPhase ?? (() => {});

  // need at least one full baseline window of history
  for (let i = 0; i < 200; i++) {
    const b = rec.samples.brake;
    if (b.length > 1 && b[b.length - 1].t - b[0].t > (windowMs + 500) * 1e6) break;
    phase('collecting baseline');
    await sleep(100);
  }
  const offCpu0 = rec.status.brake?.off_cpu ?? 0;
  link.send('reset');
  phase('inject');
  link.send(`fault ${fault} ${durationMs}`);
  const ack = await link.nextAck('fault', 5000).catch((e: Error) => ({ t: 'ack', cmd: 'fault', ok: false, err: e.message }) as AckMsg);
  if (!ack.ok || !ack.t_fault) {
    return emptyResult(fault, `fault not injected: ${ack.err ?? 'no ack'}`, windowMs);
  }
  const t0 = ack.t_fault;
  phase('observe');
  let waited = 0;
  // A leak runs until the address-space limit is hit; wait for the restart, then observe.
  if (fault === 'leak') {
    while (waited < 20000 && !rec.events.some((e) => e.kind === 'restart' && e.proc === 'infotainment' && e.ts > t0)) {
      await sleep(250);
      waited += 250;
    }
  }
  const winNs = (waited + windowMs) * 1e6;
  for (let i = 0; i < 400 && rec.guestNow < t0 + winNs + 300e6; i++) await sleep(50);
  phase('analyse');
  const res = analyse(rec, fault, t0, winNs, t0 - winNs);
  res.brake_off_cpu_iterations = (rec.status.brake?.off_cpu ?? 0) - offCpu0;
  return res;
}

function emptyWindow(): LoopWindow {
  const z = { n: 0, mean: 0, p50: 0, p99: 0, p999: 0, max: 0 };
  return { iterations: 0, expected: 0, deadline_misses: 0, skipped_releases: 0, stale_iterations: 0, safe_iterations: 0,
    latency_us: z, exec_us: z, sensor_io_us: z, jitter_max_us: 0 };
}

function emptyResult(fault: FaultName, error: string, windowMs: number): RunResult {
  return {
    id: `${fault}-${Date.now()}`, fault, label: FAULT_LABELS[fault], ok: false, error, t_fault_ns: 0, window_ms: windowMs,
    brake: { baseline: emptyWindow(), fault: emptyWindow() }, steer: { baseline: emptyWindow(), fault: emptyWindow() },
    brake_off_cpu_iterations: 0, brake_miss_excess: false, steer_miss_excess: false, recovery: null,
    safety: { brake_safe_entries: 0, brake_first_detect_ms: null, brake_safe_duration_ms: null, brake_safe_reason: null, steer_safe_entries: 0 },
    cluster: { baseline_display_latency_max_us: 0, fault_display_latency_max_us: 0, media_messages: 0 },
    infotainment: { peak_alloc_mb: 0, flood_messages: 0, restarts: 0 }, cpu_peak_pct: [], events: [],
  };
}

/** Compute every metric of a run from the recorder (pure, unit tested). */
export function analyse(rec: Recorder, fault: FaultName, t0: number, winNs: number, baseT0: number): RunResult {
  const t1 = t0 + winNs;
  const res = emptyResult(fault, '', winNs / 1e6);
  res.ok = true;
  delete res.error;
  res.id = `${fault}-${Math.round(t0 / 1e6)}`;
  res.t_fault_ns = t0;
  res.brake = { baseline: summarizeWindow(rec, 'brake', baseT0, baseT0 + winNs), fault: summarizeWindow(rec, 'brake', t0, t1) };
  res.steer = { baseline: summarizeWindow(rec, 'steer', baseT0, baseT0 + winNs), fault: summarizeWindow(rec, 'steer', t0, t1) };
  res.brake_miss_excess = res.brake.fault.deadline_misses > poissonUpper999(res.brake.baseline.deadline_misses);
  res.steer_miss_excess = res.steer.fault.deadline_misses > poissonUpper999(res.steer.baseline.deadline_misses);

  const evs = rec.eventsBetween(t0 - 50e6, t1);
  res.events = evs.filter((e) => e.kind !== 'deadline_miss').slice(0, 200);
  const target = fault === 'kill_sensor' ? 'wheel_sensors' : 'infotainment';

  const death = evs.find((e) => e.kind === 'death' && e.proc === target && e.ts >= t0);
  const restart = evs.find((e) => e.kind === 'restart' && e.proc === target && e.ts >= t0);
  const exit = evs.find((e) => e.kind === 'exit_status' && e.proc === target && e.ts >= t0);
  if (restart) {
    const f2r = restart.v0 >= 0 && fault !== 'leak' ? restart.v0 / 1e6 : null;
    const d2r = restart.v1 / 1e6;
    res.recovery = {
      process: target,
      cause: exit?.text ?? 'unknown',
      detection_path: death?.text ?? 'unknown',
      fault_to_detect_ms: death && death.v0 >= 0 && fault !== 'leak' ? round(death.v0 / 1e6, 3) : null,
      detect_to_ready_ms: round(d2r, 3),
      fault_to_ready_ms: f2r === null ? null : round(f2r, 3),
      within_budget: (f2r ?? d2r) <= RECOVERY_BUDGET_MS,
      new_pid: restart.pid,
      old_pid: death?.pid ?? 0,
    };
  }

  const bEnter = evs.filter((e) => e.src === 'brake_ctl' && e.kind === 'safe_enter' && e.ts >= t0);
  const bExit = evs.filter((e) => e.src === 'brake_ctl' && e.kind === 'safe_exit' && e.ts >= t0);
  res.safety.brake_safe_entries = bEnter.length;
  res.safety.steer_safe_entries = evs.filter((e) => e.src === 'steer_ctl' && e.kind === 'safe_enter' && e.ts >= t0).length;
  if (bEnter.length) {
    const first = bEnter[0];
    res.safety.brake_first_detect_ms = round((first.ts - t0) / 1e6, 3);
    res.safety.brake_safe_reason = SAFE_REASONS[first.a] ?? String(first.a);
    const ex = bExit.find((e) => e.ts >= first.ts);
    res.safety.brake_safe_duration_ms = ex ? round(ex.v0 / 1e6, 3) : null;
  }

  const clMax = (a: number, b: number) => Math.max(0, ...rec.cluster.filter((c) => c.guest >= a && c.guest < b).map((c) => c.msg.lat_us));
  res.cluster.baseline_display_latency_max_us = clMax(baseT0, baseT0 + winNs);
  res.cluster.fault_display_latency_max_us = clMax(t0, t1);
  const cl0 = rec.cluster.find((c) => c.guest >= t0)?.msg.media ?? 0;
  const cl1 = [...rec.cluster].reverse().find((c) => c.guest < t1)?.msg.media ?? cl0;
  res.cluster.media_messages = Math.max(0, cl1 - cl0);

  const inf = rec.infot.filter((s) => s.guest >= t0 && s.guest < t1 + 2e9);
  res.infotainment.peak_alloc_mb = Math.max(0, ...inf.map((s) => s.msg.alloc_mb));
  const fend = evs.find((e) => e.kind === 'fault_end' && e.ts >= t0);
  res.infotainment.flood_messages = fault === 'flood' && fend ? fend.v1 : 0;
  if (fault === 'flood' && inf.length > 1) {
    res.infotainment.flood_messages = Math.max(res.infotainment.flood_messages,
      inf[inf.length - 1].msg.flood_sent - (rec.infot.find((s) => s.guest >= t0)?.msg.flood_sent ?? 0));
  }
  res.infotainment.restarts = evs.filter((e) => e.kind === 'restart' && e.proc === 'infotainment' && e.ts >= t0).length;

  const procs = rec.procs.filter((p) => p.guest >= t0 && p.guest < t1);
  const ncpu = procs[0]?.msg.cpus.length ?? 0;
  res.cpu_peak_pct = Array.from({ length: ncpu }, (_, i) => round(Math.max(0, ...procs.map((p) => p.msg.cpus[i] ?? 0)), 1));
  return res;
}

/** Flags-aware helpers for the UI */
export const isMiss = (flags: number) => (flags & (SF_MISS | SF_OVERRUN)) !== 0;
