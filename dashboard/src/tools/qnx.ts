// Crumple Zone: helpers for driving the QNX target from the host (ssh, scp,
// traceprinter) and a small telemetry session used by the test runners.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

import { execFile, spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { TelemetryClient } from '../server/telemetry.js';
import { Recorder, sleep } from '../shared/runs.js';
import type { TelemetryMsg } from '../shared/types.js';

const pexec = promisify(execFile);
export const here = path.dirname(fileURLToPath(import.meta.url));
export const repo = path.resolve(here, '..', '..', '..');

const SSH_PORT = process.env.CZ_SSH_PORT ?? '2222';
const sshOpts = ['-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=15'];

export async function ssh(cmd: string, timeoutMs = 60000): Promise<string> {
  const { stdout } = await pexec('ssh', [...sshOpts, '-p', SSH_PORT, 'root@127.0.0.1', cmd], { timeout: timeoutMs, maxBuffer: 64 << 20 });
  return stdout;
}

/** Start a command on the target without waiting for it (returns when ssh exits). */
export function sshBackground(cmd: string): Promise<number> {
  return new Promise((resolve) => {
    const p = spawn('ssh', [...sshOpts, '-p', SSH_PORT, 'root@127.0.0.1', cmd], { stdio: 'ignore' });
    p.on('exit', (code) => resolve(code ?? 0));
  });
}

export async function scpFrom(remote: string, local: string): Promise<void> {
  await pexec('scp', [...sshOpts, '-P', SSH_PORT, `root@127.0.0.1:${remote}`, local], { timeout: 120000 });
}

export function qnxHostTool(name: string): string {
  const host = process.env.QNX_HOST ?? path.join(process.env.HOME ?? '/root', 'qnx800', 'host', 'linux', 'x86_64');
  return path.join(host, 'usr', 'bin', name);
}

export async function traceprinter(kev: string, out: string): Promise<void> {
  const env = { ...process.env, QNX_HOST: process.env.QNX_HOST ?? path.join(process.env.HOME ?? '/root', 'qnx800', 'host', 'linux', 'x86_64') };
  await new Promise<void>((resolve, reject) => {
    const o = fs.openSync(out, 'w');
    const p = spawn(qnxHostTool('traceprinter'), ['-f', kev], { stdio: ['ignore', o, o], env });
    p.on('exit', (code) => { fs.closeSync(o); code === 0 ? resolve() : reject(new Error(`traceprinter exit ${code}`)); });
  });
}

export function accelerator(): string {
  const f = path.join(repo, 'build', 'qemu-accel.txt');
  return fs.existsSync(f) ? fs.readFileSync(f, 'utf8').trim() : 'TCG';
}

/** Connect to telemetry and keep a recorder fed. */
export async function session(): Promise<{ tele: TelemetryClient; rec: Recorder; close: () => void }> {
  const tele = new TelemetryClient();
  const rec = new Recorder();
  tele.on('msg', (m: TelemetryMsg) => rec.ingest(m));
  tele.start();
  await tele.waitConnected(30000);
  for (let i = 0; i < 100 && !rec.status.brake; i++) await sleep(100);
  if (!rec.status.brake) throw new Error('no telemetry from the target');
  return { tele, rec, close: () => tele.stop() };
}

export function log(...a: unknown[]): void {
  console.log(`[${new Date().toISOString().slice(11, 19)}]`, ...a);
}
