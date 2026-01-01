#include "x11_shim.h"

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
  
  if (s_present) {
    const int w = 320, h = 200, bpr = w * 4;
    static uint32_t buf[320 * 200];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        // BGRA: blue gradient + green bars
        uint8_t b = (uint8_t)(x & 0xFF);
        uint8_t g = (uint8_t)((y * 2) & 0xFF);
        uint8_t r = (uint8_t)((x ^ y) & 0xFF);
        uint8_t a = 0xFF;
        buf[y*w + x] = (uint32_t)(b | (g<<8) | (r<<16) | (a<<24));
      }
    }
    s_present(0x10001, buf, w, h, bpr);
  }
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
