// Crumple Zone: record and render demo/video.mp4 from a real run.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// 1. Starts the dashboard in-process, opens it in Chromium (1920x1080) and
//    captures every painted frame with the DevTools screencast while a
//    scripted chaos run plays on the live QNX target.
// 2. Writes the narration from the numbers that run actually produced, voices
//    it with ElevenLabs (voice Jessica, eleven_multilingual_v2; every request
//    is counted in build/tts-ledger.json, budget 6,000 characters).
// 3. Renders H.264 1920x1080 with ffmpeg, then captures stills and the
//    thumbnail at device scale factor 2.

import { execFile } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { promisify } from 'node:util';
import { DashboardServer } from '../server/index.js';
import { loadEnv } from '../server/llm.js';
import { sleep, type RunResult } from '../shared/runs.js';
import type { FaultName } from '../shared/types.js';
import { log, repo } from './qnx.js';

const pexec = promisify(execFile);
const require = createRequire(import.meta.url);
// eslint-disable-next-line @typescript-eslint/no-explicit-any
const { chromium } = require('playwright') as any;

loadEnv(path.join(repo, '.env'));
const work = path.join(repo, 'demo', 'work');
const framesDir = path.join(work, 'frames');
const audioDir = path.join(work, 'audio');
const ledgerFile = path.join(repo, 'build', 'tts-ledger.json');
const TTS_BUDGET = 6000;
const VIDEO_MAX_S = 164;

interface Clip { id: string; at: number; text: string; file?: string; dur?: number; start?: number }

// ---------------------------------------------------------------- TTS
function ledger(): { used: number; requests: { id: string; chars: number; when: string }[] } {
  if (!fs.existsSync(ledgerFile)) return { used: 0, requests: [] };
  return JSON.parse(fs.readFileSync(ledgerFile, 'utf8'));
}

async function tts(id: string, text: string): Promise<string> {
  const voice = process.env.ELEVENLABS_VOICE_ID ?? 'cgSgspJ2msm6clMCkdW9';
  const model = process.env.ELEVENLABS_MODEL_ID ?? 'eleven_multilingual_v2';
  if (voice !== 'cgSgspJ2msm6clMCkdW9') throw new Error('narration must use ElevenLabs voice Jessica (cgSgspJ2msm6clMCkdW9)');
  const hash = crypto.createHash('sha1').update(`${voice}|${model}|${text}`).digest('hex').slice(0, 12);
  const file = path.join(audioDir, `${id}-${hash}.mp3`);
  if (fs.existsSync(file)) return file; // never pay twice for the same line
  const l = ledger();
  if (l.used + text.length > TTS_BUDGET) throw new Error(`ElevenLabs budget: ${l.used} used, ${text.length} more would exceed ${TTS_BUDGET}`);
  const key = process.env.ELEVENLABS_API_KEY;
  if (!key) throw new Error('ELEVENLABS_API_KEY not set');
  const res = await fetch(`https://api.elevenlabs.io/v1/text-to-speech/${voice}?output_format=mp3_44100_128`, {
    method: 'POST',
    headers: { 'xi-api-key': key, 'Content-Type': 'application/json', Accept: 'audio/mpeg' },
    body: JSON.stringify({ text, model_id: model, voice_settings: { stability: 0.5, similarity_boost: 0.75, style: 0.15, use_speaker_boost: true } }),
  });
  if (!res.ok) throw new Error(`ElevenLabs HTTP ${res.status}: ${(await res.text()).slice(0, 300)}`);
  fs.mkdirSync(audioDir, { recursive: true });
  fs.writeFileSync(file, Buffer.from(await res.arrayBuffer()));
  l.used += text.length;
  l.requests.push({ id, chars: text.length, when: new Date().toISOString() });
  fs.mkdirSync(path.dirname(ledgerFile), { recursive: true });
  fs.writeFileSync(ledgerFile, JSON.stringify(l, null, 1));
  return file;
}

async function duration(file: string): Promise<number> {
  const { stdout } = await pexec('ffprobe', ['-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', file]);
  return Number(stdout.trim());
}

// ---------------------------------------------------------------- recording
const TIMELINE: { at: number; act: string; arg?: string }[] = [
  { at: 0.3, act: 'callout', arg: 'Crumple Zone|A crash in infotainment cannot touch the brakes. Live on QNX OS 8.0.' },
  { at: 16.0, act: 'fault', arg: 'crash' },
  { at: 30.0, act: 'type', arg: 'czctl status\r' },
  { at: 49.0, act: 'fault', arg: 'storm' },
  { at: 64.0, act: 'fault', arg: 'leak' },
  { at: 81.0, act: 'fault', arg: 'flood' },
  { at: 96.0, act: 'type', arg: 'cat /dev/vehicle/wheel_speed\r' },
  { at: 99.0, act: 'fault', arg: 'kill_sensor' },
  { at: 117.0, act: 'report' },
  { at: 137.0, act: 'closereport' },
  { at: 137.2, act: 'final' },
];
const RECORD_S = 158;

async function record(server: DashboardServer): Promise<{ runs: Record<string, RunResult>; frames: { file: string; t: number }[]; reportCount: number; ui: Record<string, number> }> {
  fs.rmSync(framesDir, { recursive: true, force: true });
  fs.mkdirSync(framesDir, { recursive: true });
  const browser = await chromium.launch({ args: ['--disable-gpu-vsync', '--force-device-scale-factor=1'] });
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
  await page.goto('http://127.0.0.1:8080/');
  await sleep(9000);
  const cdp = await page.context().newCDPSession(page);
  const frames: { file: string; t: number }[] = [];
  let t0 = 0;
  // Video time at which the page itself first showed each event. Narration is
  // anchored to these, so it stays in sync even if the page lags the target.
  const ui: Record<string, number> = {};
  const poll = setInterval(async () => {
    if (!t0) return;
    try {
      const seen = await page.evaluate(() => ({
        faults: window.cz.state.events.filter((e) => e.kind === 'fault_inject').map((e) => e.text),
        report: !document.getElementById('report-panel')!.hidden,
      }));
      const now = Date.now() / 1000 - t0;
      for (const f of seen.faults) if (!(f in ui) && pendingFaults.has(f)) { ui[f] = now; pendingFaults.delete(f); }
      if (seen.report && !('report' in ui)) ui.report = now;
    } catch { /* page busy */ }
  }, 150);
  const pendingFaults = new Set<string>();
  cdp.on('Page.screencastFrame', async (f: { data: string; sessionId: number; metadata: { timestamp: number } }) => {
    const t = f.metadata.timestamp;
    if (!t0) t0 = t;
    const file = path.join(framesDir, `f${String(frames.length).padStart(6, '0')}.jpg`);
    fs.writeFileSync(file, Buffer.from(f.data, 'base64'));
    frames.push({ file, t: t - t0 });
    await cdp.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
  });
  await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 88, maxWidth: 1920, maxHeight: 1080, everyNthFrame: 1 });
  const start = Date.now();
  const runs: Record<string, RunResult> = {};
  let reportCount = 0;
  const pending: Promise<unknown>[] = [];
  for (const step of TIMELINE) {
    const wait = start + step.at * 1000 - Date.now();
    if (wait > 0) await sleep(wait);
    log(`t=${((Date.now() - start) / 1000).toFixed(1)} ${step.act} ${step.arg ?? ''}`);
    if (step.act === 'callout') {
      const [a, b] = step.arg!.split('|');
      await page.evaluate(([x, y]: string[]) => window.cz.callout(x, y, 12000), [a, b]);
    } else if (step.act === 'type') {
      await page.evaluate((s: string) => window.cz.typeConsole(s), step.arg);
    } else if (step.act === 'fault') {
      const f = step.arg as FaultName;
      pendingFaults.add(f);
      pending.push(server.chaos(f).then(async (r) => {
        if (!r) { log(`fault ${f} was refused`); return; }
        runs[f] = r;
        const text = calloutFor(r);
        await page.evaluate(([x, y]: string[]) => window.cz.callout(x, y, 7000), text);
      }));
    } else if (step.act === 'report') {
      const last = Object.values(runs).at(-1);
      if (last) pending.push(server.report(last.id).then((rep) => { reportCount = rep?.verified_count ?? 0; }));
    } else if (step.act === 'closereport') {
      await page.evaluate(() => window.cz.closeReport());
    } else if (step.act === 'final') {
      ui.final = Date.now() / 1000 - t0;
      const c = runs.crash?.recovery, k = runs.kill_sensor?.recovery;
      const sub = `infotainment back in ${fmtMs(c?.fault_to_ready_ms ?? c?.detect_to_ready_ms)} ms | sensor driver restarted in ${fmtMs(k?.detect_to_ready_ms)} ms | brake never left its core`;
      await page.evaluate(([x, y]: string[]) => window.cz.callout(x, y, 30000), ['Isolation, priorities, message passing, a guardian', sub]);
    }
  }
  const tail = start + RECORD_S * 1000 - Date.now();
  if (tail > 0) await sleep(tail);
  await Promise.all(pending);
  clearInterval(poll);
  await cdp.send('Page.stopScreencast');
  await browser.close();
  return { runs, frames, reportCount, ui };
}

function fmtMs(v: number | null | undefined): string {
  return v === null || v === undefined ? '-' : v.toFixed(1);
}

function calloutFor(r: RunResult): [string, string] {
  const b = r.brake.fault;
  const brake = `brake: ${b.iterations} periods run, sensor link losses ${r.safety.brake_link_lost_entries}, never off CPU 2`;
  switch (r.fault) {
    case 'crash': return [`Infotainment back in ${fmtMs(r.recovery?.fault_to_ready_ms)} ms`, `SIGSEGV detected via ${r.recovery?.detection_path}; ${brake}`];
    case 'storm': return ['CPU storm: every core at 100%', `CPU peak ${r.cpu_peak_pct.join(' / ')} %; ${brake}`];
    case 'leak': return [`Leak contained at ${r.infotainment.peak_alloc_mb} MB, restart ${fmtMs(r.recovery?.detect_to_ready_ms)} ms`, `RLIMIT_AS 192 MB, ${r.recovery?.cause}; ${brake}`];
    case 'flood': return [`${r.infotainment.flood_messages.toLocaleString('en-US')} flood messages absorbed`, `cluster display latency max ${r.cluster.fault_display_latency_max_us} us; ${brake}`];
    case 'kill_sensor': return [`Sensor loss caught ${fmtMs(r.safety.brake_first_detect_ms)} ms after the kill`, `driver restarted in ${fmtMs(r.recovery?.detect_to_ready_ms)} ms; safe state held ${fmtMs(r.safety.brake_safe_duration_ms)} ms, then normal control`];
  }
}

// ---------------------------------------------------------------- narration
function say(v: number | null | undefined, d = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'a few';
  return v.toFixed(d).replace(/\.0$/, '');
}

function narration(runs: Record<string, RunResult>, reportCount: number, ui: Record<string, number>): Clip[] {
  const crash = runs.crash?.recovery, leak = runs.leak?.recovery, kill = runs.kill_sensor;
  const u = (k: string, plan: number, lead: number) => (ui[k] !== undefined ? ui[k] - lead : plan);
  return [
    { id: 'c1', at: 0.6, text: 'Carmakers are moving infotainment, the instrument cluster and brake control onto one computer. A crashing media app must never delay a brake command. This is Crumple Zone, live on QNX OS 8.0. Watch the brake trace as we crash the infotainment system.' },
    { id: 'c2', at: u('crash', 19.0, -2.5), text: `The brake loop never blinked. The guardian saw the segmentation fault, restarted infotainment, and had it back in ${say(crash?.fault_to_ready_ms)} milliseconds.` },
    { id: 'c3', at: 28.0, text: 'Every number on screen is measured inside QNX. The brake loop runs at one kilohertz, FIFO priority sixty, pinned to its own core, and reads wheel speed from a resource manager through time-bounded message passing. Infotainment runs at priority eight, as an unprivileged user, with a memory limit.' },
    { id: 'c4', at: u('storm', 48.0, 1.0), text: 'Now a CPU storm. Infotainment spins a thread on every core, including the brake core, and all three CPUs hit one hundred percent. The brake loop simply preempts it, every period.' },
    { id: 'c5', at: u('leak', 63.0, 1.0), text: `Next, a memory leak. Infotainment allocates until the limit the guardian set. Malloc fails, it aborts alone, and it is back in ${say(leak?.detect_to_ready_ms)} milliseconds.` },
    { id: 'c6', at: u('flood', 80.0, 1.0), text: 'A message flood. Four threads hammer the instrument cluster. QNX queues messages by priority, so the flood is served at infotainment priority, and the cluster display stays fresh.' },
    { id: 'c7', at: u('kill_sensor', 96.0, 2.6), text: `Now the hard case: we kill the wheel speed sensor driver. The brake loop notices on its next read, ${say(kill?.safety.brake_first_detect_ms)} milliseconds after the kill, and holds its last command. The guardian restarts the driver in ${say(kill?.recovery?.detect_to_ready_ms)} milliseconds, and normal control resumes.` },
    { id: 'c8', at: u('report', 116.0, 0.8), text: `After each run, Qwen on Weights and Biases Inference writes an incident report from the structured event log. Code checks every number against the log before it is shown${reportCount ? `, and all ${reportCount} numbers here were verified` : ''}.` },
    { id: 'c9', at: u('final', 136.0, -0.3), text: 'This is emulated hardware on a shared cloud host, and the dashboard shows that noise honestly; the kernel traces in the repository separate it from QNX scheduling. Process isolation, priorities, message passing and a guardian. The crash stays in the crumple zone.' },
  ];
}

// ---------------------------------------------------------------- render
async function render(frames: { file: string; t: number }[], clips: Clip[], out: string): Promise<number> {
  const lastT = frames.at(-1)!.t;
  const videoLen = Math.min(VIDEO_MAX_S, Math.max(lastT, ...clips.map((c) => (c.start ?? c.at) + (c.dur ?? 0) + 1.5)));
  const list = path.join(work, 'frames.txt');
  const lines: string[] = [];
  for (let i = 0; i < frames.length; i++) {
    const d = (i + 1 < frames.length ? frames[i + 1].t : videoLen) - frames[i].t;
    lines.push(`file '${frames[i].file}'`, `duration ${Math.max(0.001, d).toFixed(4)}`);
  }
  lines.push(`file '${frames.at(-1)!.file}'`);
  fs.writeFileSync(list, lines.join('\n'));
  const silent = path.join(work, 'video-silent.mp4');
  await pexec('ffmpeg', ['-y', '-v', 'error', '-f', 'concat', '-safe', '0', '-i', list, '-vf', 'scale=1920:1080:flags=lanczos,format=yuv420p',
    '-fps_mode', 'cfr', '-r', '30', '-t', videoLen.toFixed(2), '-c:v', 'libx264', '-preset', 'slow', '-crf', '23', '-movflags', '+faststart', silent], { maxBuffer: 1 << 26 });
  const args = ['-y', '-v', 'error', '-i', silent];
  const filters: string[] = [];
  clips.forEach((c, i) => {
    args.push('-i', c.file!);
    const ms = Math.round((c.start ?? c.at) * 1000);
    filters.push(`[${i + 1}:a]adelay=${ms}|${ms},aresample=48000[a${i}]`);
  });
  filters.push(`${clips.map((_, i) => `[a${i}]`).join('')}amix=inputs=${clips.length}:normalize=0:duration=longest,loudnorm=I=-16:TP=-1.5:LRA=11,apad[aout]`);
  args.push('-filter_complex', filters.join(';'), '-map', '0:v', '-map', '[aout]', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k', '-ar', '48000',
    '-t', videoLen.toFixed(2), '-movflags', '+faststart', out);
  await pexec('ffmpeg', args, { maxBuffer: 1 << 26 });
  return videoLen;
}

// ---------------------------------------------------------------- stills at 2x
async function stills(server: DashboardServer): Promise<void> {
  const dir = path.join(repo, 'docs', 'screenshots');
  fs.mkdirSync(dir, { recursive: true });
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 2 });
  await page.goto('http://127.0.0.1:8080/');
  await sleep(8000);
  await page.screenshot({ path: path.join(dir, '1-overview.png') });
  const shot = async (f: FaultName, name: string, at: number) => {
    const p = server.chaos(f);
    await sleep(at);
    await page.screenshot({ path: path.join(dir, name) });
    await p;
    await sleep(5000);
  };
  await shot('storm', '2-cpu-storm.png', 3500);
  await shot('leak', '3-memory-leak.png', 1800);
  const kill = server.chaos('kill_sensor');
  await kill;
  await sleep(600);
  await page.screenshot({ path: path.join(dir, '4-sensor-driver-killed.png') });
  const crash = await server.chaos('crash');
  await sleep(1500);
  await page.screenshot({ path: path.join(dir, '5-crash-recovered.png') });
  if (crash) {
    const rep = server.report(crash.id);
    await rep;
    await sleep(1500);
    await page.screenshot({ path: path.join(dir, '6-incident-report.png') });
  }
  await browser.close();
}

async function thumbnail(): Promise<void> {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
  await page.goto('http://127.0.0.1:8080/');
  await sleep(7000);
  await page.evaluate(() => {
    const el = document.createElement('div');
    el.innerHTML = '<div style="font:800 92px Inter,sans-serif;letter-spacing:.06em;color:#fff">CRUMPLE ZONE</div>' +
      '<div style="font:600 40px Inter,sans-serif;color:#38bdf8;margin-top:18px">Crash the infotainment. The brakes never notice.</div>' +
      '<div style="font:500 28px DejaVu Sans Mono,monospace;color:#c4d0dd;margin-top:26px">QNX OS 8.0 | 1 kHz brake-by-wire | guardian restarts in milliseconds</div>';
    el.style.cssText = 'position:fixed;inset:0;display:flex;flex-direction:column;justify-content:center;padding:0 110px;background:linear-gradient(90deg,#0a0e13f2 0%,#0a0e13d9 55%,#0a0e1366 100%);z-index:50';
    document.body.appendChild(el);
  });
  await sleep(300);
  await page.screenshot({ path: path.join(repo, 'demo', 'thumb.png') });
  await browser.close();
}

async function main(): Promise<void> {
  const mode = process.argv[2] ?? 'all';
  fs.mkdirSync(work, { recursive: true });
  const server = new DashboardServer();
  await server.start();
  await server.tele.waitConnected(30000);
  await sleep(4000);
  try {
    if (mode === 'all' || mode === 'video') {
      log('recording');
      const { runs, frames, reportCount, ui } = await record(server);
      fs.writeFileSync(path.join(work, 'runs.json'), JSON.stringify({ runs, reportCount, ui, frames: frames.length }, null, 1));
      log(`page showed events at (video s): ${JSON.stringify(Object.fromEntries(Object.entries(ui).map(([k, v]) => [k, Math.round(v * 10) / 10])))}`);
      log(`${frames.length} frames; runs: ${Object.keys(runs).join(', ')}; report numbers verified: ${reportCount}`);
      const missing = ['crash', 'storm', 'leak', 'flood', 'kill_sensor'].filter((f) => !runs[f]?.ok);
      if (missing.length) throw new Error(`runs missing or failed: ${missing.join(', ')}; not spending narration`);
      const clips = narration(runs, reportCount, ui);
      const total = clips.reduce((n, c) => n + c.text.length, 0);
      log(`narration: ${total} characters, ElevenLabs used so far ${ledger().used}`);
      let prevEnd = 0;
      for (const c of clips) {
        c.file = await tts(c.id, c.text);
        c.dur = await duration(c.file);
        c.start = Math.max(c.at, prevEnd + 0.35);
        prevEnd = c.start + c.dur;
        log(`  ${c.id} at ${c.start.toFixed(1)} s for ${c.dur.toFixed(1)} s`);
      }
      const out = path.join(repo, 'demo', 'video.mp4');
      const len = await render(frames, clips, out);
      fs.writeFileSync(path.join(repo, 'demo', 'narration.md'), '# Narration\n\nVoice: ElevenLabs Jessica (cgSgspJ2msm6clMCkdW9), model eleven_multilingual_v2.\n\n' +
        clips.map((c) => `- ${(c.start ?? c.at).toFixed(1)} s: ${c.text}`).join('\n') + '\n');
      fs.writeFileSync(path.join(repo, 'demo', 'video-run.json'), JSON.stringify({ recorded: new Date().toISOString(), runs, reportCount }, null, 1));
      log(`rendered demo/video.mp4 (${len.toFixed(1)} s); ElevenLabs characters used in total: ${ledger().used}`);
    }
    if (mode === 'all' || mode === 'stills') {
      log('stills');
      await stills(server);
      await thumbnail();
    }
  } finally {
    server.stop();
  }
  process.exit(0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
