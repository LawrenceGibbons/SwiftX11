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
void x11_proto_bridge_begin_session(int client_fd,
                                    uint32_t rid_base,
                                    uint32_t rid_mask);
  
// Called at end of session (disconnect).
void x11_proto_bridge_end_session(int client_fd);

// Called from drain_requests loop.
void x11_proto_bridge_note_last_seq(uint16_t seq);

// Called from drain_requests loop (top of loop and/or after dispatch).
void x11_proto_bridge_flush_notify_queue(void);

// Process a single host command (caller must activate the correct client).
void x11_proto_bridge_process_host_cmd(const void* cmd_ptr);


void x11_proto_bridge_queue_expose_rect(uint32_t wid,
                                        uint16_t x, uint16_t y,
                                        uint16_t w, uint16_t h,
                                        uint16_t count);
  
 
  
int x11_proto_bridge_dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                                const uint8_t* payload, size_t remain);

  

void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid);

// Called when the host surface dimensions change after initial registration.
// Triggers re-expose of the host + all mapped descendants on the xproto thread.
void x11_proto_bridge_surface_resized(uint32_t xid);
  

// ------- mouse related event bridging  
//void x11_proto_bridge_post_pointer_move(uint32_t xid,
//                                       int32_t x_px, int32_t y_px,
//                                       uint32_t buttons, uint32_t modifiers);

void x11_proto_bridge_post_pointer_button_legacy(uint32_t xid,
                                                int is_press,
                                                int32_t x_px, int32_t y_px,
                                                uint32_t buttons, uint32_t modifiers);
  
void x11_proto_bridge_post_pointer_move2(uint32_t xid,
                                         int32_t win_x, int32_t win_y,
                                         int32_t root_x, int32_t root_y,
                                         uint8_t deliver,
                                         uint32_t buttons,
                                         uint32_t modifiers);

  void x11_proto_bridge_post_pointer_button(uint32_t xid,
                                            uint8_t is_press,
                                            uint8_t button,
                                            int32_t win_x, int32_t win_y,
                                            uint32_t buttons,
                                            uint32_t modifiers);

  void x11_proto_bridge_post_scroll(uint32_t xid,
                                    uint8_t axis,
                                    int16_t ticks,
                                    int32_t win_x_u, int32_t win_y_u,
                                    uint32_t buttons,
                                    uint32_t modifiers);

  void x11_proto_bridge_post_key(uint32_t xid,
                                 uint8_t is_down,
                                 uint32_t keycode,
                                 uint32_t modifiers);

  void x11_proto_bridge_post_enter(uint32_t xid,
                                   int32_t win_x, int32_t win_y,
                                   uint32_t modifiers);

  void x11_proto_bridge_post_leave(uint32_t xid,
                                   int32_t win_x, int32_t win_y,
                                   uint32_t modifiers);

  void x11_proto_bridge_post_focus(uint32_t xid,
                                   uint8_t focused);
  
  uint32_t x11_cpp_list_descendants(uint32_t host, uint32_t* out, uint32_t cap);

  int x11_cpp_get_window_geom(uint32_t xid,
                              uint32_t* out_parent,
                              int16_t* out_x,
                              int16_t* out_y,
                              uint16_t* out_w,
                              uint16_t* out_h,
                              int* out_mapped);
  
  int x11_cpp_get_abs_pos_in_host(uint32_t host, uint32_t xid,
                                  int32_t* out_abs_x,
                                  int32_t* out_abs_y);
  
#ifdef __cplusplus
}
#endif

