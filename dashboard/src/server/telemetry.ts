// Crumple Zone: TCP client for the telemetry process running on QNX.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

import { EventEmitter } from 'node:events';
import net from 'node:net';
import type { AckMsg, TelemetryMsg } from '../shared/types.js';
import type { Link } from '../shared/runs.js';

export class TelemetryClient extends EventEmitter implements Link {
  private sock: net.Socket | null = null;
  private buf = '';
  private closed = false;
  connected = false;

  constructor(private host = '127.0.0.1', private port = Number(process.env.CZ_TELEMETRY_PORT ?? 7300)) {
    super();
    this.setMaxListeners(50);
  }

  start(): void {
    this.closed = false;
    this.connect();
  }

  stop(): void {
    this.closed = true;
    this.sock?.destroy();
  }

  private connect(): void {
    const s = net.createConnection({ host: this.host, port: this.port });
    this.sock = s;
    s.setNoDelay(true);
    s.on('connect', () => {
      this.connected = true;
      this.emit('status', true);
    });
    s.on('data', (d) => this.onData(d.toString('utf8')));
    const retry = () => {
      if (this.connected) this.emit('status', false);
      this.connected = false;
      if (!this.closed) setTimeout(() => this.connect(), 1000);
    };
    s.on('error', () => {});
    s.on('close', retry);
  }

  private onData(chunk: string): void {
    this.buf += chunk;
    let nl: number;
    while ((nl = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, nl);
      this.buf = this.buf.slice(nl + 1);
      if (!line) continue;
      try {
        const m = JSON.parse(line) as TelemetryMsg;
        this.emit('msg', m);
      } catch {
        // ignore a torn line
      }
    }
  }

  send(line: string): void {
    this.sock?.write(line.endsWith('\n') ? line : line + '\n');
  }

  nextAck(cmd: string, timeoutMs: number): Promise<AckMsg> {
    return new Promise((resolve, reject) => {
      const onMsg = (m: TelemetryMsg) => {
        if (m.t === 'ack' && m.cmd === cmd) {
          clearTimeout(timer);
          this.off('msg', onMsg);
          resolve(m);
        }
      };
      const timer = setTimeout(() => {
        this.off('msg', onMsg);
        reject(new Error(`no ${cmd} ack within ${timeoutMs} ms`));
      }, timeoutMs);
      this.on('msg', onMsg);
    });
  }

  waitConnected(timeoutMs: number): Promise<void> {
    if (this.connected) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error('telemetry not reachable')), timeoutMs);
      this.once('status', (ok: boolean) => {
        if (ok) {
          clearTimeout(t);
          resolve();
        }
      });
    });
  }
}
