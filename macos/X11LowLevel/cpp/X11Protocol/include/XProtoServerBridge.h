//
//  XProtoServerBridge.h
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/21/26.
//

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once per accepted client session.
void x11_proto_bridge_begin_session(int client_fd);

// Called at end of session (disconnect).
void x11_proto_bridge_end_session(void);

// Called from drain_requests loop.
void x11_proto_bridge_note_last_seq(uint16_t seq);

// Called from drain_requests loop (top of loop and/or after dispatch).
void x11_proto_bridge_flush_notify_queue(void);

// Called from places that currently call queue_notify / enqueue Configure/Expose.
void x11_proto_bridge_queue_notify(uint32_t wid, int want_configure, int want_expose);

void x11_proto_bridge_queue_expose_rect(uint32_t wid,
                                        uint16_t x, uint16_t y,
                                        uint16_t w, uint16_t h,
                                        uint16_t count);
  
// Send raw reply/handshake bytes on the current client connection (must be called on xproto thread).
// Returns 1 on success, 0 on failure.
int x11_proto_bridge_send_reply_bytes(const void* buf, size_t n);

  
// int x11_proto_bridge_send_get_geometry_reply(uint16_t seq,
//                                              uint32_t root,
//                                              int16_t x, int16_t y,
//                                              uint16_t w, uint16_t h,
//                                              uint16_t borderWidth,
//                                              uint16_t depth);

  
// int x11_proto_bridge_send_get_input_focus_reply(uint16_t seq,
//                                                 uint8_t revert_to,
//                                                 uint32_t focus);
  
  
int x11_proto_bridge_send_intern_atom_reply(uint16_t seq, uint32_t atom);
int x11_proto_bridge_send_get_atom_name_reply(uint16_t seq, const char* name, uint16_t nameLen);
  
  
int x11_proto_bridge_dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                                const uint8_t* payload, size_t remain);

  
//void x11_proto_bridge_window_upsert(uint32_t xid, uint32_t parent,
//                                   int16_t x, int16_t y,
//                                   uint16_t w, uint16_t h,
//                                   uint32_t event_mask,
//                                   int owner_fd);

void x11_proto_bridge_window_erase(uint32_t xid);

void x11_proto_bridge_window_set_mapped(uint32_t xid, int mapped);
void x11_proto_bridge_window_set_presentable(uint32_t xid, int presentable);
void x11_proto_bridge_window_set_event_mask(uint32_t xid, uint32_t event_mask);
void x11_proto_bridge_window_set_geometry(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h);

int  x11_proto_bridge_window_is_ready_to_present(uint32_t xid);
void x11_proto_bridge_window_mark_dirty(uint32_t xid);
int  x11_proto_bridge_window_consume_dirty_if_ready(uint32_t xid);

void x11_proto_bridge_window_debug_state(uint32_t xid,
                                         uint32_t* out_parent,
                                         int* out_mapped,
                                         int* out_presentable,
                                         int* out_dirty,
                                         int* out_owner_fd);

void x11_xproto_c_set_window_event_mask(uint32_t xid, uint32_t event_mask);


// Apply ConfigureWindow result to C canonical window + framebuffer.
// resize_fb = 1 means: if w/h changed, resize framebuffer with preserve+white-fill.
void x11_xproto_apply_configure_from_cpp(uint32_t wid,
                                        int16_t x, int16_t y,
                                        uint16_t w, uint16_t h,
                                        int resize_fb);

  
void x11_proto_bridge_pixmap_create(uint32_t pid, uint8_t depth, uint16_t w, uint16_t h);
void x11_proto_bridge_pixmap_free(uint32_t pid);

  // Returns 1 on success, 0 on failure.
  // outPixels is a pointer to the window framebuffer (ARGB/BGRA as you currently store it),
  // outW/outH are pixel dimensions.
  int x11_xproto_window_fb_rw(uint32_t xid,
                              uint32_t** outPixels,
                              uint32_t* outW,
                              uint32_t* outH);

  // Call the exact same damage gating as the old C draw ops.
  void x11_xproto_enqueue_damage(uint32_t xid);



  
#ifdef __cplusplus
}
#endif

