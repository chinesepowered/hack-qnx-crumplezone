// Crumple Zone: verify every number in an LLM-written incident report against
// the structured event log it was given.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
//
// The log uses unit-suffixed keys (_ns, _us, _ms, _s, _pct, _mb, _kb, _hz,
// _bar, _kmh); a value can be quoted in any compatible unit and with any
// rounding the text itself shows. A number with an explicit unit only matches
// values of that dimension. Markdown list markers and heading numbers are not
// claims and are skipped.

export type Unit = 'ns' | 'us' | 'ms' | 's' | 'pct' | 'mb' | 'kb' | 'gb' | 'hz' | 'khz' | 'bar' | 'kmh' | '';

export interface NumberToken {
  raw: string;
  value: number;
  unit: Unit;
  decimals: number;
  index: number;
}

export interface Candidate {
  value: number;
  unit: Unit;
  path: string;
}

export interface VerifyResult {
  ok: boolean;
  tokens: NumberToken[];
  verified: { token: NumberToken; match: Candidate }[];
  unverified: NumberToken[];
}

const UNIT_ALIASES: [RegExp, Unit][] = [
  [/^(?:µs|μs|us|usec|microseconds?)$/i, 'us'],
  [/^(?:ns|nanoseconds?)$/i, 'ns'],
  [/^(?:ms|msec|milliseconds?)$/i, 'ms'],
  [/^(?:s|sec|secs|seconds?)$/i, 's'],
  [/^(?:%|percent)$/i, 'pct'],
  [/^(?:mb|mib|megabytes?)$/i, 'mb'],
  [/^(?:kb|kib|kilobytes?)$/i, 'kb'],
  [/^(?:gb|gib)$/i, 'gb'],
  [/^(?:khz)$/i, 'khz'],
  [/^(?:hz)$/i, 'hz'],
  [/^(?:bar)$/i, 'bar'],
  [/^(?:km\/h|kmh|kph)$/i, 'kmh'],
];

function normUnit(u: string | undefined): Unit {
  if (!u) return '';
  const t = u.trim();
  for (const [re, unit] of UNIT_ALIASES) if (re.test(t)) return unit;
  return '';
}

// number, optional unit; thousands separators allowed; not part of a word
const NUM_RE =
  /(?<![\w.\-/])(\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?)(?:\s?(µs|μs|us|microseconds?|nanoseconds?|milliseconds?|ns|ms|seconds?|secs?|s|%|percent|MiB|MB|KiB|kB|KB|GB|kHz|Hz|bar|km\/h))?(?![\w])/g;

/** Extract the numeric claims from report text. */
export function extractNumbers(text: string): NumberToken[] {
  const out: NumberToken[] = [];
  // Remove markdown list markers ("1. ", "2) ") and heading numbers before scanning.
  const masked = text
    .split('\n')
    .map((line) => line.replace(/^(\s*(?:#{1,6}\s*)?)(\d{1,2})([.)])(\s)/, (_m, a, n, b, c) => a + ' '.repeat(n.length) + ' '.repeat(b.length) + c))
    .join('\n');
  let m: RegExpExecArray | null;
  NUM_RE.lastIndex = 0;
  while ((m = NUM_RE.exec(masked)) !== null) {
    const raw = m[1];
    const clean = raw.replace(/,/g, '');
    const value = Number(clean);
    if (!Number.isFinite(value)) continue;
    const dot = clean.indexOf('.');
    out.push({
      raw: m[0],
      value,
      unit: normUnit(m[2]),
      decimals: dot >= 0 ? clean.length - dot - 1 : 0,
      index: m.index,
    });
  }
  return out;
}

function keyUnit(key: string): Unit {
  const k = key.toLowerCase();
  if (k.endsWith('_ns')) return 'ns';
  if (k.endsWith('_us')) return 'us';
  if (k.endsWith('_ms')) return 'ms';
  if (k.endsWith('_s') || k.endsWith('_sec')) return 's';
  if (k.endsWith('_pct')) return 'pct';
  if (k.endsWith('_mb')) return 'mb';
  if (k.endsWith('_kb')) return 'kb';
  if (k.endsWith('_hz')) return 'hz';
  if (k.endsWith('_bar')) return 'bar';
  if (k.endsWith('_kmh')) return 'kmh';
  return '';
}

/** All representations of a logged value in every compatible unit. */
function expand(value: number, unit: Unit, path: string): Candidate[] {
  const c = (v: number, u: Unit): Candidate => ({ value: v, unit: u, path });
  switch (unit) {
    case 'ns': return [c(value, 'ns'), c(value / 1e3, 'us'), c(value / 1e6, 'ms'), c(value / 1e9, 's')];
    case 'us': return [c(value * 1e3, 'ns'), c(value, 'us'), c(value / 1e3, 'ms'), c(value / 1e6, 's')];
    case 'ms': return [c(value * 1e6, 'ns'), c(value * 1e3, 'us'), c(value, 'ms'), c(value / 1e3, 's')];
    case 's': return [c(value * 1e3, 'ms'), c(value, 's')];
    case 'kb': return [c(value, 'kb'), c(value / 1024, 'mb'), c(value / 1000, 'mb')];
    case 'mb': return [c(value, 'mb'), c(value * 1024, 'kb'), c(value / 1024, 'gb')];
    case 'hz': return [c(value, 'hz'), c(value / 1000, 'khz')];
    default: return [c(value, unit)];
  }
}

/** Flatten every number in the log, including numbers inside strings. */
export function logCandidates(log: unknown): Candidate[] {
  const out: Candidate[] = [];
  const walk = (v: unknown, path: string, key: string): void => {
    if (typeof v === 'number' && Number.isFinite(v)) {
      out.push(...expand(v, keyUnit(key), path));
    } else if (typeof v === 'string') {
      for (const t of extractNumbers(v)) out.push(...expand(t.value, t.unit, path));
    } else if (Array.isArray(v)) {
      v.forEach((x, i) => walk(x, `${path}[${i}]`, key));
    } else if (v && typeof v === 'object') {
      for (const [k, x] of Object.entries(v as Record<string, unknown>)) walk(x, path ? `${path}.${k}` : k, k);
    }
  };
  walk(log, '', '');
  return out;
}

function sameDimension(a: Unit, b: Unit): boolean {
  const time = ['ns', 'us', 'ms', 's'];
  const mem = ['kb', 'mb', 'gb'];
  const freq = ['hz', 'khz'];
  if (a === b) return true;
  return (time.includes(a) && time.includes(b)) || (mem.includes(a) && mem.includes(b)) || (freq.includes(a) && freq.includes(b));
}

function matches(t: NumberToken, c: Candidate): boolean {
  if (t.unit) {
    // explicit unit: candidate must be expressed in exactly that unit (after conversion)
    if (c.unit !== t.unit) return false;
  } else if (c.unit && !['', 'pct', 'kmh', 'bar', 'hz'].includes(c.unit) && t.decimals === 0 && t.value < 10) {
    // a bare single digit is too easy to hit by accident through unit conversions
    return false;
  }
  const tol = 0.5 * 10 ** -t.decimals + 1e-9 * Math.max(1, Math.abs(c.value));
  return Math.abs(t.value - c.value) <= tol;
}

export function verifyReport(text: string, log: unknown, extraAllowed: number[] = []): VerifyResult {
  const cands = logCandidates(log).concat(extraAllowed.map((v) => ({ value: v, unit: '' as Unit, path: 'allowed' })));
  const tokens = extractNumbers(text);
  const verified: VerifyResult['verified'] = [];
  const unverified: NumberToken[] = [];
  for (const t of tokens) {
    const exact = cands.find((c) => matches(t, c) && (t.unit === '' || sameDimension(t.unit, c.unit)));
    if (exact) verified.push({ token: t, match: exact });
    else unverified.push(t);
  }
  return { ok: unverified.length === 0, tokens, verified, unverified };
}

/** Wrap unverified numbers so the UI can flag them. */
export function flagUnverified(text: string, unverified: NumberToken[]): string {
  let out = '';
  let pos = 0;
  for (const t of [...unverified].sort((a, b) => a.index - b.index)) {
    out += text.slice(pos, t.index) + `[[UNVERIFIED:${t.raw}]]`;
    pos = t.index + t.raw.length;
  }
  return out + text.slice(pos);
}
