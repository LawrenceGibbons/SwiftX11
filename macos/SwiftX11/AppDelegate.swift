import Cocoa
import X11LowLevel

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
  private var statusItemController: StatusItemController?
  private var logWindowMenuItem: NSMenuItem?

  func applicationWillFinishLaunching(_ notification: Notification) {
    // Install the layout recursion guard before any windows are created.
    // This prevents _NSDetectedLayoutRecursion crashes caused by
    // FBSScene updates triggering recursive layout in SwiftUI windows.
    NSView.installLayoutRecursionGuard()
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return false
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    // Stop the X11 server first to close all client sockets and the poll loop.
    // Without this, windowWillClose posts host commands that can deadlock if the
    // server thread is blocked on socket I/O with a connected client.
    x11_stop_server()
    // Small delay to let the server thread exit its poll loop before we tear
    // down windows and sockets from under it.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
      NSApp.reply(toApplicationShouldTerminate: true)
    }
    return .terminateLater
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    // Explicitly set the app icon for Stage Manager / Dock / window server.
    // Load from the bundle icns first (complete 10-type file), fall back to
    // the asset catalog.  Also push to the Dock tile so Stage Manager picks
    // it up even when using a cached stale entry.
    let icon: NSImage? = {
      if let url = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
         let img = NSImage(contentsOf: url) { return img }
      return NSImage(named: "AppIcon")
    }()
    if let icon {
      icon.size = NSSize(width: 512, height: 512)  // ensure HiDPI representations
      NSApp.applicationIconImage = icon
      let imgView = NSImageView(image: icon)
      NSApp.dockTile.contentView = imgView
      NSApp.dockTile.display()
    }

    statusItemController = StatusItemController()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      StatusItemController.shared.install()
    }
    // Delay menu installation so SwiftUI has finished setting up its menu bar.
    // SwiftUI overwrites menus installed before it renders.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      self?.installViewMenu()
      self?.installWindowMenu()
    }
  }

  /// Install a "View" menu with "Show/Hide Log Window" (Cmd+0).
  /// Uses NSMenuDelegate to toggle the title based on current window visibility.
  private func installViewMenu() {
    guard let mainMenu = NSApp.mainMenu else { return }

    let viewMenu = NSMenu(title: "View")
    viewMenu.delegate = self

    let logItem = NSMenuItem(title: "Show Log Window",
                             action: #selector(toggleLogWindow),
                             keyEquivalent: "0")
    logItem.target = self
    viewMenu.addItem(logItem)
    self.logWindowMenuItem = logItem

    let viewMenuItem = NSMenuItem(title: "View", action: nil, keyEquivalent: "")
    viewMenuItem.submenu = viewMenu
    // Insert after Edit (index ~2) or at position 2 if menu is short
    let insertIndex = min(2, mainMenu.items.count)
    mainMenu.insertItem(viewMenuItem, at: insertIndex)
  }

  @objc private func toggleLogWindow() {
    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" }),
       win.isVisible {
      win.orderOut(nil)
    } else {
      NSApp.activate(ignoringOtherApps: true)
      if let win = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" }) {
        win.makeKeyAndOrderFront(nil)
      }
    }
  }

  // MARK: - NSMenuDelegate

  func menuNeedsUpdate(_ menu: NSMenu) {
    let isVisible = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" })?.isVisible ?? false
    logWindowMenuItem?.title = isVisible ? "Hide Log Window" : "Show Log Window"
  }

  /// Install a standard macOS "Window" menu so all X11 NSWindows appear in
  /// the menu bar and can be brought to front via the menu.  AppKit
  /// automatically manages the list of visible windows once windowsMenu is set.
  private func installWindowMenu() {
    guard let mainMenu = NSApp.mainMenu else { return }

    let windowMenu = NSMenu(title: "Window")

    // Standard items
    windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.miniaturize(_:)), keyEquivalent: "m")
    windowMenu.addItem(withTitle: "Zoom", action: #selector(NSWindow.zoom(_:)), keyEquivalent: "")
    windowMenu.addItem(.separator())
    windowMenu.addItem(withTitle: "Bring All to Front", action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")

    let windowMenuItem = NSMenuItem(title: "Window", action: nil, keyEquivalent: "")
    windowMenuItem.submenu = windowMenu
    // Insert before the last item (Help) if present, otherwise append
    let insertIndex = max(mainMenu.items.count - 1, 0)
    mainMenu.insertItem(windowMenuItem, at: insertIndex)

    // Tell AppKit this is THE window menu — it will auto-populate with NSWindows
    NSApp.windowsMenu = windowMenu
  }
}
