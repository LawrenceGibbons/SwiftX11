import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var statusItemController: StatusItemController?

  func applicationWillFinishLaunching(_ notification: Notification) {
    // Install the layout recursion guard before any windows are created.
    // This prevents _NSDetectedLayoutRecursion crashes caused by
    // FBSScene updates triggering recursive layout in SwiftUI windows.
    NSView.installLayoutRecursionGuard()
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    statusItemController = StatusItemController()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      StatusItemController.shared.install()
    }
    installViewMenu()
    installWindowMenu()
  }

  /// Install a "View" menu with "Show Control Panel" (Cmd+0).
  private func installViewMenu() {
    guard let mainMenu = NSApp.mainMenu else { return }

    let viewMenu = NSMenu(title: "View")
    let showPanel = NSMenuItem(title: "Show Control Panel",
                               action: #selector(showControlPanel),
                               keyEquivalent: "0")
    showPanel.target = self
    viewMenu.addItem(showPanel)

    let viewMenuItem = NSMenuItem(title: "View", action: nil, keyEquivalent: "")
    viewMenuItem.submenu = viewMenu
    // Insert after Edit (index ~2) or at position 2 if menu is short
    let insertIndex = min(2, mainMenu.items.count)
    mainMenu.insertItem(viewMenuItem, at: insertIndex)
  }

  @objc private func showControlPanel() {
    NSApp.activate(ignoringOtherApps: true)
    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11" }) {
      win.makeKeyAndOrderFront(nil)
    }
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
