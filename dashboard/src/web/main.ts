// Crumple Zone dashboard front-end. Draws what the QNX target measured; it
// never simulates or smooths a number.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import { FAULT_LABELS, MODES, SF_MISS, SF_OVERRUN, type ClusterMsg, type EventMsg, type HelloMsg, type InfotMsg, type LoopId, type LoopStatus, type ProcMsg, type Sample } from '../shared/types.js';
import type { RunResult } from '../shared/runs.js';
import type { IncidentReport } from '../server/report.js';

interface Pt { t: number; lat: number; flags: number; mode: number; io: number }

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const WINDOW_NS = 10e9;

const state = {
  hello: null as HelloMsg | null,
  platform: { accel: 'TCG', vcpus: 3 },
  link: false,
  status: {} as Partial<Record<LoopId, LoopStatus>>,
  pts: { brake: [] as Pt[], steer: [] as Pt[] },
  lastN: { brake: -1, steer: -1 },
  events: [] as EventMsg[],
  proc: null as ProcMsg | null,
  cluster: null as ClusterMsg | null,
  infot: null as InfotMsg | null,
  runs: [] as RunResult[],
  reports: {} as Record<string, IncidentReport>,
  busy: null as string | null,
  guestNow: 0,
  markers: [] as { t: number; label: string; color: string; kind: string }[],
  lastEnter: {} as Record<string, number>,
  restartFlash: new Map<string, number>(),
};

// ---------------- websocket ----------------
let ws: WebSocket;
function connect(): void {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = (ev) => onMsg(JSON.parse(ev.data as string));
  ws.onclose = () => setTimeout(connect, 1000);
}
function send(obj: unknown): void {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function addSamples(id: LoopId, st: LoopStatus | undefined, s: Sample[]): void {
  if (!st) return;
  const arr = state.pts[id];
  for (const [n, lat, , , flags, mode, io] of s) {
    if (n <= state.lastN[id] && arr.length) continue;
    state.lastN[id] = n;
    arr.push({ t: st.start_ns + n * st.period_ns, lat, flags, mode, io });
  }
  const cutoff = (arr.at(-1)?.t ?? 0) - 12e9;
  let k = 0;
  while (k < arr.length && arr[k].t < cutoff) k++;
  if (k > 2000) arr.splice(0, k);
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function onMsg(m: any): void {
  switch (m.t) {
    case 'snapshot':
      state.hello = m.hello;
      state.platform = m.platform;
      state.link = m.link;
      state.status = m.status ?? {};
      addSamples('brake', m.status?.brake, m.brake ?? []);
      addSamples('steer', m.status?.steer, m.steer ?? []);
      state.events = m.events ?? [];
      state.proc = m.proc;
      state.cluster = m.cluster;
      state.infot = m.infot;
      state.runs = m.runs ?? [];
      state.reports = m.reports ?? {};
      state.busy = m.busy;
      for (const e of state.events) markerFor(e);
      if (m.console) term.write(Uint8Array.from(atob(m.console), (c) => c.charCodeAt(0)));
      renderEvents();
      renderRun();
      renderBusy(m.phase);
      break;
    case 'hello':
      state.hello = m;
      break;
    case 'link':
      state.link = m.ok;
      break;
    case 'loop':
      state.status[m.id as LoopId] = m.st;
      state.guestNow = Math.max(state.guestNow, m.ts);
      addSamples(m.id, m.st, m.s);
      break;
    case 'ev':
      state.events.push(m);
      if (state.events.length > 2000) state.events.splice(0, 500);
      markerFor(m);
      if (m.kind === 'restart' || m.kind === 'death') state.restartFlash.set(m.proc, Date.now());
      renderEvents();
      break;
    case 'proc':
      state.proc = m;
      break;
    case 'cluster':
      state.cluster = m;
      break;
    case 'infot':
      state.infot = m;
      break;
    case 'busy':
      state.busy = m.fault;
      renderBusy(m.phase);
      break;
    case 'run':
      state.runs.push(m.run);
      renderRun();
      break;
    case 'report_pending':
      showReport(m.run, null);
      break;
    case 'report':
      state.reports[m.run] = m.report;
      showReport(m.run, m.report);
      break;
    case 'con':
      term.write(Uint8Array.from(atob(m.d), (c) => c.charCodeAt(0)));
      break;
    default:
      break;
  }
}

function markerFor(e: EventMsg): void {
  if (e.kind === 'fault_inject') state.markers.push({ t: e.ts, label: (FAULT_LABELS as Record<string, string>)[e.text] ?? e.text.toUpperCase(), color: '#a78bfa', kind: 'fault' });
  else if (e.kind === 'restart') state.markers.push({ t: e.ts, label: `${e.proc} back in ${fmtMs(e.v0 >= 0 && e.a !== 3 ? e.v0 : e.v1)}`, color: '#22c55e', kind: 'restart' });
  else if (e.kind === 'death') state.markers.push({ t: e.ts, label: '', color: '#ef4444', kind: 'death' });
  if (state.markers.length > 60) state.markers.splice(0, 20);
}

// ---------------- formatting ----------------
function fmtUs(ns: number): string {
  if (!Number.isFinite(ns)) return '-';
  if (ns >= 1e6) return `${(ns / 1e6).toFixed(2)} ms`;
  return `${Math.round(ns / 1e3)} us`;
}
function fmtMs(ns: number): string {
  return `${(ns / 1e6).toFixed(1)} ms`;
}
function pct(sorted: number[], p: number): number {
  if (!sorted.length) return NaN;
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1))];
}

// ---------------- charts ----------------
const Y_MIN = 20e3, Y_MAX = 20e6; // 20 us .. 20 ms, log scale
function yOf(ns: number, h: number, top: number, bottom: number): number {
  const v = Math.min(Math.max(ns, Y_MIN), Y_MAX);
  const f = (Math.log10(v) - Math.log10(Y_MIN)) / (Math.log10(Y_MAX) - Math.log10(Y_MIN));
  return h - bottom - f * (h - top - bottom);
}

function drawChart(canvas: HTMLCanvasElement, id: LoopId): void {
  const dpr = window.devicePixelRatio || 1;
  const cw = canvas.clientWidth, ch = canvas.clientHeight;
  if (canvas.width !== Math.round(cw * dpr)) { canvas.width = Math.round(cw * dpr); canvas.height = Math.round(ch * dpr); }
  const g = canvas.getContext('2d')!;
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, cw, ch);
  const left = 54, right = 10, top = 8, bottom = 18;
  const w = cw - left - right;
  const arr = state.pts[id];
  const st = state.status[id];
  const now = arr.at(-1)?.t ?? 0;
  const deadline = st?.period_ns ?? 1e6;

  // grid
  g.font = '11px DejaVu Sans Mono, monospace';
  g.textAlign = 'right';
  g.textBaseline = 'middle';
  for (const [v, label] of [[50e3, '50 us'], [100e3, '100 us'], [300e3, '300 us'], [1e6, '1 ms'], [3e6, '3 ms'], [10e6, '10 ms']] as [number, string][]) {
    const y = yOf(v, ch, top, bottom);
    g.strokeStyle = '#17212c';
    g.lineWidth = 1;
    g.beginPath(); g.moveTo(left, y); g.lineTo(cw - right, y); g.stroke();
    g.fillStyle = '#566476';
    g.fillText(label, left - 6, y);
  }
  // deadline line
  const yd = yOf(deadline, ch, top, bottom);
  g.strokeStyle = '#ef444488';
  g.setLineDash([5, 4]);
  g.beginPath(); g.moveTo(left, yd); g.lineTo(cw - right, yd); g.stroke();
  g.setLineDash([]);
  g.fillStyle = '#ef4444aa';
  g.textAlign = 'left';
  g.fillText(`deadline ${deadline / 1e6} ms`, left + 6, yd - 8);

  if (arr.length) {
    // envelope: per pixel column, max and median latency
    const cols = Math.max(1, Math.floor(w));
    const maxv = new Float64Array(cols), minv = new Float64Array(cols).fill(Infinity);
    const miss = new Uint8Array(cols), safe = new Uint8Array(cols);
    for (let i = arr.length - 1; i >= 0; i--) {
      const p = arr[i];
      const age = now - p.t;
      if (age > WINDOW_NS) break;
      const c = Math.min(cols - 1, Math.floor(((WINDOW_NS - age) / WINDOW_NS) * cols));
      if (p.lat > maxv[c]) maxv[c] = p.lat;
      if (p.lat < minv[c]) minv[c] = p.lat;
      if (p.flags & (SF_MISS | SF_OVERRUN)) miss[c] = 1;
      if (p.mode !== 0) safe[c] = 1;
    }
    for (let c = 0; c < cols; c++) {
      if (safe[c]) {
        g.fillStyle = '#f5a52422';
        g.fillRect(left + c, top, 1, ch - top - bottom);
      }
      if (maxv[c] === 0) continue;
      const y1 = yOf(maxv[c], ch, top, bottom), y0 = yOf(minv[c], ch, top, bottom);
      g.fillStyle = id === 'brake' ? '#38bdf8' : '#5eead4';
      g.globalAlpha = 0.85;
      g.fillRect(left + c, y1, 1, Math.max(1, y0 - y1 + 1));
      g.globalAlpha = 1;
      if (miss[c]) {
        g.fillStyle = '#ef4444';
        g.fillRect(left + c, top, 1, 4);
      }
    }
    // fault / recovery markers: fault labels along the top, recovery labels along the bottom
    g.textAlign = 'left';
    let lastTop = -1e9, lastBottom = -1e9;
    for (const mk of state.markers) {
      const age = now - mk.t;
      if (age < 0 || age > WINDOW_NS) continue;
      const x = left + ((WINDOW_NS - age) / WINDOW_NS) * w;
      g.strokeStyle = mk.color;
      g.lineWidth = 1.5;
      g.beginPath(); g.moveTo(x, top); g.lineTo(x, ch - bottom); g.stroke();
      if (id !== 'brake' || mk.kind === 'death') continue;
      g.font = '600 12px Inter, sans-serif';
      const tw = g.measureText(mk.label).width + 10;
      const atTop = mk.kind === 'fault';
      if (atTop ? x < lastTop : x < lastBottom) continue;
      const y = atTop ? top + 16 : ch - bottom - 10;
      g.fillStyle = '#0b1118dd';
      g.fillRect(x + 3, y - 10, tw, 15);
      g.fillStyle = mk.color;
      g.fillText(mk.label, x + 7, y + 1);
      if (atTop) lastTop = x + tw + 6; else lastBottom = x + tw + 6;
      g.font = '11px DejaVu Sans Mono, monospace';
    }
  }
  // time axis
  g.fillStyle = '#566476';
  g.textAlign = 'center';
  g.textBaseline = 'alphabetic';
  for (let s = 0; s <= 10; s += 2) {
    const x = left + (s / 10) * w;
    g.fillText(s === 10 ? 'now' : `-${10 - s} s`, x, ch - 4);
  }
}

// ---------------- panels ----------------
function loopStats(id: LoopId): void {
  const arr = state.pts[id];
  const now = arr.at(-1)?.t ?? 0;
  const w = arr.filter((p) => now - p.t <= WINDOW_NS);
  const lat = w.map((p) => p.lat).sort((a, b) => a - b);
  const io = w.map((p) => p.io).sort((a, b) => a - b);
  let misses = 0;
  for (let i = 0; i < w.length; i++) if (w[i].flags & (SF_MISS | SF_OVERRUN)) misses++;
  $(`${id}-p50`).textContent = fmtUs(pct(lat, 50));
  $(`${id}-p99`).textContent = fmtUs(pct(lat, 99));
  $(`${id}-max`).textContent = fmtUs(lat.at(-1) ?? NaN);
  if (id === 'brake') $('brake-io').textContent = fmtUs(pct(io, 99));
  $(`${id}-miss`).textContent = `${misses} / ${w.length}`;
  const st = state.status[id];
  const mode = MODES[st?.mode ?? 0] ?? 'NORMAL';
  const el = $(`${id}-mode`);
  el.textContent = mode;
  el.classList.toggle('safe', mode !== 'NORMAL');
  if (id === 'steer' && st) $('steer-assist').textContent = `${st.assist.toFixed(2)} Nm`;
}

function renderVehicle(): void {
  const b = state.status.brake;
  if (!b) return;
  $('speed').textContent = Math.round(b.true_kmh).toString();
  $('pedal').textContent = `${Math.round(b.pedal * 100)}%`;
  $('pedal-bar').style.width = `${Math.min(100, b.pedal * 100)}%`;
  document.querySelectorAll<HTMLElement>('.wheel').forEach((el) => {
    const i = Number(el.dataset.w);
    (el.querySelector('.bar i') as HTMLElement).style.width = `${Math.min(100, (b.p[i] / 160) * 100)}%`;
    (el.querySelector('span') as HTMLElement).textContent = `${b.p[i].toFixed(0)} bar`;
    el.classList.toggle('abs', (b.abs & (1 << i)) !== 0);
  });
  $('road').textContent = b.mu < 0.6 ? `wet patch, mu ${b.mu.toFixed(2)}` : `dry road, mu ${b.mu.toFixed(2)}`;
  if (state.cluster) {
    $('cluster-lat').textContent = `${state.cluster.lat_us} us`;
    $('cluster-media').textContent = state.cluster.media.toLocaleString('en-US');
  }
}

const PROC_ORDER = ['brake_ctl', 'steer_ctl', 'wheel_sensors', 'vehicle_sim', 'guardian', 'cluster', 'telemetry', 'infotainment'];
function renderProcs(): void {
  const p = state.proc;
  if (!p) return;
  const rows = PROC_ORDER.map((n) => p.p.find((x) => x.name === n)).filter(Boolean) as ProcMsg['p'];
  const tb = $('proc-rows');
  tb.innerHTML = rows.map((r) => {
    const cpu = r.mask ? String(Math.log2(r.mask)) : 'any';
    const mem = r.mem_kb >= 10240 ? `${(r.mem_kb / 1024).toFixed(0)} MB` : `${r.mem_kb} KB`;
    const limit = r.aslimit ? ` / ${(r.aslimit / 1048576).toFixed(0)}` : '';
    const flash = (state.restartFlash.get(r.name) ?? 0) > Date.now() - 1500 ? ' flash' : '';
    const crit = r.crit ? '<span class="crit">safety</span>' : '';
    return `<tr class="${r.state}${flash}"><td><span class="dot"></span>${r.name}${crit}</td><td>${r.pid}</td><td>${r.prio}${r.pol}</td><td>${cpu}</td><td>${r.cpu.toFixed(1)}</td><td>${mem}${limit && r.mem_kb >= 10240 ? limit + ' MB' : ''}</td><td>${r.restarts}</td></tr>`;
  }).join('');
  $('cores').innerHTML = p.cpus.map((c, i) => `<div class="core">CPU ${i} ${c.toFixed(0)}%<div class="bar"><i style="width:${Math.min(100, c)}%"></i></div></div>`).join('');
}

function eventText(e: EventMsg): { txt: string; cls: string } | null {
  const ms = (v: number) => `${(v / 1e6).toFixed(2)} ms`;
  switch (e.kind) {
    case 'boot': return { txt: `system up: ${e.a} processes in ${ms(e.v0)}`, cls: 'good' };
    case 'fault_inject': return { txt: `fault injected: ${(FAULT_LABELS as Record<string, string>)[e.text] ?? e.text} (pid ${e.pid})`, cls: 'bad' };
    case 'fault_end': return { txt: `${e.text} ended after ${ms(e.v0)}`, cls: '' };
    case 'death': return { txt: `${e.proc} (pid ${e.pid}) died; detected via ${e.text}${e.v0 >= 0 ? `, ${ms(e.v0)} after the fault` : ''}`, cls: 'bad' };
    case 'exit_status': return { txt: `${e.proc} exit status: ${e.text}`, cls: 'bad' };
    case 'spawn': return { txt: `spawned ${e.proc} (pid ${e.pid}, priority ${e.a})`, cls: '' };
    case 'restart': return { txt: `${e.proc} restored: ${e.v0 >= 0 ? `${ms(e.v0)} fault to ready, ` : ''}${ms(e.v1)} restart (${e.text})`, cls: 'good' };
    case 'safe_enter': return e.a === 3 ? null : { txt: `${e.src} SAFE STATE: sensor link lost (${e.a === 2 ? 'timeout' : 'read error'}), holding last command`, cls: 'bad' };
    case 'safe_exit': return state.lastEnter[e.src] === 3 ? null : { txt: `${e.src} normal control resumed after ${ms(e.v0)}`, cls: 'good' };
    case 'info': return e.text === 'SAFE_STOP' ? { txt: `${e.src} SAFE_STOP: controlled stop pressure`, cls: 'bad' } : null;
    default: return null;
  }
}

function renderEvents(): void {
  const ol = $('events');
  const rows: string[] = [];
  const evs = state.events;
  // remember the reason of each source's latest safe-state entry, in time order
  const reasonAt = new Map<EventMsg, number>();
  const cur: Record<string, number> = {};
  for (const e of evs) {
    if (e.kind === 'safe_enter') cur[e.src] = e.a;
    if (e.kind === 'safe_exit') reasonAt.set(e, cur[e.src] ?? 0);
  }
  for (let i = evs.length - 1; i >= 0 && rows.length < 26; i--) {
    const e = evs[i];
    if (e.kind === 'safe_exit') state.lastEnter[e.src] = reasonAt.get(e) ?? 0;
    const r = eventText(e);
    if (!r) continue;
    rows.push(`<li class="${r.cls}"><time>${(e.ts / 1e9).toFixed(3)}</time><span class="src ${e.src}">${e.src}</span><span class="txt">${r.txt}</span></li>`);
  }
  ol.innerHTML = rows.join('');
}

function renderBusy(phase?: string): void {
  document.querySelectorAll<HTMLButtonElement>('.chaos button[data-fault]').forEach((b) => {
    b.disabled = !!state.busy;
    b.classList.toggle('active', b.dataset.fault === state.busy);
  });
  $('chaos-phase').textContent = state.busy ? `${(FAULT_LABELS as Record<string, string>)[state.busy]}: ${phase ?? ''}` : 'faults go into the non-critical side only';
}

function renderRun(): void {
  const r = state.runs.at(-1);
  const btn = $<HTMLButtonElement>('btn-report');
  btn.disabled = !r;
  if (!r) return;
  const el = $('run-result');
  if (!r.ok) {
    el.innerHTML = `<span class="muted">${r.label}: ${r.error}</span>`;
    return;
  }
  const rec = r.recovery;
  const big = rec ? `${(rec.fault_to_ready_ms ?? rec.detect_to_ready_ms).toFixed(1)} ms` : 'no restart needed';
  const bf = r.brake.fault, bb = r.brake.baseline;
  el.innerHTML = `
    <div class="kv"><label>${r.label}</label><span class="big">${big}</span></div>
    <div class="kv"><label>${rec ? (rec.fault_to_ready_ms !== null ? 'fault to ready' : 'death to ready') : 'recovery'}</label><b>${rec ? rec.cause : '-'}</b></div>
    <div class="kv"><label>brake misses, fault window</label><b>${bf.deadline_misses} / ${bf.iterations}</b></div>
    <div class="kv"><label>baseline, same length</label><b>${bb.deadline_misses} / ${bb.iterations}</b></div>
    <div class="kv"><label>brake p99 latency, fault / baseline</label><b>${bf.latency_us.p99} / ${bb.latency_us.p99} us</b></div>
    <div class="kv"><label>verdict</label><b style="color:${r.brake_miss_excess ? '#ef4444' : '#22c55e'}">${r.brake_miss_excess ? 'brake affected' : 'brake unaffected'}</b></div>`;
}

// ---------------- incident report ----------------
function renderMarkdown(md: string): string {
  const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  let html = '';
  let inList = false;
  for (const raw of md.split('\n')) {
    const line = raw.trimEnd();
    const inline = (s: string) => esc(s)
      .replace(/\[\[UNVERIFIED:(.+?)\]\]/g, '<span class="unv" title="not found in the event log">$1</span>')
      .replace(/\*\*(.+?)\*\*/g, '<b>$1</b>')
      .replace(/(?<![\w.])(\d+(?:\.\d+)?)(\s?(?:us|ms|%|MB|Hz|kHz))?(?![\w])/g, '<span class="num">$1$2</span>');
    if (/^#{1,6}\s/.test(line)) {
      if (inList) { html += '</ul>'; inList = false; }
      html += `<h3>${inline(line.replace(/^#{1,6}\s+/, ''))}</h3>`;
    } else if (/^\s*[-*]\s+/.test(line)) {
      if (!inList) { html += '<ul>'; inList = true; }
      html += `<li>${inline(line.replace(/^\s*[-*]\s+/, ''))}</li>`;
    } else if (line.trim()) {
      if (inList) { html += '</ul>'; inList = false; }
      html += `<p>${inline(line)}</p>`;
    }
  }
  if (inList) html += '</ul>';
  return html;
}

function showReport(runId: string, rep: IncidentReport | null): void {
  const panel = $('report-panel');
  panel.hidden = false;
  const run = state.runs.find((r) => r.id === runId);
  const badge = $('report-badge');
  if (!rep) {
    badge.textContent = 'generating';
    badge.className = 'badge';
    $('report-meta').textContent = `${run?.label ?? runId}: structured event log sent to ${'W&B Inference'}`;
    $('report-body').innerHTML = '<p class="muted">Writing the report. Every number will be checked against the log before it is shown.</p>';
    return;
  }
  badge.className = `badge ${rep.status}`;
  badge.textContent = rep.status === 'verified' ? `all ${rep.verified_count} numbers verified against the log`
    : rep.status === 'flagged' ? `${rep.unverified.length} number(s) not in the log, flagged` : 'error';
  $('report-meta').textContent = `${run?.label ?? runId} | model ${rep.model} | attempts ${rep.attempts} | ${(rep.generated_ms / 1000).toFixed(1)} s`;
  $('report-body').innerHTML = rep.status === 'error' ? `<p>${rep.error}</p>` : renderMarkdown(rep.flagged_text ?? rep.text);
}

// ---------------- console ----------------
const term = new Terminal({
  fontFamily: 'DejaVu Sans Mono, monospace', fontSize: 12.5, lineHeight: 1.12, convertEol: false,
  theme: { background: '#05080b', foreground: '#c9d4df', cursor: '#38bdf8' }, scrollback: 2000,
});
const fit = new FitAddon();

// ---------------- automation hooks (used by the recorder) ----------------
declare global {
  interface Window {
    cz: {
      fault(name: string): void;
      report(runId?: string): void;
      closeReport(): void;
      lastRun(): RunResult | undefined;
      busy(): string | null;
      typeConsole(text: string): void;
      callout(text: string, sub?: string, ms?: number): void;
      state: typeof state;
    };
  }
}
let calloutTimer = 0;
window.cz = {
  fault: (name) => send({ cmd: 'fault', fault: name }),
  report: (runId) => { const id = runId ?? state.runs.at(-1)?.id; if (id) send({ cmd: 'report', run: id }); },
  closeReport: () => { $('report-panel').hidden = true; },
  lastRun: () => state.runs.at(-1),
  busy: () => state.busy,
  typeConsole: (text) => send({ cmd: 'console', data: text }),
  callout: (text, sub, ms = 5000) => {
    const el = $('callout');
    el.innerHTML = `${text}${sub ? `<small>${sub}</small>` : ''}`;
    el.hidden = false;
    clearTimeout(calloutTimer);
    calloutTimer = window.setTimeout(() => { el.hidden = true; }, ms);
  },
  state,
};

// ---------------- boot ----------------
function boot(): void {
  term.loadAddon(fit);
  term.open($('term'));
  fit.fit();
  window.addEventListener('resize', () => fit.fit());
  term.onData((d) => send({ cmd: 'console', data: d }));
  document.querySelectorAll<HTMLButtonElement>('.chaos button[data-fault]').forEach((b) => {
    b.addEventListener('click', () => send({ cmd: 'fault', fault: b.dataset.fault }));
  });
  $('btn-report').addEventListener('click', () => window.cz.report());
  $('report-close').addEventListener('click', () => window.cz.closeReport());
  connect();

  let last = 0;
  const frame = (ts: number) => {
    if (ts - last > 33) {
      last = ts;
      drawChart($<HTMLCanvasElement>('chart-brake'), 'brake');
      drawChart($<HTMLCanvasElement>('chart-steer'), 'steer');
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  setInterval(() => {
    loopStats('brake');
    loopStats('steer');
    renderVehicle();
    renderProcs();
    const h = state.hello;
    $('chip-os').textContent = h ? `${h.system} ${h.release} ${h.machine}` : 'QNX';
    $('chip-vm').textContent = `QEMU ${state.platform.accel} | ${state.platform.vcpus} vCPU`;
    const up = state.guestNow / 1e9;
    $('chip-up').textContent = `uptime ${Math.floor(up / 60)}:${String(Math.floor(up % 60)).padStart(2, '0')}`;
    const link = $('chip-link');
    link.classList.toggle('on', state.link);
    link.querySelector('b')!.textContent = state.link ? 'telemetry live' : 'no telemetry';
  }, 200);
}
boot();
