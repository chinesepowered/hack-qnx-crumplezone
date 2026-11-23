/*
 * Crumple Zone: shared-memory bus access (seqlocks, sample and event rings).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Every region has exactly one writer, so writers never take a lock and never
 * block. Readers retry on a torn read (seqlock). This keeps telemetry and the
 * cluster from ever being able to delay a control loop.
 */
#ifndef CZ_BUS_H
#define CZ_BUS_H

#include "cz.h"

struct cz_bus *cz_bus_create(void);           /* guardian only */
struct cz_bus *cz_bus_open(int writable);
struct cz_hw  *cz_hw_create(void);            /* vehicle_sim only */
struct cz_hw  *cz_hw_open(int writable);

static inline void cz_seq_write_begin(_Atomic uint32_t *seq)
{
    uint32_t s = atomic_load_explicit(seq, memory_order_relaxed);
    atomic_store_explicit(seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static inline void cz_seq_write_end(_Atomic uint32_t *seq)
{
    atomic_thread_fence(memory_order_release);
    uint32_t s = atomic_load_explicit(seq, memory_order_relaxed);
    atomic_store_explicit(seq, s + 1, memory_order_release);
}

static inline uint32_t cz_seq_read_begin(_Atomic uint32_t *seq)
{
    uint32_t s;
    while ((s = atomic_load_explicit(seq, memory_order_acquire)) & 1u) { }
    return s;
}

static inline int cz_seq_read_retry(_Atomic uint32_t *seq, uint32_t start)
{
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(seq, memory_order_relaxed) != start;
}

/* Copy a loop's status block consistently. */
void cz_loop_read_status(struct cz_loop *l, struct cz_loop_status *out);
void cz_loop_push(struct cz_loop *l, const struct cz_sample *s);

/* Append an event to a producer ring (single writer per ring). */
void cz_emit(struct cz_bus *b, int src, int kind, int proc, int pid, int arg0,
             int64_t v0, int64_t v1, const char *text);

/* Reader helper: copy events [*tail, head) into out (max n), advancing *tail.
 * Returns the number copied; *lost counts events overwritten before reading. */
int cz_events_read(struct cz_evring *r, uint64_t *tail, struct cz_event *out, int n, uint64_t *lost);

#endif
