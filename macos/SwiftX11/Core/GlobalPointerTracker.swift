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

  // Last root position posted; a tick that lands on the same X11 point is
  // dropped.  xorg emits raw events only for actual motion, and the 30 Hz
  // timer used to stream RawMotion to every root selector while the mouse
  // sat still (v1.20.0.24).
  private var lastRootXY: (Int32, Int32)? = nil

  func updateActiveWindow(xid: UInt32, lastWinXY: (Int32, Int32)) {
    self.activeXid = xid
    self.lastWinXY = lastWinXY
  }

  func start() {
    stop()

    globalMonitor = NSEvent.addGlobalMonitorForEvents(
      matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]
    ) { [weak self] _ in
      DispatchQueue.main.async {
        self?.tickGlobalPointer(deliver: 0)
      }
    }

    localMonitor = NSEvent.addLocalMonitorForEvents(
      matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]
    ) { [weak self] ev in
      DispatchQueue.main.async {
        self?.tickGlobalPointer(deliver: 0)
      }
      return ev
    }

    timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
      DispatchQueue.main.async {
        self?.tickGlobalPointer(deliver: 0)
      }
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
    // No `activeXid != 0` guard any more (v1.20.0.24): XI2 RawMotion is a
    // root-level event that xorg delivers for every pointer move, whether or
    // not any X window has ever seen the pointer.  With the guard, xeyes
    // started on a fresh server got nothing until the pointer had crossed
    // some X11 window.  The bridge accepts xid 0 for these ticks; the server
    // sends RawMotion and stops there (no window to route to).

    // NSEvent.mouseLocation is in global screen coords in *points*.
    let gp = NSEvent.mouseLocation
    let screens = NSScreen.screens
    let vminX = screens.map { $0.frame.minX }.min() ?? 0
    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0

    // Convert to X11 root coords (top-left origin) in *points* (X11 units).
    let rootX = Int32((gp.x - vminX).rounded(.toNearestOrAwayFromZero))
    let rootY = max(0, Int32((vmaxY - gp.y).rounded(.toNearestOrAwayFromZero)) - 1)

    if let last = lastRootXY, last.0 == rootX, last.1 == rootY { return }
    lastRootXY = (rootX, rootY)

    // Window-local coords are also X11 units (points). Keep last-known.
    let (winX, winY) = lastWinXY

    // Buttons/modifiers: UInt32.max means "unknown — use the server's
    // canonical InputState".  Passing 0 fed empty button masks into
    // grab-routed XI2 motion at 30 Hz (M8 in docs/XI2_XORG_COMPARISON.md).
    x11_post_pointer_move2(activeXid, winX, winY, rootX, rootY, deliver, UInt32.max, UInt32.max)
  }
  
//  private func tickGlobalPointer(deliver: UInt8) {
//    guard activeXid != 0 else { return }
//
//    // Global root coords (top-left) — use your existing helper if you put it somewhere shared
//    let gp = NSEvent.mouseLocation
//    let screens = NSScreen.screens
//    let vminX = screens.map { $0.frame.minX }.min() ?? 0
//    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0
//    let screen = screens.first(where: { $0.frame.contains(gp) }) ?? NSScreen.main
//    let scale = screen?.backingScaleFactor ?? 1.0
//
//    let rootX = Int32(((gp.x - vminX) * scale).rounded(.toNearestOrAwayFromZero))
//    let rootY = Int32(((vmaxY - gp.y) * scale).rounded(.toNearestOrAwayFromZero)) - 1
//
//    // We can’t know window-local coords when pointer is outside; send last-known.
//    let (winX, winY) = lastWinXY
//
//    x11_post_pointer_move2(activeXid, winX, winY, rootX, max(0, rootY), deliver, 0, 0)
//  }
}
