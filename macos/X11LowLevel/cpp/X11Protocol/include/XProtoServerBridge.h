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
void x11_proto_bridge_end_session(int client_fd);

// Called from drain_requests loop.
void x11_proto_bridge_note_last_seq(uint16_t seq);

// Called from drain_requests loop (top of loop and/or after dispatch).
void x11_proto_bridge_flush_notify_queue(void);


void x11_proto_bridge_queue_expose_rect(uint32_t wid,
                                        uint16_t x, uint16_t y,
                                        uint16_t w, uint16_t h,
                                        uint16_t count);
  
 
  
int x11_proto_bridge_dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                                const uint8_t* payload, size_t remain);

  

void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid);
  

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

#ifdef __cplusplus
}
#endif

