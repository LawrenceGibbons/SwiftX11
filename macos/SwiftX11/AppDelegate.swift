import Cocoa
import SwiftUI
import X11LowLevel

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
  private var statusItemController: StatusItemController?
  private var logWindowMenuItem: NSMenuItem?

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
    // Delay menu installation so SwiftUI has finished setting up its menu bar.
    // SwiftUI overwrites menus installed before it renders.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      self?.installAboutMenuItem()
      self?.installViewMenu()
      self?.installWindowMenu()
      self?.installHelpMenu()
    }
  }

  /// Install "About SwiftX11" in the app menu (replacing the empty SwiftUI stub).
  private func installAboutMenuItem() {
    guard let mainMenu = NSApp.mainMenu,
          let appMenu = mainMenu.items.first?.submenu else { return }
    let aboutItem = NSMenuItem(title: "About SwiftX11",
                               action: #selector(showAboutPanel),
                               keyEquivalent: "")
    aboutItem.target = self
    appMenu.insertItem(aboutItem, at: 0)
  }

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

  /// Install a "Help" menu with "SwiftX11 Help" (Cmd+?).
  /// Replaces the default (empty) SwiftUI Help menu to avoid the
  /// "Help isn't available" dialog triggered by macOS Help Book system.
  private func installHelpMenu() {
    guard let mainMenu = NSApp.mainMenu else { return }

    // Remove the default Help menu if SwiftUI created one
    if let existingHelp = mainMenu.items.last, existingHelp.submenu?.title == "Help" {
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
