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
    // About, View toggle, and Help are all SwiftUI CommandGroups now.
    // AppDelegate only needs to: neutralize Help Book search + adopt Window menu.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      self?.customiseMenus()
    }
  }

  // MARK: - Menu Customisation (runs once after SwiftUI menu setup)

  private func customiseMenus() {
    guard let mainMenu = NSApp.mainMenu else { return }

    // Adopt the existing Window menu (so AppKit auto-populates with NSWindows)
    if let existingWindowMenu = mainMenu.items.first(where: { $0.submenu?.title == "Window" })?.submenu {
      NSApp.windowsMenu = existingWindowMenu
    }

    // Neutralize macOS Help Book search dialog.  SwiftUI's Help CommandGroup
    // creates a menu that macOS recognises as the "help menu" and adds a
    // Spotlight-style search field to.  Setting helpMenu to nil prevents this.
    NSApp.helpMenu = nil
  }

  // MARK: - Help Window (static so SwiftUI CommandGroup can call it)

  static func openHelpWindow() {
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
