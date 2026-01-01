import Cocoa

final class StatusItemController {
    private var statusItem: NSStatusItem?

    func install() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem?.button {
            button.image = NSImage(systemSymbolName: "rectangle.on.rectangle", accessibilityDescription: "SwiftX11")
        }

        let menu = NSMenu()
        menu.addItem(withTitle: "Start Server", action: #selector(startServer), keyEquivalent: "")
        menu.addItem(withTitle: "Stop Server", action: #selector(stopServer), keyEquivalent: "")
        menu.addItem(NSMenuItem.separator())
        menu.addItem(withTitle: "Preferences…", action: #selector(openPreferences), keyEquivalent: ",")
        menu.addItem(withTitle: "Quit SwiftX11", action: #selector(quit), keyEquivalent: "q")
        statusItem?.menu = menu
    }

    @objc private func startServer() { NotificationCenter.default.post(name: .x11StartRequested, object: nil) }
    @objc private func stopServer()  { NotificationCenter.default.post(name: .x11StopRequested, object: nil) }
    @objc private func openPreferences() { PreferencesWindow.open() }
    @objc private func quit() { NSApp.terminate(nil) }
}

extension Notification.Name {
    static let x11StartRequested = Notification.Name("x11StartRequested")
    static let x11StopRequested  = Notification.Name("x11StopRequested")
}
