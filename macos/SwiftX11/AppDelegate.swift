import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var statusItemController: StatusItemController?

  func applicationWillFinishLaunching(_ notification: Notification) {
    // Install the layout recursion guard before any windows are created.
    // This prevents _NSDetectedLayoutRecursion crashes caused by
    // FBSScene updates triggering recursive layout in SwiftUI windows.
    NSView.installLayoutRecursionGuard()
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return false
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    statusItemController = StatusItemController()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      StatusItemController.shared.install()
    }
    installWindowMenu()
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
