import SwiftUI
import AppKit

enum PreferencesWindow {
    static func open() {
        let vc = NSHostingController(rootView: PreferencesView())
        let win = NSWindow(contentViewController: vc)
        win.title = "SwiftX11 Preferences"
        win.setContentSize(NSSize(width: 720, height: 480))
        win.styleMask = [.titled, .closable, .miniaturizable]
        win.center()
        win.makeKeyAndOrderFront(nil)
    }
}
