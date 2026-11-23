// Crumple Zone: kernel-trace analysis (traceprinter text from tracelogger).
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// Separates what QNX decided from what the emulator did. For the brake thread
// it measures READY -> RUNNING latency (time spent in the scheduler only),
// before and after the fault marker. Timer delivery latency, which TCG adds
// before the thread is even made ready, is not part of this number. It also
// totals the time fault threads (infotainment) ran on the brake CPU. Note: in
// QNX 8.0 a scheduling event can be logged by a CPU other than the one the
// thread runs on, so per-CPU attribution of THRUNNING events is approximate;
// the brake loop itself counts iterations that ran off its pinned CPU
// (off_cpu in telemetry), which stays at zero.

import fs from 'node:fs';
import readline from 'node:readline';
import { summarize, round, type Summary } from '../shared/stats.js';

export interface TraceTarget {
  brakePid: number;
  brakeTid: number;
  brakePrio: number;
  brakeCpu: number;
  faultPids: number[];
}

export interface TraceAnalysis {
  duration_ms: number;
  events: number;
  cps: number;
  fault_at_ms: number | null;
  brake_wakeups: number;
  /** brake thread READY -> RUNNING: time spent in the scheduler only */
  brake_ready_to_running_us: Summary;
  brake_ready_to_running_before_fault_us: Summary;
  brake_ready_to_running_after_fault_us: Summary;
  /** waits over 100 us during which the brake CPU logged nothing at all: the emulated CPU was frozen */
  platform_stalls: number;
  platform_stall_max_us: number;
  /** waits over 100 us during which the brake CPU was logging other work */
  busy_waits_over_100us: number;
  fault_threads_on_brake_cpu_ms: number;
  ipis_on_brake_cpu: number;
  markers: { t_ms: number; cpu: number; text: string }[];
}

const RE = /^t:0x([0-9a-f]+) CPU:(\d+) (\S+)\s+:(\S+)\s*(.*)$/;

export async function analyseTrace(file: string, tgt: TraceTarget): Promise<TraceAnalysis> {
  const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
  let cps = 0;
  let t0: bigint | null = null;
  let tLast = 0n;
  let events = 0;
  const running = new Map<number, { pid: number; tid: number; prio: number; since: bigint }>();
  let readyAt: bigint | null = null;
  const r2r: number[] = [];
  const r2rAt: number[] = [];
  let ipis = 0;
  let faultNs = 0;
  let faultAt: number | null = null;
  let lastOnBrakeCpu: bigint | null = null;
  let maxSilence = 0n;
  let stalls = 0, stallMax = 0, busyWaits = 0;
  const markers: TraceAnalysis['markers'] = [];
  const faultSet = new Set(tgt.faultPids);
  const ns = (t: bigint) => (cps ? Number(((t - (t0 ?? 0n)) * 1_000_000_000n) / BigInt(cps)) : 0);

  for await (const line of rl) {
    if (!cps) {
      const m = line.match(/TRACE_CYCLES_PER_SEC::\s*(\d+)/);
      if (m) cps = Number(m[1]);
      continue;
    }
    const m = RE.exec(line);
    if (!m) continue;
    const t = BigInt('0x' + m[1]);
    if (t0 === null) t0 = t;
    tLast = t;
    events++;
    const cpu = Number(m[2]);
    const cls = m[3], ev = m[4], rest = m[5];
    if (cpu === tgt.brakeCpu) {
      if (readyAt !== null && lastOnBrakeCpu !== null && t - lastOnBrakeCpu > maxSilence) maxSilence = t - lastOnBrakeCpu;
      lastOnBrakeCpu = t;
    }
    if (cls === 'THREAD') {
      const pm = rest.match(/pid:(\d+) tid:(\d+) priority:(\d+)/);
      if (!pm) continue;
      const pid = Number(pm[1]), tid = Number(pm[2]), prio = Number(pm[3]);
      const isBrake = pid === tgt.brakePid && tid === tgt.brakeTid;
      if (ev === 'THREADY' && isBrake && readyAt === null) {
        readyAt = t;
        maxSilence = 0n;
      }
      if (ev === 'THRUNNING') {
        const prev = running.get(cpu);
        if (cpu === tgt.brakeCpu && prev && faultSet.has(prev.pid)) faultNs += Number(((t - prev.since) * 1_000_000_000n) / BigInt(cps));
        running.set(cpu, { pid, tid, prio, since: t });
        if (isBrake && readyAt !== null) {
          const us = Number(((t - readyAt) * 1_000_000_000n) / BigInt(cps)) / 1e3;
          r2r.push(us);
          r2rAt.push(ns(t));
          if (us > 100) {
            const silentUs = Number((maxSilence * 1_000_000_000n) / BigInt(cps)) / 1e3;
            if (silentUs > 0.8 * us) {
              stalls++;
              if (us > stallMax) stallMax = us;
            } else {
              busyWaits++;
            }
          }
          readyAt = null;
        }
      }
    } else if (cls === 'SYSTEM' && ev === 'IPI_64' && cpu === tgt.brakeCpu) {
      ipis++;
    } else if (cls === 'USREVENT') {
      const sm = rest.match(/STR:"([^"]*)"/);
      if (sm && !sm[1].includes('deadline miss')) {
        markers.push({ t_ms: round(ns(t) / 1e6, 3), cpu, text: sm[1] });
        if (faultAt === null && (sm[1].startsWith('CZ fault inject') || sm[1].includes(' died '))) faultAt = ns(t);
      }
    }
  }
  const r = (v: number[]): Summary => {
    const s = summarize(v);
    return { n: s.n, mean: round(s.mean, 2), p50: round(s.p50, 2), p99: round(s.p99, 2), p999: round(s.p999, 2), max: round(s.max, 2) };
  };
  const before = r2r.filter((_, i) => faultAt === null || r2rAt[i] < faultAt);
  const after = r2r.filter((_, i) => faultAt !== null && r2rAt[i] >= faultAt);
  return {
    duration_ms: round(ns(tLast) / 1e6, 1),
    events,
    cps,
    fault_at_ms: faultAt === null ? null : round(faultAt / 1e6, 3),
    brake_wakeups: r2r.length,
    brake_ready_to_running_us: r(r2r),
    brake_ready_to_running_before_fault_us: r(before),
    brake_ready_to_running_after_fault_us: r(after),
    platform_stalls: stalls,
    platform_stall_max_us: round(stallMax, 1),
    busy_waits_over_100us: busyWaits,
    fault_threads_on_brake_cpu_ms: round(faultNs / 1e6, 2),
    ipis_on_brake_cpu: ipis,
    markers,
  };
}

/** The lines of a traceprinter file around the first marker containing `needle`. */
export async function excerpt(file: string, needle: string, before = 40, after = 160): Promise<string> {
  const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
  const header: string[] = [];
  const ring: string[] = [];
  const out: string[] = [];
  let inHeader = true, found = false, left = after;
  for await (const line of rl) {
    if (inHeader) {
      header.push(line);
      if (line.includes('KERNEL EVENTS')) inHeader = false;
      continue;
    }
    if (!line.startsWith('t:')) continue; // drop multi-line kernel call payloads
    if (!found) {
      ring.push(line);
      if (ring.length > before) ring.shift();
      if (line.includes(needle)) {
        found = true;
        out.push(...ring);
      }
    } else {
      out.push(line);
      if (--left <= 0) break;
    }
  }
  return [...header, ...(found ? out : ['(marker not found)'])].join('\n') + '\n';
}
