// Crumple Zone: tests for statistics and chaos-run analysis.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
import { describe, expect, it } from 'vitest';
import { percentile, poissonUpper999, summarize } from '../src/shared/stats.js';
import { analyse, Recorder, summarizeWindow } from '../src/shared/runs.js';
import { SF_MISS, type EventMsg, type LoopStatus, type Sample } from '../src/shared/types.js';

describe('stats', () => {
  it('nearest-rank percentiles', () => {
    const s = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    expect(percentile(s, 50)).toBe(5);
    expect(percentile(s, 90)).toBe(9);
    expect(percentile(s, 99)).toBe(10);
    expect(percentile([], 50)).toBe(0);
  });
  it('summarize', () => {
    const r = summarize([5, 1, 3]);
    expect(r).toMatchObject({ n: 3, p50: 3, max: 5 });
    expect(r.mean).toBeCloseTo(3);
  });
  it('poisson bound grows with the baseline', () => {
    expect(poissonUpper999(0)).toBe(3);
    expect(poissonUpper999(30)).toBeGreaterThan(40);
    expect(poissonUpper999(30)).toBeLessThan(55);
  });
});

const status = (start: number, period: number): LoopStatus => ({ start_ns: start, period_ns: period } as LoopStatus);

function feed(rec: Recorder, id: 'brake' | 'steer', from: number, to: number, period: number, mk: (n: number) => Partial<{ lat: number; flags: number; skip: boolean }>) {
  const st = status(0, period);
  const s: Sample[] = [];
  for (let n = from; n < to; n++) {
    const o = mk(n);
    if (o.skip) continue;
    s.push([n, o.lat ?? 100_000, 50_000, 0, o.flags ?? 0, 0, 40_000]);
  }
  rec.ingest({ t: 'loop', id, ts: to * period, st, s });
}

describe('recorder and analysis', () => {
  it('counts misses and skipped releases in a window', () => {
    const rec = new Recorder();
    feed(rec, 'brake', 0, 1000, 1e6, (n) => ({ flags: n % 100 === 0 ? SF_MISS : 0, skip: n === 501 || n === 502 }));
    const w = summarizeWindow(rec, 'brake', 0, 1000e6);
    expect(w.iterations).toBe(998);
    expect(w.skipped_releases).toBe(2);
    expect(w.deadline_misses).toBe(10 + 2);
    expect(w.latency_us.p50).toBe(100);
  });

  it('counts releases skipped across a window edge', () => {
    const rec = new Recorder();
    // releases 495..504 skipped: the gap straddles t = 500 ms
    feed(rec, 'brake', 0, 1000, 1e6, (n) => ({ skip: n >= 495 && n <= 504 }));
    const before = summarizeWindow(rec, 'brake', 0, 500e6);
    const after = summarizeWindow(rec, 'brake', 500e6, 1000e6);
    expect(before.iterations + before.skipped_releases).toBe(before.expected);
    expect(after.iterations + after.skipped_releases).toBe(after.expected);
    expect(before.skipped_releases).toBe(5);
    expect(after.skipped_releases).toBe(5);
    // a window entirely inside the gap
    const inside = summarizeWindow(rec, 'brake', 497e6, 500e6);
    expect(inside.iterations).toBe(0);
    expect(inside.skipped_releases).toBe(3);
  });

  it('ignores replayed samples after a reconnect', () => {
    const rec = new Recorder();
    feed(rec, 'brake', 0, 100, 1e6, () => ({}));
    feed(rec, 'brake', 50, 150, 1e6, () => ({}));
    expect(rec.samples.brake.length).toBe(150);
  });

  it('derives recovery and safety facts from events', () => {
    const rec = new Recorder();
    feed(rec, 'brake', 0, 20000, 1e6, () => ({}));
    feed(rec, 'steer', 0, 10000, 2e6, () => ({}));
    const t0 = 12_000e6;
    const ev = (o: Partial<EventMsg>): EventMsg => ({ t: 'ev', ts: 0, src: 'guardian', kind: '', proc: '', pid: 0, a: 0, v0: 0, v1: 0, text: '', ...o });
    for (const e of [
      ev({ ts: t0, kind: 'fault_inject', proc: 'wheel_sensors', text: 'kill_sensor' }),
      ev({ ts: t0 + 0.4e6, src: 'brake_ctl', kind: 'safe_enter', a: 1, v0: 900_000 }),
      ev({ ts: t0 + 5e6, kind: 'death', proc: 'wheel_sensors', pid: 7, v0: 5e6, text: 'channel disconnect' }),
      ev({ ts: t0 + 6e6, kind: 'exit_status', proc: 'wheel_sensors', pid: 7, a: 9, text: 'SIGKILL' }),
      ev({ ts: t0 + 15e6, kind: 'restart', proc: 'wheel_sensors', pid: 9, a: 5, v0: 15e6, v1: 10e6, text: 'within budget' }),
      ev({ ts: t0 + 20e6, src: 'brake_ctl', kind: 'safe_exit', v0: 19.6e6 }),
    ]) rec.ingest(e);
    const r = analyse(rec, 'kill_sensor', t0, 3000e6, t0 - 3000e6);
    expect(r.recovery).toMatchObject({ process: 'wheel_sensors', cause: 'SIGKILL', detection_path: 'channel disconnect', fault_to_ready_ms: 15, detect_to_ready_ms: 10, within_budget: true });
    expect(r.safety.brake_safe_entries).toBe(1);
    expect(r.safety.brake_first_detect_ms).toBe(0.4);
    expect(r.safety.brake_safe_duration_ms).toBe(19.6);
    expect(r.safety.brake_safe_reason).toContain('read error');
    expect(r.brake.fault.iterations).toBe(3000);
    expect(r.brake_miss_excess).toBe(false);
  });
});
