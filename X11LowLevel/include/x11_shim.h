#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*x11_window_created_cb)(uint32_t xwin_id,
                                      const char* title,
                                      int width,
                                      int height);

typedef void (*x11_window_closed_cb)(uint32_t xwin_id);

bool x11_start_server(int32_t display);
void x11_stop_server(void);

void x11_register_callbacks(x11_window_created_cb on_create,
                            x11_window_closed_cb on_close);

#ifdef __cplusplus
}
#endif
