/*
 * Crumple Zone: timing statistics (portable C, unit tested on the host).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cz_stats_t keeps count, min, max, mean and variance (Welford) plus a
 * fixed-resolution histogram for percentiles. Values are nanoseconds.
 * Nothing allocates, so it is safe to use inside a hard real-time loop.
 */
#ifndef CZ_STATS_H
#define CZ_STATS_H

#include <stdint.h>

#define CZ_HIST_BIN_NS   1000u   /* 1 us bins */
#define CZ_HIST_BINS     5000u   /* 0 .. 5 ms, plus one overflow bin */

typedef struct {
    uint64_t n;
    int64_t  min;
    int64_t  max;
    double   mean;
    double   m2;
    uint32_t hist[CZ_HIST_BINS + 1];
} cz_stats_t;

void     cz_stats_reset(cz_stats_t *s);
void     cz_stats_add(cz_stats_t *s, int64_t v);
double   cz_stats_mean(const cz_stats_t *s);
double   cz_stats_stddev(const cz_stats_t *s);
/* p in [0,100]. Returns the upper edge (ns) of the histogram bin that holds the
 * p-th percentile, clamped to the observed max. Returns 0 when empty. */
int64_t  cz_stats_percentile(const cz_stats_t *s, double p);

/* Deadline bookkeeping for a periodic task. */
typedef struct {
    uint64_t period_ns;
    uint64_t start_ns;    /* release time of iteration 0 */
} cz_period_t;

/* Index of the release that a wake-up at t_ns belongs to (nearest release at or before t). */
uint64_t cz_release_index(const cz_period_t *p, uint64_t t_ns);
uint64_t cz_release_time(const cz_period_t *p, uint64_t index);
/* A deadline is missed when the work for a release completes after the next release. */
int      cz_deadline_missed(const cz_period_t *p, uint64_t index, uint64_t done_ns);

#endif
