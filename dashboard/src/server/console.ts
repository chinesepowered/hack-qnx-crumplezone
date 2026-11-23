// Crumple Zone: bridge to the QNX serial console (QEMU chardev on TCP).
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// QEMU exposes the target's serial port (where ksh and every process log
// line live) on 127.0.0.1:4555. The dashboard shows it in a terminal pane so
// the real QNX console is on screen next to the measurements.

import { EventEmitter } from 'node:events';
import net from 'node:net';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);

export class ConsoleBridge extends EventEmitter {
  private sock: net.Socket | null = null;
  private closed = false;
  private history: Buffer[] = [];
  private historyBytes = 0;

  constructor(private port = Number(process.env.CZ_CONSOLE_PORT ?? 4555)) {
    super();
  }

  /** Seed the scrollback with the tail of QEMU's console log (boot messages). */
  seed(file: string): void {
    try {
      const fs = require('node:fs') as typeof import('node:fs');
      if (!fs.existsSync(file)) return;
      const buf = fs.readFileSync(file);
      const tail = buf.subarray(Math.max(0, buf.length - 12 * 1024));
      const text = tail.toString('utf8').replace(/\r?\n/g, '\r\n');
      const b = Buffer.from(text, 'utf8');
      this.history = [b];
      this.historyBytes = b.length;
    } catch {
      /* no log, no seed */
    }
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
    const s = net.createConnection({ host: '127.0.0.1', port: this.port });
    this.sock = s;
    s.on('data', (d: Buffer) => {
      this.history.push(d);
      this.historyBytes += d.length;
      while (this.historyBytes > 64 * 1024 && this.history.length > 1) this.historyBytes -= this.history.shift()!.length;
      this.emit('data', d);
    });
    s.on('error', () => {});
    s.on('close', () => {
      if (!this.closed) setTimeout(() => this.connect(), 1000);
    });
  }

  backlog(): Buffer {
    return Buffer.concat(this.history);
  }

  write(data: string): void {
    this.sock?.write(data);
  }
}
