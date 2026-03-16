import Cocoa
import SwiftUI
import X11LowLevel

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var statusItemController: StatusItemController?

  func applicationWillFinishLaunching(_ notification: Notification) {
    // Install the layout recursion guard before any windows are created.
    // This prevents _NSDetectedLayoutRecursion crashes caused by
    // FBSScene updates triggering recursive layout in SwiftUI windows.
    NSView.installLayoutRecursionGuard()
    // Do NOT set applicationIconImage — macOS reads from the asset catalog
    // and applies the rounded superellipse mask automatically.  Overriding
    // it produces a raw square icon in the Dock.
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
    statusItemController = StatusItemController()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      StatusItemController.shared.install()
    }
    // Delay menu customisation so SwiftUI has finished setting up its menu bar.
    // Only Help and Window menus need AppDelegate — About and View are handled
    // by SwiftUI CommandGroups which survive SwiftUI's menu rebuilds.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      self?.customiseMenus()
    }
  }

  // MARK: - Menu Customisation (runs once after SwiftUI menu setup)

  /// Augments SwiftUI's menus for the items that can't be done in SwiftUI:
  /// - Help menu (SwiftUI triggers macOS Help Book "not available" dialog)
  /// - Window menu (needs NSApp.windowsMenu assignment for auto-population)
  private func customiseMenus() {
    guard let mainMenu = NSApp.mainMenu else { return }

    // 1. Adopt the existing Window menu (so AppKit auto-populates with NSWindows)
    if let existingWindowMenu = mainMenu.items.first(where: { $0.submenu?.title == "Window" })?.submenu {
      NSApp.windowsMenu = existingWindowMenu
    }

    // 2. Replace the Help menu (SwiftUI's triggers the macOS Help Book dialog)
    if let existingHelp = mainMenu.items.first(where: { $0.submenu?.title == "Help" }) {
      mainMenu.removeItem(existingHelp)
    }
    let helpMenu = NSMenu(title: "Help")
    let helpItem = NSMenuItem(title: "SwiftX11 Help",
                              action: #selector(showHelpWindow),
                              keyEquivalent: "/")
    helpItem.keyEquivalentModifierMask = [.command, .shift]
    helpItem.target = self
    helpMenu.addItem(helpItem)
    let helpMenuItem = NSMenuItem(title: "Help", action: nil, keyEquivalent: "")
    helpMenuItem.submenu = helpMenu
    mainMenu.addItem(helpMenuItem)
  }

  // MARK: - Actions

  @objc private func showHelpWindow() {
    // Look for an existing help window first
    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11 Help" }) {
      win.makeKeyAndOrderFront(nil)
      NSApp.activate(ignoringOtherApps: true)
      return
    }
    // Create a new help window with SwiftUI content
    let helpView = NSHostingController(rootView: HelpView())
    let window = NSWindow(contentViewController: helpView)
    window.title = "SwiftX11 Help"
    window.setContentSize(NSSize(width: 640, height: 700))
    window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
    window.center()
    window.makeKeyAndOrderFront(nil)
    NSApp.activate(ignoringOtherApps: true)
  }
}
