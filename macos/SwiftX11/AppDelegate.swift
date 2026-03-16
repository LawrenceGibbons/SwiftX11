import Cocoa
import SwiftUI
import X11LowLevel

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuItemValidation {
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
    // We augment SwiftUI's existing menus rather than creating duplicates.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      self?.customiseMenus()
    }
  }

  // MARK: - Menu Customisation (runs once after SwiftUI menu setup)

  /// Single entry point — augments SwiftUI's menus without creating duplicates.
  private func customiseMenus() {
    guard let mainMenu = NSApp.mainMenu else { return }

    // 1. Retarget the About item in the app menu
    if let appMenu = mainMenu.items.first?.submenu {
      if let aboutItem = appMenu.items.first(where: {
        $0.action == #selector(NSApplication.orderFrontStandardAboutPanel(_:))
      }) {
        aboutItem.target = self
        aboutItem.action = #selector(showAboutPanel)
      }
    }

    // 2. Add "Show/Hide Log Window" to the existing View menu (or create one)
    let viewMenu: NSMenu
    if let existing = mainMenu.items.first(where: { $0.submenu?.title == "View" })?.submenu {
      viewMenu = existing
    } else {
      viewMenu = NSMenu(title: "View")
      let viewMenuItem = NSMenuItem(title: "View", action: nil, keyEquivalent: "")
      viewMenuItem.submenu = viewMenu
      let insertIndex = min(2, mainMenu.items.count)
      mainMenu.insertItem(viewMenuItem, at: insertIndex)
    }
    // Only add our item if it isn't already there
    if viewMenu.items.first(where: { $0.action == #selector(toggleLogWindow) }) == nil {
      if !viewMenu.items.isEmpty { viewMenu.addItem(.separator()) }
      let logItem = NSMenuItem(title: "Show Log Window",
                               action: #selector(toggleLogWindow),
                               keyEquivalent: "0")
      logItem.target = self
      viewMenu.addItem(logItem)
    }

    // 3. Adopt the existing Window menu (so AppKit auto-populates it with NSWindows)
    if let existingWindowMenu = mainMenu.items.first(where: { $0.submenu?.title == "Window" })?.submenu {
      NSApp.windowsMenu = existingWindowMenu
    }

    // 4. Replace the Help menu (SwiftUI's triggers the macOS Help Book dialog)
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

  // MARK: - NSMenuItemValidation

  /// Dynamically update menu item titles before they are displayed.
  func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
    if menuItem.action == #selector(toggleLogWindow) {
      let isVisible = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" })?.isVisible ?? false
      menuItem.title = isVisible ? "Hide Log Window" : "Show Log Window"
    }
    return true
  }

  // MARK: - Actions

  @objc private func showAboutPanel() {
    let version = XServerController.buildVersion
    let buildDate = XServerController.buildDate
    let credits = NSAttributedString(
      string: "An X11 display server for macOS.\n\nDeveloped by Rlan and Claude.",
      attributes: [
        .font: NSFont.systemFont(ofSize: 11),
        .foregroundColor: NSColor.labelColor
      ]
    )
    NSApp.orderFrontStandardAboutPanel(options: [
      .applicationName: "SwiftX11",
      .applicationVersion: version,
      .version: "Built \(buildDate)",
      .credits: credits
    ])
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
