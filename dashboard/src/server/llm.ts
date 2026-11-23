// Crumple Zone: OpenAI-compatible chat client (W&B Inference) with fallback.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

import fs from 'node:fs';
import path from 'node:path';

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant';
  content: string;
}

export interface ChatResult {
  model: string;
  content: string;
  ms: number;
}

/** Load KEY=VALUE lines from the repository's .env into process.env (no override). */
export function loadEnv(file = path.resolve(process.cwd(), '..', '.env')): void {
  for (const f of [file, path.resolve(process.cwd(), '.env')]) {
    if (!fs.existsSync(f)) continue;
    for (const line of fs.readFileSync(f, 'utf8').split('\n')) {
      const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/);
      if (m && process.env[m[1]] === undefined) process.env[m[1]] = m[2];
    }
  }
}

/** Remove any chain-of-thought the model emits inline. */
export function stripReasoning(text: string): string {
  return text.replace(/<think>[\s\S]*?<\/think>/gi, '').replace(/^\s+/, '');
}

export async function chat(messages: ChatMessage[], opts: { maxTokens?: number; temperature?: number } = {}): Promise<ChatResult> {
  const base = process.env.OPENAI_BASE_URL;
  const key = process.env.OPENAI_API_KEY;
  if (!base || !key) throw new Error('OPENAI_BASE_URL / OPENAI_API_KEY not set (see .env.example)');
  const models = [process.env.OPENAI_MODEL ?? 'Qwen/Qwen3.8-27B', process.env.OPENAI_FALLBACK_MODEL].filter(Boolean) as string[];
  let lastErr: unknown = null;
  for (const model of models) {
    const t0 = Date.now();
    try {
      const res = await fetch(`${base.replace(/\/$/, '')}/chat/completions`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${key}` },
        body: JSON.stringify({ model, messages, max_tokens: opts.maxTokens ?? 2500, temperature: opts.temperature ?? 0.2 }),
        signal: AbortSignal.timeout(180000),
      });
      if (!res.ok) throw new Error(`${model}: HTTP ${res.status} ${(await res.text()).slice(0, 200)}`);
      const data = (await res.json()) as { choices?: { message?: { content?: string } }[] };
      const content = stripReasoning(data.choices?.[0]?.message?.content ?? '');
      if (!content.trim()) throw new Error(`${model}: empty response`);
      return { model, content, ms: Date.now() - t0 };
    } catch (e) {
      lastErr = e;
    }
  }
  throw lastErr instanceof Error ? lastErr : new Error(String(lastErr));
}
