import Foundation
import AppKit

@_cdecl("swift_on_x11_window_created")
func swift_on_x11_window_created(xwin_id: UInt32,
                                 titlePtr: UnsafePointer<CChar>?,
                                 width: Int32,
                                 height: Int32)
{
    let title = titlePtr.flatMap { String(cString: $0) } ?? "SwiftX11 Window"
    DispatchQueue.main.async {
        let controller = X11WindowController(title: title)
        controller.showWindow(nil)
        // TODO: store window controller in registry keyed by xwin_id
    }
}

@_cdecl("swift_on_x11_window_closed")
func swift_on_x11_window_closed(xwin_id: UInt32) {
    DispatchQueue.main.async {
        // TODO: look up controller by ID and close window
    }
}
