#include "x11_shim.h"
#include <stdint.h>
#include <stdlib.h>

static x11_window_created_cb s_on_create = 0;
static x11_window_closed_cb  s_on_close  = 0;
static x11_present_frame_cb    s_present = 0;

void x11_register_callbacks(
                            x11_window_created_cb on_create,
                            x11_window_closed_cb on_close)
{
  s_on_create = on_create;
  s_on_close  = on_close;
}

bool x11_start_server(int32_t display)
{
  (void)display;
  
  // Test window
  if (s_on_create) {
    s_on_create(0x10001, "Test X11 Window", 800, 600);
  }
  
  // Initial paint (uses the same path as resize repaint)
  x11_request_repaint(0x10001, 800, 600);

  return true;
}

void x11_stop_server(void)
{
  if (s_on_close) {
    s_on_close(0x10001);
  }
}

void x11_register_frame_presenter(x11_present_frame_cb on_present) {
  s_present = on_present;
}

// Simple BGRA test pattern generator that matches the requested size.
void x11_request_repaint(uint32_t xwin_id, int width_px, int height_px)
{
    if (!s_present) return;
    if (width_px <= 0 || height_px <= 0) return;

    const int bpr = width_px * 4;
    const size_t count = (size_t)width_px * (size_t)height_px;

    // Allocate a temporary buffer per repaint (fine for testing).
    uint32_t *buf = (uint32_t*)malloc(count * sizeof(uint32_t));
    if (!buf) return;

    for (int y = 0; y < height_px; y++) {
        for (int x = 0; x < width_px; x++) {
            uint8_t b = (uint8_t)(x & 0xFF);
            uint8_t g = (uint8_t)((y * 2) & 0xFF);
            uint8_t r = (uint8_t)((x ^ y) & 0xFF);
            uint8_t a = 0xFF;
            buf[(size_t)y * (size_t)width_px + (size_t)x] =
                (uint32_t)(b | (g << 8) | (r << 16) | (a << 24));
        }
    }

    s_present(xwin_id, buf, width_px, height_px, bpr);

    free(buf);
}

