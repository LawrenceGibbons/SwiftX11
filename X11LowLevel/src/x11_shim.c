#include "x11_shim.h"

static x11_window_created_cb s_create;
static x11_window_closed_cb  s_close;

void x11_register_callbacks(x11_window_created_cb on_create,
                            x11_window_closed_cb on_close)
{
    s_create = on_create;
    s_close  = on_close;
}

bool x11_start_server(int32_t display)
{
    (void)display;
    // TODO: initialize low-level X11 server using existing C libs

    // Temporary fake test window for Swift integration:
    if (s_create) {
        s_create(0x10001, "Test X11 Window", 800, 600);
    }

    return true;
}

void x11_stop_server(void)
{
    if (s_close) {
        s_close(0x10001);
    }
}
