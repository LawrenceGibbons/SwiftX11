import Cocoa

final class StatusItemController {
  static let shared = StatusItemController()

  private var installed = false
  private var statusItem: NSStatusItem?
  
  func install() {
    guard !installed else { return }
    installed = true

    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    self.statusItem = item

    guard let button = item.button else { return }

    let img = NSImage(systemSymbolName: "rectangle.on.rectangle",
                      accessibilityDescription: "SwiftX11")
    img?.isTemplate = true
    button.image = img
    button.imagePosition = .imageOnly

    let menu = NSMenu()

    func add(_ title: String, _ sel: Selector, key: String = "") {
      let it = NSMenuItem(title: title, action: sel, keyEquivalent: key)
      it.target = self
      menu.addItem(it)
    }

    add("Start Server", #selector(startServer))
    add("Stop Server",  #selector(stopServer))
    menu.addItem(.separator())
    add("Show Control Panel", #selector(showControlPanel), key: "0")
    add("Preferences…", #selector(openPreferences), key: ",")
    menu.addItem(.separator())
    add("Quit SwiftX11", #selector(quit), key: "q")

    // Defer menu attachment one tick to avoid sizing during creation
    DispatchQueue.main.async { [weak self] in
      self?.statusItem?.menu = menu
    }
  }

  
  @objc private func startServer() { NotificationCenter.default.post(name: .x11StartRequested, object: nil) }
  @objc private func stopServer()  { NotificationCenter.default.post(name: .x11StopRequested, object: nil) }
  @objc private func showControlPanel() {
    NSApp.activate(ignoringOtherApps: true)
    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11" }) {
      win.makeKeyAndOrderFront(nil)
    }
  }
  @objc private func openPreferences() {
    NSApp.activate(ignoringOtherApps: true)
    NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
  }
  @objc private func quit() { NSApp.terminate(nil) }
}

extension Notification.Name {
  static let x11StartRequested = Notification.Name("x11StartRequested")
  static let x11StopRequested  = Notification.Name("x11StopRequested")
  static let x11LogMessage     = Notification.Name("x11LogMessage")
}
