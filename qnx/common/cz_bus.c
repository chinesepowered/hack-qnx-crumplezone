/*
 * Crumple Zone: shared-memory bus access.
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include "cz_bus.h"
#include "cz_rt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syspage.h>
#include <unistd.h>

static void *map_object(const char *name, size_t size, int create, int writable)
{
    int oflag = writable ? O_RDWR : O_RDONLY;
    if (create) oflag |= O_CREAT | O_TRUNC;
    /* 0644: root processes may write, everyone else may at most read. The
     * untrusted infotainment process runs as a different uid and gets no
     * write access to either object. */
    int fd = shm_open(name, oflag, 0644);
    if (fd == -1) return NULL;
    if (create && ftruncate(fd, (off_t)size) == -1) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    return p;
}

struct cz_bus *cz_bus_create(void)
{
    struct cz_bus *b = map_object(CZ_BUS_NAME, sizeof(struct cz_bus), 1, 1);
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->version = CZ_BUS_VERSION;
    b->boot_ns = cz_now_ns();
    b->ncpu = cz_num_cpus();
    b->cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    for (int i = 0; i < CZ_NPROC; i++) {
        snprintf(b->procs[i].name, sizeof(b->procs[i].name), "%s", cz_proc_name(i));
    }
    b->brake.st.period_ns = CZ_BRAKE_PERIOD_NS;
    b->steer.st.period_ns = CZ_STEER_PERIOD_NS;
    atomic_thread_fence(memory_order_release);
    b->magic = CZ_BUS_MAGIC;
    return b;
}

struct cz_bus *cz_bus_open(int writable)
{
    struct cz_bus *b = map_object(CZ_BUS_NAME, sizeof(struct cz_bus), 0, writable);
    if (!b) return NULL;
    if (b->magic != CZ_BUS_MAGIC || b->version != CZ_BUS_VERSION) {
        munmap(b, sizeof(*b));
        errno = EINVAL;
        return NULL;
    }
    return b;
}

struct cz_hw *cz_hw_create(void)
{
    struct cz_hw *h = map_object(CZ_HW_NAME, sizeof(struct cz_hw), 1, 1);
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->version = 1;
    atomic_thread_fence(memory_order_release);
    h->magic = CZ_HW_MAGIC;
    return h;
}

struct cz_hw *cz_hw_open(int writable)
{
    struct cz_hw *h = map_object(CZ_HW_NAME, sizeof(struct cz_hw), 0, writable);
    if (!h) return NULL;
    if (h->magic != CZ_HW_MAGIC) {
        munmap(h, sizeof(*h));
        errno = EINVAL;
        return NULL;
    }
    return h;
}

void cz_loop_read_status(struct cz_loop *l, struct cz_loop_status *out)
{
    uint32_t s;
    do {
        s = cz_seq_read_begin(&l->seq);
        memcpy(out, &l->st, sizeof(*out));
    } while (cz_seq_read_retry(&l->seq, s));
}

void cz_loop_push(struct cz_loop *l, const struct cz_sample *smp)
{
    uint64_t h = atomic_load_explicit(&l->head, memory_order_relaxed);
    l->s[h & (CZ_SAMPLES - 1)] = *smp;
    atomic_store_explicit(&l->head, h + 1, memory_order_release);
}

void cz_emit(struct cz_bus *b, int src, int kind, int proc, int pid, int arg0,
             int64_t v0, int64_t v1, const char *text)
{
    if (!b || src < 0 || src >= CZ_NEVSRC) return;
    struct cz_evring *r = &b->ev[src];
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    struct cz_event *e = &r->ev[h & (CZ_EVENTS - 1)];
    e->ts_ns = cz_now_ns();
    e->kind = (uint16_t)kind;
    e->proc = (uint16_t)proc;
    e->pid = pid;
    e->arg0 = arg0;
    e->v0 = v0;
    e->v1 = v1;
    if (text) {
        strncpy(e->text, text, sizeof(e->text) - 1);
        e->text[sizeof(e->text) - 1] = '\0';
    } else {
        e->text[0] = '\0';
    }
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

int cz_events_read(struct cz_evring *r, uint64_t *tail, struct cz_event *out, int n, uint64_t *lost)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head - *tail > CZ_EVENTS) {
        if (lost) *lost += head - *tail - CZ_EVENTS;
        *tail = head - CZ_EVENTS;
    }
    int k = 0;
    while (*tail < head && k < n) {
        out[k++] = r->ev[*tail & (CZ_EVENTS - 1)];
        (*tail)++;
    }
    return k;
}
