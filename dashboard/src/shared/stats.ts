// Crumple Zone: summary statistics for timing samples.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only

export interface Summary {
  n: number;
  mean: number;
  p50: number;
  p99: number;
  p999: number;
  max: number;
}

/** Nearest-rank percentile of an ascending array, p in [0, 100]. */
export function percentile(sorted: ArrayLike<number>, p: number): number {
  const n = sorted.length;
  if (n === 0) return 0;
  if (p <= 0) return sorted[0];
  if (p >= 100) return sorted[n - 1];
  const rank = Math.ceil((p / 100) * n);
  return sorted[Math.min(n, Math.max(1, rank)) - 1];
}

export function summarize(values: number[]): Summary {
  const n = values.length;
  if (n === 0) return { n: 0, mean: 0, p50: 0, p99: 0, p999: 0, max: 0 };
  const s = Float64Array.from(values).sort();
  let sum = 0;
  for (let i = 0; i < n; i++) sum += s[i];
  return {
    n,
    mean: sum / n,
    p50: percentile(s, 50),
    p99: percentile(s, 99),
    p999: percentile(s, 99.9),
    max: s[n - 1],
  };
}

/**
 * Upper bound of a Poisson count at roughly 99.9% confidence, used to decide
 * whether a fault window saw more deadline misses than the baseline rate can
 * explain. Normal approximation with continuity, floored for small means.
 */
export function poissonUpper999(mean: number): number {
  if (mean <= 0) return 3; // P(X > 3 | mean ~0.3) < 0.001
  return Math.ceil(mean + 3.1 * Math.sqrt(mean) + 1.5);
}

export function round(v: number, digits = 1): number {
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}
