/*
 * Crumple Zone: timing statistics.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "stats.h"

#include <math.h>
#include <string.h>

void cz_stats_reset(cz_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min = INT64_MAX;
    s->max = INT64_MIN;
}

void cz_stats_add(cz_stats_t *s, int64_t v)
{
    s->n++;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    double d = (double)v - s->mean;
    s->mean += d / (double)s->n;
    s->m2 += d * ((double)v - s->mean);

    uint64_t bin;
    if (v < 0) {
        bin = 0;
    } else {
        bin = (uint64_t)v / CZ_HIST_BIN_NS;
        if (bin > CZ_HIST_BINS) bin = CZ_HIST_BINS;
    }
    s->hist[bin]++;
}

double cz_stats_mean(const cz_stats_t *s)
{
    return s->n ? s->mean : 0.0;
}

double cz_stats_stddev(const cz_stats_t *s)
{
    if (s->n < 2) return 0.0;
    return sqrt(s->m2 / (double)(s->n - 1));
}

int64_t cz_stats_percentile(const cz_stats_t *s, double p)
{
    if (s->n == 0) return 0;
    if (p <= 0.0) return s->min;
    if (p >= 100.0) return s->max;
    /* rank of the sample we need, 1-based */
    uint64_t rank = (uint64_t)ceil(p / 100.0 * (double)s->n);
    if (rank < 1) rank = 1;
    uint64_t acc = 0;
    for (uint32_t i = 0; i <= CZ_HIST_BINS; i++) {
        acc += s->hist[i];
        if (acc >= rank) {
            if (i == CZ_HIST_BINS) return s->max;
            int64_t edge = (int64_t)(i + 1) * CZ_HIST_BIN_NS;
            return edge < s->max ? edge : s->max;
        }
    }
    return s->max;
}

uint64_t cz_release_index(const cz_period_t *p, uint64_t t_ns)
{
    if (t_ns <= p->start_ns) return 0;
    return (t_ns - p->start_ns) / p->period_ns;
}

uint64_t cz_release_time(const cz_period_t *p, uint64_t index)
{
    return p->start_ns + index * p->period_ns;
}

int cz_deadline_missed(const cz_period_t *p, uint64_t index, uint64_t done_ns)
{
    return done_ns > cz_release_time(p, index) + p->period_ns;
}
