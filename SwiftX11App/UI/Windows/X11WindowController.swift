import AppKit
import SwiftUI

final class X11WindowController: NSWindowController {
    init(title: String) {
        let hosting = NSHostingController(rootView: X11WindowHost())
        let window = NSWindow(contentViewController: hosting)
        window.title = title.isEmpty ? "SwiftX11 Window" : title
        window.setContentSize(NSSize(width: 800, height: 600))
        window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
        window.isReleasedWhenClosed = false
        super.init(window: window)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
