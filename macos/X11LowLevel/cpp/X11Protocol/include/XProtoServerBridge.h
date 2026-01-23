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

#ifdef __cplusplus
}
#endif

