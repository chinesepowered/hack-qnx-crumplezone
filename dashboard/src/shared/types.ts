// Crumple Zone: telemetry message types (mirror of qnx/src/telemetry.c).
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

export type LoopId = 'brake' | 'steer';

/** [iteration, latency ns, exec ns, jitter ns, flags, mode, io ns] */
export type Sample = [number, number, number, number, number, number, number];

export const SF_MISS = 0x1;
export const SF_STALE = 0x2;
export const SF_SAFE = 0x4;
export const SF_OVERRUN = 0x8;

export const MODES = ['NORMAL', 'SAFE_HOLD', 'SAFE_STOP', 'RESUMING'] as const;

export interface LoopStatus {
  start_ns: number;
  period_ns: number;
  cpu: number;
  prio: number;
  mode: number;
  iter: number;
  miss_total: number;
  miss_win: number;
  overruns: number;
  lat_last: number;
  lat_max_win: number;
  lat_max_total: number;
  jit_max_win: number;
  exec_max_win: number;
  resp_max_win: number;
  lat_mean_win: number;
  win_n: number;
  stale_total: number;
  off_cpu: number;
  safe_entries: number;
  reset_gen: number;
  speed_kmh: number;
  true_kmh: number;
  pedal: number;
  p: [number, number, number, number];
  slip: [number, number, number, number];
  abs: number;
  mu: number;
  torque: number;
  assist: number;
  angle: number;
}

export interface HelloMsg {
  t: 'hello';
  v: number;
  system: string;
  release: string;
  version: string;
  machine: string;
  node: string;
  ncpu: number;
  cps: number;
  boot_ns: number;
  now_ns: number;
  brake_period_ns: number;
  steer_period_ns: number;
}

export interface LoopMsg {
  t: 'loop';
  id: LoopId;
  ts: number;
  st: LoopStatus;
  s: Sample[];
}

export interface EventMsg {
  t: 'ev';
  ts: number;
  src: string;
  kind: string;
  proc: string;
  pid: number;
  a: number;
  v0: number;
  v1: number;
  text: string;
}

export interface ProcInfo {
  name: string;
  pid: number;
  state: string;
  prio: number;
  pol: string;
  crit: number;
  mask: number;
  uid: number;
  aslimit: number;
  restarts: number;
  sig: number;
  rec_ns: number;
  cpu: number;
  mem_kb: number;
  thr: number;
}

export interface ProcMsg {
  t: 'proc';
  ts: number;
  freemem: number;
  cpus: number[];
  p: ProcInfo[];
}

export interface ClusterMsg {
  t: 'cluster';
  frames: number;
  pulses: number;
  media: number;
  lat_us: number;
  lat_max_us: number;
  fjit_max_us: number;
  speed: number;
  warn: number;
}

export interface InfotMsg {
  t: 'infot';
  mode: string;
  alloc_mb: number;
  flood_sent: number;
  tracks: number;
  hb_age_ms: number;
}

export interface AckMsg {
  t: 'ack';
  cmd: string;
  ok: boolean;
  err?: string;
  fault?: string;
  t_fault?: number;
  ts?: number;
}

export type TelemetryMsg = HelloMsg | LoopMsg | EventMsg | ProcMsg | ClusterMsg | InfotMsg | AckMsg;

export const FAULTS = ['crash', 'storm', 'leak', 'flood', 'kill_sensor'] as const;
export type FaultName = (typeof FAULTS)[number];

export const FAULT_LABELS: Record<FaultName, string> = {
  crash: 'Crash infotainment',
  storm: 'CPU storm',
  leak: 'Memory leak',
  flood: 'Message flood',
  kill_sensor: 'Kill sensor driver',
};

/** Release time (guest ns) of iteration n of a loop. */
export function releaseNs(st: Pick<LoopStatus, 'start_ns' | 'period_ns'>, n: number): number {
  return st.start_ns + n * st.period_ns;
}
