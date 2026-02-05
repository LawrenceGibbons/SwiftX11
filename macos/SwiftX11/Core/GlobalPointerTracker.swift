//
//  GlobalPointerTracker.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/5/26.
//

import AppKit
import Foundation
import X11LowLevel

@MainActor
final class GlobalPointerTracker {
  static let shared = GlobalPointerTracker()

  private var globalMonitor: Any?
  private var localMonitor: Any?
  private var timer: Timer?

  // We need an xid to associate with the host window for now (top-level)
  // If you have multiple windows, you can track “focused” xid here.
  private var activeXid: UInt32 = 0

  // Cache last known window-local coords so we can send something sensible
  private var lastWinXY: (Int32, Int32) = (0, 0)

  func updateActiveWindow(xid: UInt32, lastWinXY: (Int32, Int32)) {
    self.activeXid = xid
    self.lastWinXY = lastWinXY
  }

  func start() {
    stop()

    // 1) Global monitor: works even when your app is not key, but requires Accessibility permissions sometimes.
    globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]) { [weak self] _ in
      Task { @MainActor in self?.tickGlobalPointer(deliver: 0) }
    }

    // 2) Local monitor: always works while your app is active
    localMonitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]) { [weak self] ev in
      Task { @MainActor in self?.tickGlobalPointer(deliver: 0) }
      return ev
    }

    // 3) Fallback timer (in case monitors are throttled): 30 Hz
    timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
      Task { @MainActor in self?.tickGlobalPointer(deliver: 0) }
    }
  }

  func stop() {
    if let gm = globalMonitor { NSEvent.removeMonitor(gm) }
    if let lm = localMonitor { NSEvent.removeMonitor(lm) }
    globalMonitor = nil
    localMonitor = nil

    timer?.invalidate()
    timer = nil
  }

  private func tickGlobalPointer(deliver: UInt8) {
    guard activeXid != 0 else { return }

    // Global root coords (top-left) — use your existing helper if you put it somewhere shared
    let gp = NSEvent.mouseLocation
    let screens = NSScreen.screens
    let vminX = screens.map { $0.frame.minX }.min() ?? 0
    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0
    let screen = screens.first(where: { $0.frame.contains(gp) }) ?? NSScreen.main
    let scale = screen?.backingScaleFactor ?? 1.0

    let rootX = Int32(((gp.x - vminX) * scale).rounded(.toNearestOrAwayFromZero))
    let rootY = Int32(((vmaxY - gp.y) * scale).rounded(.toNearestOrAwayFromZero)) - 1

    // We can’t know window-local coords when pointer is outside; send last-known.
    let (winX, winY) = lastWinXY

    x11_post_pointer_move2(activeXid, winX, winY, rootX, max(0, rootY), deliver, 0, 0)
  }
}
