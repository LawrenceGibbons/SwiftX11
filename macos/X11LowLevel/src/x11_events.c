//
//  x11_events.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/1/26.
//

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <inttypes.h> 
#include "x11_events.h"

#ifndef X11_EVENT_Q_CAP
#define X11_EVENT_Q_CAP 4096
#endif

#define X11_ENABLE_MOTION_COALESCE 1

typedef struct {
    x11_event_t buf[X11_EVENT_Q_CAP];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
} x11_event_queue_t;

static x11_event_queue_t g_q;

// Debug counters
static _Atomic uint64_t g_motion_overwrites = 0;  // NEW
static _Atomic uint64_t g_push_drops = 0;         // NEW

// stringify event types for the dump
static const char* x11_ev_name(x11_event_type_t t) {
    switch (t) {
        case X11_EV_NONE:           return "NONE";
        case X11_EV_POINTER_MOTION: return "MOTION";
        case X11_EV_POINTER_BUTTON: return "BUTTON";
        case X11_EV_SCROLL:         return "SCROLL";
        case X11_EV_KEY:            return "KEY";
        case X11_EV_MODIFIERS:      return "MODS";
        case X11_EV_FOCUS:          return "FOCUS";
        case X11_EV_WINDOW_CREATE:  return "WIN_CREATE";
        case X11_EV_WINDOW_DESTROY: return "WIN_DESTROY";
        case X11_EV_POINTER_ENTER:  return "ENTER";
        case X11_EV_POINTER_LEAVE:  return "LEAVE";
        case X11_EV_WINDOW_RAISE:   return "RAISE";
        default:                    return "UNKNOWN";
    }
}

// helper to append safely into a caller buffer
static void x11_buf_append(char* dst, size_t cap, size_t* pos, const char* fmt, ...) {
    if (!dst || cap == 0 || !pos) return;
    if (*pos >= cap) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + *pos, cap - *pos, fmt, ap);
    va_end(ap);

    if (n <= 0) return;
    size_t nn = (size_t)n;
    *pos = (*pos + nn < cap) ? (*pos + nn) : cap;
}

void x11_events_init(void) {
  atomic_store(&g_q.head, 0);
  atomic_store(&g_q.tail, 0);

  // reset counters at init
  atomic_store(&g_motion_overwrites, 0);        // NEW
  atomic_store(&g_push_drops, 0);               // NEW
  
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

#if X11_ENABLE_MOTION_COALESCE
  // Motion coalescing: if the most recently enqueued event is also a motion event
  // for the same XID, overwrite it in-place instead of enqueueing a new one.
  // This prevents pointer-move storms from starving more important events.
  // NOTE: This queue is currently used in an SPSC-style pattern (single producer,
  // single consumer). If you later introduce multiple producers, this needs a
  // different synchronization strategy.
  if (ev->type == X11_EV_POINTER_MOTION) {
    if (head != tail) {
      // Start at last enqueued slot (head-1)
      uint32_t i = (head == 0) ? (X11_EVENT_Q_CAP - 1u) : (head - 1u); // NEW
      
      while (true) { 
        const x11_event_t* last = &g_q.buf[i];

        // Stop at first non-motion event: do not coalesce across button/key/etc.
        if (last->type != X11_EV_POINTER_MOTION) {
          break; 
        }

        // If it's motion for the same window, overwrite in-place.
        if (last->xid == ev->xid) { // NEW
          g_q.buf[i] = *ev;
          atomic_fetch_add_explicit(&g_motion_overwrites, 1, memory_order_relaxed); // NEW
          return true; // 
        }

        // Don't scan past tail (don't walk into unread region boundary)
        if (i == tail) { 
          break; 
        }

        // Move backward with wrap-around
        i = (i == 0) ? (X11_EVENT_Q_CAP - 1u) : (i - 1u); // NEW
      } 
    }
  }
#endif
  // OLD simple coalescer (kept here as reference):
  // if (ev->type == X11_EV_POINTER_MOTION) {
  //     if (head != tail) {
  //         uint32_t prev = (head == 0) ? (X11_EVENT_Q_CAP - 1u) : (head - 1u);
  //         const x11_event_t* last = &g_q.buf[prev];
  //         if (last->type == X11_EV_POINTER_MOTION && last->xid == ev->xid) {
  //             g_q.buf[prev] = *ev;
  //             atomic_fetch_add_explicit(&g_motion_overwrites, 1, memory_order_relaxed);
  //             return true;
  //         }
  //     }
  // }

  uint32_t n = next_i(head);
  if (n == tail) {
    // track drops when full
    atomic_fetch_add_explicit(&g_push_drops,                  // NEW
                              1, memory_order_relaxed);       // NEW
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

// Debug getters
uint64_t x11_debug_motion_overwrites(void) {           
    return atomic_load_explicit(&g_motion_overwrites,  
                                memory_order_relaxed); 
}                                                      

uint64_t x11_debug_push_drops(void) {                  
    return atomic_load_explicit(&g_push_drops,         
                                memory_order_relaxed); 
}                                                      

void x11_debug_reset_counters(void) {                  
    atomic_store_explicit(&g_motion_overwrites, 0,     
                          memory_order_relaxed);       
    atomic_store_explicit(&g_push_drops, 0,            
                          memory_order_relaxed);       
}                                                      

bool x11_debug_dump_queue(char* dst, size_t cap, uint32_t max_items) {
    if (!dst || cap == 0) return false;
    dst[0] = 0;

    // Snapshot indices
    uint32_t head = atomic_load_explicit(&g_q.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&g_q.tail, memory_order_acquire);

    uint32_t count = 0;
    if (head >= tail) count = head - tail;
    else count = (X11_EVENT_Q_CAP - tail) + head;

    uint64_t co = atomic_load_explicit(&g_motion_overwrites, memory_order_relaxed);
    uint64_t dr = atomic_load_explicit(&g_push_drops, memory_order_relaxed);

    size_t pos = 0;

    x11_buf_append(dst, cap, &pos,
        "X11 EVENT QUEUE SNAPSHOT\n"
        "cap=%u head=%u tail=%u count=%u\n"
        "coalesce=%" PRIu64 " drops=%" PRIu64 "\n",
        (unsigned)X11_EVENT_Q_CAP,
        (unsigned)head,
        (unsigned)tail,
        (unsigned)count,
        co, dr
    );

    uint32_t n = count;
    if (max_items > 0 && n > max_items) n = max_items;

    x11_buf_append(dst, cap, &pos, "oldest->newest (showing %u):\n", (unsigned)n);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (tail + i) % X11_EVENT_Q_CAP;
        const x11_event_t* ev = &g_q.buf[idx];

        x11_buf_append(dst, cap, &pos,
            "  [%4u] idx=%4u xid=0x%X type=%s(%u) size=%u ts=%" PRIu64 "\n",
            (unsigned)i,
            (unsigned)idx,
            (unsigned)ev->xid,
            x11_ev_name(ev->type),
            (unsigned)ev->type,
            (unsigned)ev->size,
            (uint64_t)ev->timestamp_ns
        );

        if (pos + 2 >= cap) break;
    }

    return true;
}

x11_eventq_stats_t x11_events_stats(void) {
  x11_eventq_stats_t s;
  s.head = atomic_load_explicit(&g_q.head, memory_order_relaxed);
  s.tail = atomic_load_explicit(&g_q.tail, memory_order_relaxed);

  if (s.head >= s.tail) s.count = s.head - s.tail;
  else s.count = (X11_EVENT_Q_CAP - s.tail) + s.head;

  s.motion_overwrites = atomic_load_explicit(&g_motion_overwrites, memory_order_relaxed);
  s.push_drops        = atomic_load_explicit(&g_push_drops, memory_order_relaxed);
  return s;
}
