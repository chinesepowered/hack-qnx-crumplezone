// Crumple Zone: AI incident report from the structured event log of a chaos run.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// The model only summarises. Every number in its report is checked against the
// log by code (verify.ts). A report with an unverified number is rejected and
// regenerated once with the offending numbers named; if the second attempt
// still has one, it is shown with each mismatch flagged.

import { chat, type ChatMessage } from './llm.js';
import { flagUnverified, verifyReport } from '../shared/verify.js';
import { RECOVERY_BUDGET_MS, type RunResult } from '../shared/runs.js';
import type { HelloMsg } from '../shared/types.js';

export interface PlatformInfo {
  accel: 'TCG' | 'KVM' | string;
  vcpus: number;
}

export interface IncidentReport {
  status: 'verified' | 'flagged' | 'error';
  model: string;
  attempts: number;
  text: string;
  flagged_text?: string;
  unverified: string[];
  verified_count: number;
  generated_ms: number;
  error?: string;
  log: unknown;
}

const DESCRIPTIONS: Record<string, string> = {
  crash: 'infotainment process dereferences a NULL pointer (segmentation fault)',
  storm: 'infotainment starts one spinning thread pinned to every CPU, at its own low priority',
  leak: 'infotainment allocates and touches memory until its address-space limit makes malloc fail, then aborts',
  flood: 'infotainment floods the instrument cluster with 1 KB messages from four threads',
  kill_sensor: 'the wheel-speed and pedal sensor driver (a resource manager) is killed with SIGKILL',
};

/** The structured log handed to the model. Keys carry their units. */
export function buildIncidentLog(run: RunResult, hello: HelloMsg | null, platform: PlatformInfo): Record<string, unknown> {
  const loop = (w: RunResult['brake']['fault']) => ({
    iterations: w.iterations,
    deadline_misses: w.deadline_misses,
    latency_p50_us: w.latency_us.p50,
    latency_p99_us: w.latency_us.p99,
    latency_max_us: w.latency_us.max,
    exec_p99_us: w.exec_us.p99,
    sensor_read_p99_us: w.sensor_io_us.p99,
    jitter_max_us: w.jitter_max_us,
    safe_state_iterations: w.safe_iterations,
  });
  const t0 = run.t_fault_ns;
  return {
    system: {
      product: 'Crumple Zone mixed-criticality vehicle computer',
      os: `${hello?.system ?? 'QNX'} ${hello?.release ?? '8.0.0'}`,
      platform: `QEMU ${platform.accel}, ${platform.vcpus} vCPU x86_64`,
      standard: 'ISO 26262',
      brake_loop_rate_hz: 1000,
      steer_loop_rate_hz: 500,
      brake_priority: 60,
      steer_priority: 55,
      infotainment_priority: 8,
      brake_cpu: 2,
      steer_cpu: 1,
      infotainment_address_space_limit_mb: 192,
      recovery_budget_ms: RECOVERY_BUDGET_MS,
      deadline_rule: 'a control iteration must finish before the next release (1 ms brake, 2 ms steering)',
      noise_note: 'on this emulated platform deadline misses occur with no fault at all (emulator and shared cloud host). misses_beyond_platform_noise compares the fault window with the noisiest fault-free window of the same length in the previous minute; only that flag says whether the fault affected a loop',
    },
    fault: { name: run.fault, label: run.label, description: DESCRIPTIONS[run.fault], window_ms: run.window_ms },
    requirements: [
      'R1: lower-criticality software must not delay the brake or steering loops (judge by misses_beyond_platform_noise and iterations_off_pinned_cpu)',
      'R2: loss of wheel-speed data must be detected within one brake period and the brake must enter a safe state',
      `R3: a failed process must be restarted within the recovery budget`,
    ],
    recovery: run.recovery
      ? {
          process: run.recovery.process,
          cause: run.recovery.cause,
          detected_by: run.recovery.detection_path,
          fault_to_detect_ms: run.recovery.fault_to_detect_ms,
          detect_to_ready_ms: run.recovery.detect_to_ready_ms,
          fault_to_ready_ms: run.recovery.fault_to_ready_ms,
          within_budget: run.recovery.within_budget,
        }
      : 'no process restart was needed',
    brake_loop: {
      baseline_window: loop(run.brake.baseline),
      fault_window: loop(run.brake.fault),
      noise_envelope: { fault_free_windows_compared: run.brake_noise.windows, max_deadline_misses: run.brake_noise.max, mean_deadline_misses: run.brake_noise.mean },
      misses_beyond_platform_noise: run.brake_miss_excess,
      iterations_off_pinned_cpu: run.brake_off_cpu_iterations,
    },
    steering_loop: {
      baseline_window: loop(run.steer.baseline),
      fault_window: loop(run.steer.fault),
      noise_envelope: { fault_free_windows_compared: run.steer_noise.windows, max_deadline_misses: run.steer_noise.max, mean_deadline_misses: run.steer_noise.mean },
      misses_beyond_platform_noise: run.steer_miss_excess,
    },
    brake_safety: {
      sensor_link_lost_entries: run.safety.brake_link_lost_entries,
      stale_sample_entries_from_emulator_pauses: run.safety.brake_stale_entries,
      first_detection_after_fault_ms: run.safety.brake_first_detect_ms,
      safe_state_duration_ms: run.safety.brake_safe_duration_ms,
      reason: run.safety.brake_safe_reason,
    },
    instrument_cluster: {
      display_latency_max_baseline_us: run.cluster.baseline_display_latency_max_us,
      display_latency_max_fault_us: run.cluster.fault_display_latency_max_us,
      media_messages_handled: run.cluster.media_messages,
    },
    infotainment: {
      peak_allocation_mb: run.infotainment.peak_alloc_mb,
      flood_messages_delivered: run.infotainment.flood_messages,
    },
    cpu_peak_load_pct: run.cpu_peak_pct,
    timeline: run.events
      .filter((e) => !['spawn', 'stats_reset', 'info'].includes(e.kind) || e.kind === 'spawn')
      .slice(0, 24)
      .map((e) => ({ t_ms: Math.round(((e.ts - t0) / 1e6) * 1000) / 1000, source: e.src, event: e.kind, process: e.proc, detail: e.text })),
  };
}

const SYSTEM_PROMPT = `You are a functional safety engineer at an automotive supplier. You write short, precise incident reports for a mixed-criticality vehicle computer after fault-injection tests.
Rules:
- Use only facts and numbers that appear in the JSON log you are given. You may round a number to fewer decimals; keep its unit (us, ms, %, MB).
- Do not compute new numbers (no differences, ratios or percentages that are not in the log), and do not cite dates, clause numbers or any number that is not in the log.
- Write "us" for microseconds.
- Structure the report with exactly these markdown headings: "## Summary", "## What failed", "## What the guardian did", "## Timing impact on safety loops", "## Safety requirements". Under Safety requirements give one bullet per requirement (R1, R2, R3): "met", "not met" or "not applicable", with the evidence.
- Serious, factual tone. At most 230 words.`;

export async function generateReport(log: Record<string, unknown>): Promise<IncidentReport> {
  const t0 = Date.now();
  const messages: ChatMessage[] = [
    { role: 'system', content: SYSTEM_PROMPT },
    { role: 'user', content: `Incident log (JSON):\n${JSON.stringify(log, null, 1)}\n\nWrite the incident report.` },
  ];
  let attempts = 0;
  let model = '';
  try {
    let res = await chat(messages);
    attempts = 1;
    model = res.model;
    let v = verifyReport(res.content, log, [1, 2, 3]);
    if (!v.ok) {
      const bad = [...new Set(v.unverified.map((t) => t.raw.trim()))];
      messages.push({ role: 'assistant', content: res.content });
      messages.push({
        role: 'user',
        content: `These numbers in your report do not appear in the log: ${bad.join(', ')}. Rewrite the whole report using only numbers that appear in the log, with the same headings.`,
      });
      res = await chat(messages);
      attempts = 2;
      model = res.model;
      v = verifyReport(res.content, log, [1, 2, 3]);
    }
    return {
      status: v.ok ? 'verified' : 'flagged',
      model,
      attempts,
      text: res.content,
      flagged_text: v.ok ? undefined : flagUnverified(res.content, v.unverified),
      unverified: v.unverified.map((t) => t.raw.trim()),
      verified_count: v.verified.length,
      generated_ms: Date.now() - t0,
      log,
    };
  } catch (e) {
    return {
      status: 'error', model, attempts, text: '', unverified: [], verified_count: 0,
      generated_ms: Date.now() - t0, error: e instanceof Error ? e.message : String(e), log,
    };
  }
}
