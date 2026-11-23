// Crumple Zone: tests for the incident-report number verifier.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
import { describe, expect, it } from 'vitest';
import { extractNumbers, flagUnverified, logCandidates, verifyReport } from '../src/shared/verify.js';

const log = {
  system: { os: 'QNX 8.0.0', standard: 'ISO 26262', brake_loop_rate_hz: 1000, brake_priority: 60, brake_cpu: 2, recovery_budget_ms: 50 },
  recovery: { process: 'infotainment', cause: 'SIGSEGV', fault_to_detect_ms: 4.685, detect_to_ready_ms: 8.312, fault_to_ready_ms: 13.871 },
  brake_loop: { baseline_window: { deadline_misses: 31, latency_p99_us: 742.5 }, fault_window: { deadline_misses: 29, latency_p99_us: 760.1, latency_max_us: 3120.4 } },
  infotainment: { peak_allocation_mb: 188 },
  cpu_peak_load_pct: [64.6, 65.1, 58.1],
  timeline: [{ t_ms: 0.012, detail: 'crash' }],
};

describe('extractNumbers', () => {
  it('finds numbers with units and ignores identifiers', () => {
    const t = extractNumbers('steer_ctl on CPU2 recovered in 13.9 ms; p99 760 us; x86_64; R1 met');
    expect(t.map((x) => [x.value, x.unit])).toEqual([[13.9, 'ms'], [760, 'us']]);
  });
  it('skips markdown list markers and headings but keeps numbers after them', () => {
    const t = extractNumbers('## 1. Summary\n1. Restart took 8.3 ms\n2) none');
    expect(t.map((x) => x.value)).toEqual([8.3]);
  });
  it('reads thousands separators, percent and decimals', () => {
    const t = extractNumbers('handled 12,480 messages, CPU 65.1 %, 188 MB');
    expect(t.map((x) => [x.value, x.unit, x.decimals])).toEqual([[12480, '', 0], [65.1, 'pct', 1], [188, 'mb', 0]]);
  });
  it('understands microsecond spellings', () => {
    expect(extractNumbers('742 µs and 5 microseconds').map((x) => x.unit)).toEqual(['us', 'us']);
  });
});

describe('logCandidates', () => {
  it('expands unit-suffixed keys into compatible units', () => {
    const c = logCandidates({ a_ms: 13.871, b_us: 742.5, c_hz: 1000 });
    expect(c.some((x) => x.unit === 'us' && Math.abs(x.value - 13871) < 1e-6)).toBe(true);
    expect(c.some((x) => x.unit === 'ms' && Math.abs(x.value - 0.7425) < 1e-9)).toBe(true);
    expect(c.some((x) => x.unit === 'khz' && x.value === 1)).toBe(true);
  });
  it('takes numbers out of strings too', () => {
    expect(logCandidates({ os: 'QNX 8.0.0', standard: 'ISO 26262' }).map((x) => x.value)).toEqual([8.0, 26262]);
  });
});

describe('verifyReport', () => {
  it('accepts a faithful report, including rounding and unit changes', () => {
    const text = `## Summary
Infotainment crashed with SIGSEGV on QNX 8.0 and was back 13.9 ms after the fault (detected after 4.7 ms, restart 8.3 ms), inside the 50 ms budget.
## Timing impact on safety loops
Brake deadline misses were 29 in the fault window against 31 at baseline; p99 latency 760 us versus 742.5 us (0.76 ms). The worst case was 3.12 ms.
Peak CPU load 65.1%, leak peak 188 MB. Loop rate 1 kHz at priority 60 on CPU 2, ISO 26262.`;
    const v = verifyReport(text, log);
    expect(v.unverified.map((t) => t.raw)).toEqual([]);
    expect(v.ok).toBe(true);
  });
  it('rejects invented numbers', () => {
    const v = verifyReport('Recovery took 9.4 ms and misses fell by 6%.', log);
    expect(v.ok).toBe(false);
    expect(v.unverified.map((t) => t.raw.trim())).toEqual(['9.4 ms', '6%']);
  });
  it('does not let an explicit unit match the wrong dimension', () => {
    // 188 exists as MB, not as ms
    expect(verifyReport('the restart took 188 ms', log).ok).toBe(false);
    expect(verifyReport('the leak reached 188 MB', log).ok).toBe(true);
  });
  it('respects the precision the text claims', () => {
    expect(verifyReport('restart 8.31 ms', log).ok).toBe(true);
    expect(verifyReport('restart 8.35 ms', log).ok).toBe(false);
  });
  it('flags unverified numbers in place', () => {
    const text = 'took 9.4 ms, budget 50 ms';
    const v = verifyReport(text, log);
    expect(flagUnverified(text, v.unverified)).toBe('took [[UNVERIFIED:9.4 ms]], budget 50 ms');
  });
});
