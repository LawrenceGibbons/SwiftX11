//
//  x11_events.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/1/26.
//

#include <stdio.h>
#include "x11_events.h"
#include <stdatomic.h>
#include <string.h>

#ifndef X11_EVENT_Q_CAP
#define X11_EVENT_Q_CAP 4096
#endif

typedef struct {
    x11_event_t buf[X11_EVENT_Q_CAP];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
} x11_event_queue_t;

static x11_event_queue_t g_q;

void x11_events_init(void) {
    atomic_store(&g_q.head, 0);
    atomic_store(&g_q.tail, 0);
}

void x11_events_shutdown(void) {
    // nothing for now
}

static inline uint32_t next_i(uint32_t i) {
    return (i + 1u) % X11_EVENT_Q_CAP;
}

bool x11_events_push(const x11_event_t* ev) {
    uint32_t head = atomic_load_explicit(&g_q.head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_q.tail, memory_order_acquire);
    uint32_t n = next_i(head);
    if (n == tail) {
        return false; // full
    }
    g_q.buf[head] = *ev;
    atomic_store_explicit(&g_q.head, n, memory_order_release);
    return true;
}

bool x11_events_pop(x11_event_t* out_ev) {
    uint32_t tail = atomic_load_explicit(&g_q.tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&g_q.head, memory_order_acquire);
    if (tail == head) {
        return false; // empty
    }
    *out_ev = g_q.buf[tail];
    atomic_store_explicit(&g_q.tail, next_i(tail), memory_order_release);
    return true;
}

uint32_t x11_events_count(void) {
    uint32_t head = atomic_load(&g_q.head);
    uint32_t tail = atomic_load(&g_q.tail);
    if (head >= tail) return head - tail;
    return (X11_EVENT_Q_CAP - tail) + head;
}

void x11_events_clear(void) {
    atomic_store(&g_q.tail, atomic_load(&g_q.head));
}
