import AppKit
import SwiftUI

final class X11WindowController: NSWindowController, NSWindowDelegate {
    private(set) var x11View: X11View?
    private let xid: UInt32

    init(xid: UInt32, title: String, width: Int, height: Int) {
        self.xid = xid

        let viewHolder = X11ViewHolder()
        let host = X11WindowHost(useMetal: true) { view in
            viewHolder.view = view
        }
        let hosting = NSHostingController(rootView: host)

        let window = NSWindow(contentViewController: hosting)
        window.title = title.isEmpty ? "SwiftX11 Window" : title
        window.setContentSize(NSSize(width: width, height: height))
        window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
        window.isReleasedWhenClosed = false

        super.init(window: window)
        window.setContentSize(NSSize(width: width, height: height))
      
        self.x11View = viewHolder.view
        window.delegate = self
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func windowDidResize(_ notification: Notification) {
        guard let win = window else { return }

        // Size in points (logical)
        let sizePoints = win.contentLayoutRect.size

        // Scale factor (1.0 on non-Retina, 2.0 on Retina, etc.)
        let scale = win.backingScaleFactor

        // Size in pixels (physical)
        let sizePixels = CGSize(width: sizePoints.width * scale,
                                height: sizePoints.height * scale)

        WindowRegistry.shared.windowResized(
            xid: xid,
            sizePoints: sizePoints,
            sizePixels: sizePixels,
            scale: scale
        )
    }
  
    func windowDidEndLiveResize(_ notification: Notification) {
        // Force an immediate final repaint at the final size
        windowDidResize(notification)
        WindowRegistry.shared.flushRepaintNow(xid: xid)
    }
}

final class X11ViewHolder {
    var view: X11View?
}
