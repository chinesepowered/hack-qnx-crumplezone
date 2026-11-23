// Crumple Zone: dashboard server.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// Bridges the QNX target to the browser: telemetry (io-sock TCP, port 7300),
// the QNX serial console (QEMU chardev, port 4555), chaos runs and AI incident
// reports. Everything on screen comes from the target; the browser only draws.

import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer, type WebSocket } from 'ws';
import { TelemetryClient } from './telemetry.js';
import { ConsoleBridge } from './console.js';
import { loadEnv } from './llm.js';
import { buildIncidentLog, generateReport, type IncidentReport } from './report.js';
import { Recorder, runFault, type RunResult } from '../shared/runs.js';
import { FAULTS, type FaultName, type TelemetryMsg } from '../shared/types.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const repo = path.resolve(root, '..');
loadEnv(path.join(repo, '.env'));

const PORT = Number(process.env.CZ_DASHBOARD_PORT ?? 8080);
const webDir = path.join(root, 'dist', 'web');

export function platformInfo(): { accel: string; vcpus: number } {
  const f = path.join(repo, 'build', 'qemu-accel.txt');
  const accel = fs.existsSync(f) ? fs.readFileSync(f, 'utf8').trim() : fs.existsSync('/dev/kvm') ? 'KVM' : 'TCG';
  return { accel, vcpus: Number(process.env.CZ_SMP ?? 3) };
}

export class DashboardServer {
  rec = new Recorder();
  tele = new TelemetryClient();
  con = new ConsoleBridge();
  clients = new Set<WebSocket>();
  runs: RunResult[] = [];
  reports = new Map<string, IncidentReport>();
  busy: string | null = null;
  phase = '';
  server: http.Server;

  constructor() {
    this.server = http.createServer((req, res) => this.http(req, res));
    const wss = new WebSocketServer({ server: this.server, path: '/ws' });
    wss.on('connection', (ws) => this.onClient(ws));
    this.tele.on('msg', (m: TelemetryMsg) => {
      this.rec.ingest(m);
      this.broadcast(m);
    });
    this.tele.on('status', (ok: boolean) => this.broadcast({ t: 'link', ok }));
    this.con.on('data', (d: Buffer) => this.broadcast({ t: 'con', d: d.toString('base64') }));
  }

  start(port = PORT): Promise<void> {
    this.con.seed(path.join(repo, 'build', 'qemu-console.log'));
    this.tele.start();
    this.con.start();
    return new Promise((r) => this.server.listen(port, '127.0.0.1', () => r()));
  }

  stop(): void {
    this.tele.stop();
    this.con.stop();
    this.server.close();
    for (const c of this.clients) c.terminate();
  }

  private broadcast(obj: unknown): void {
    const s = JSON.stringify(obj);
    for (const c of this.clients) if (c.readyState === c.OPEN) c.send(s);
  }

  private onClient(ws: WebSocket): void {
    this.clients.add(ws);
    ws.on('close', () => this.clients.delete(ws));
    ws.on('message', (raw) => {
      try {
        const m = JSON.parse(String(raw)) as { cmd: string; fault?: string; run?: string; data?: string };
        if (m.cmd === 'fault' && m.fault) void this.chaos(m.fault as FaultName);
        else if (m.cmd === 'report' && m.run) void this.report(m.run);
        else if (m.cmd === 'console' && typeof m.data === 'string') this.con.write(m.data);
        else if (m.cmd === 'reset') this.tele.send('reset');
      } catch {
        /* ignore malformed */
      }
    });
    // initial snapshot: platform, recent history, runs, console backlog
    const r = this.rec;
    const recent = (id: 'brake' | 'steer', n: number) => r.samples[id].slice(-n).map((s) => [s.n, s.lat, s.exec, s.jit, s.flags, s.mode, s.io]);
    ws.send(JSON.stringify({
      t: 'snapshot',
      hello: r.hello,
      platform: platformInfo(),
      link: this.tele.connected,
      status: r.status,
      brake: recent('brake', 8000),
      steer: recent('steer', 4000),
      events: r.events.slice(-300),
      proc: r.procs.at(-1)?.msg ?? null,
      cluster: r.cluster.at(-1)?.msg ?? null,
      infot: r.infot.at(-1)?.msg ?? null,
      runs: this.runs,
      reports: Object.fromEntries(this.reports),
      busy: this.busy,
      phase: this.phase,
      console: this.con.backlog().toString('base64'),
    }));
  }

  async chaos(fault: FaultName): Promise<RunResult | null> {
    if (!FAULTS.includes(fault) || this.busy) return null;
    this.busy = fault;
    this.broadcast({ t: 'busy', fault, phase: 'start' });
    try {
      const run = await runFault(this.tele, this.rec, fault, {
        onPhase: (p) => {
          this.phase = p;
          this.broadcast({ t: 'busy', fault, phase: p });
        },
      });
      this.runs.push(run);
      this.broadcast({ t: 'run', run });
      return run;
    } finally {
      this.busy = null;
      this.phase = '';
      this.broadcast({ t: 'busy', fault: null, phase: '' });
    }
  }

  async report(runId: string): Promise<IncidentReport | null> {
    const run = this.runs.find((r) => r.id === runId);
    if (!run) return null;
    this.broadcast({ t: 'report_pending', run: runId });
    const log = buildIncidentLog(run, this.rec.hello, platformInfo() as never);
    const rep = await generateReport(log);
    this.reports.set(runId, rep);
    this.broadcast({ t: 'report', run: runId, report: rep });
    return rep;
  }

  private http(req: http.IncomingMessage, res: http.ServerResponse): void {
    const url = new URL(req.url ?? '/', 'http://x');
    const json = (code: number, body: unknown) => {
      res.writeHead(code, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(body));
    };
    if (url.pathname === '/api/state') return json(200, { link: this.tele.connected, busy: this.busy, runs: this.runs.length, guest_ns: this.rec.guestNow });
    if (url.pathname === '/api/runs') return json(200, this.runs);
    const fm = url.pathname.match(/^\/api\/fault\/(\w+)$/);
    if (fm && req.method === 'POST') {
      void this.chaos(fm[1] as FaultName).then((r) => json(r ? 200 : 409, r ?? { error: 'busy or unknown fault' }));
      return;
    }
    const rm = url.pathname.match(/^\/api\/report\/([\w-]+)$/);
    if (rm && req.method === 'POST') {
      void this.report(rm[1]).then((r) => json(r ? 200 : 404, r ?? { error: 'no such run' }));
      return;
    }
    // static files
    let file = path.normalize(path.join(webDir, url.pathname === '/' ? 'index.html' : url.pathname));
    if (!file.startsWith(webDir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) file = path.join(webDir, 'index.html');
    const types: Record<string, string> = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.map': 'application/json' };
    res.writeHead(200, { 'Content-Type': types[path.extname(file)] ?? 'application/octet-stream', 'Cache-Control': 'no-store' });
    fs.createReadStream(file).pipe(res);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const s = new DashboardServer();
  void s.start().then(() => console.log(`Crumple Zone dashboard on http://127.0.0.1:${PORT}`));
  process.on('SIGINT', () => {
    s.stop();
    process.exit(0);
  });
}
