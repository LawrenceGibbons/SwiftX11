//
//  WindowRegistry.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 12/31/25.
//

import AppKit
import QuartzCore
import X11LowLevel

/// Bounding rect of the region that changed since the last present.
/// Coordinates are in host-surface units (points).  `nil` means "full frame".
struct DamageRect {
  let x: Int
  let y: Int
  let w: Int
  let h: Int
}

struct X11WindowInfo {
  var xid: UInt32
  var parentXid: UInt32
  var x: Int = 0           // X11 root-relative position (for override-redirect positioning)
  var y: Int = 0
  var width: Int
  var height: Int
  var title: String
  var mapped: Bool
  var overrideRedirect: Bool = false
}

@MainActor
final class WindowRegistry {

  static let shared = WindowRegistry()

  // MARK: - Multi-monitor coordinate conversion

  /// Convert X11 root coordinates (top-left origin, y-down) to macOS global
  /// screen coordinates (bottom-left origin, y-up).  Uses the union bounding
  /// box of all connected screens so that windows on any monitor are placed
  /// correctly.  The inverse of rootPointInX11TopLeft() in X11WindowHost.swift.
  static func x11RootToMacOSOrigin(x11X: CGFloat, x11Y: CGFloat, height: CGFloat) -> NSPoint {
    let screens = NSScreen.screens
    let vminX = screens.map { $0.frame.minX }.min() ?? 0
    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0
    return NSPoint(x: x11X + vminX, y: vmaxY - x11Y - height)
  }

  /// Convert macOS global screen origin (bottom-left, y-up) back to X11 root
  /// coordinates (top-left, y-down).  Inverse of x11RootToMacOSOrigin().
  static func macOSOriginToX11Root(macOrigin: NSPoint, height: CGFloat) -> (Int32, Int32) {
    let screens = NSScreen.screens
    let vminX = screens.map { $0.frame.minX }.min() ?? 0
    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0
    let x11X = Int32((macOrigin.x - vminX).rounded(.toNearestOrAwayFromZero))
    let x11Y = Int32((vmaxY - macOrigin.y - height).rounded(.toNearestOrAwayFromZero))
    return (x11X, x11Y)
  }

  /// Adjust an OR (popup) window origin so it appears on the same macOS screen
  /// as the mouse cursor.  Clients that don't use RANDR (e.g. xterm) keep stale
  /// WidthOfScreen/HeightOfScreen from connection setup, so after a monitor
  /// hot-plug the popup X11 coords may be clipped to the old screen bounds and
  /// land on the wrong macOS monitor.
  ///
  /// Returns the (possibly adjusted) origin and whether an adjustment was made.
  /// When adjusted, the caller must also update the X11 WindowTable position
  /// so that input event coordinates are consistent.
  static func adjustOROriginForCursorScreen(
    origin: NSPoint, size: NSSize
  ) -> (origin: NSPoint, adjusted: Bool) {
    let screens = NSScreen.screens
    guard screens.count > 1 else { return (origin, false) }

    let mouseLocation = NSEvent.mouseLocation  // macOS global coords

    // Find which screen the mouse is on
    guard let cursorScreen = screens.first(where: { $0.frame.contains(mouseLocation) })
            ?? screens.first  // fallback to primary
    else { return (origin, false) }

    // Find which screen the popup center would land on
    let popupCenter = NSPoint(x: origin.x + size.width / 2,
                              y: origin.y + size.height / 2)
    let popupScreen = screens.first(where: { $0.frame.contains(popupCenter) })

    // If popup is on the same screen as cursor, no adjustment needed
    if let ps = popupScreen, ps === cursorScreen { return (origin, false) }

    // Popup is on a different screen (or off-screen entirely).
    // Place the popup near the cursor on its screen, clamped to screen bounds.
    // Prefer: cursor.x (left-aligned), cursor.y - height (above cursor).
    let cs = cursorScreen.visibleFrame
    var newX = mouseLocation.x
    var newY = mouseLocation.y - size.height

    // Clamp to cursor screen bounds
    if newX + size.width > cs.maxX { newX = cs.maxX - size.width }
    if newX < cs.minX { newX = cs.minX }
    if newY < cs.minY { newY = cs.minY }
    if newY + size.height > cs.maxY { newY = cs.maxY - size.height }

    print("[POPUP_ADJUST] cursor on screen \(cursorScreen.frame), " +
          "popup was at \(origin) (screen \(popupScreen?.frame.debugDescription ?? "none")), " +
          "adjusted to (\(newX), \(newY))")
    return (NSPoint(x: newX, y: newY), true)
  }

  /// Adjust a non-OR (normal) window origin so it appears on the main screen
  /// (the screen with the key window / menu bar) when the X11 client used the
  /// default position (0,0).  Most X11 clients (xterm, xeyes, xcalc) don't
  /// specify explicit geometry and create windows at (0,0), which maps to the
  /// top-left of the virtual desktop — often the laptop screen when an external
  /// monitor is primary.  A real WM would place new windows on the focused screen.
  ///
  /// Only adjusts when x11x==0 && x11y==0 (default position).  Windows with
  /// explicit positions (e.g. Vivado main window) are left unchanged.
  static func adjustNonOROriginForMainScreen(
    origin: NSPoint, size: NSSize, x11x: Int32, x11y: Int32
  ) -> (origin: NSPoint, adjusted: Bool) {
    let screens = NSScreen.screens
    guard screens.count > 1 else { return (origin, false) }

    // Only adjust default-position windows (client didn't specify geometry)
    guard x11x == 0 && x11y == 0 else { return (origin, false) }

    // Use NSScreen.main (screen with key window / menu bar) as target
    guard let mainScreen = NSScreen.main else { return (origin, false) }

    // Check if window center already lands on the main screen
    let center = NSPoint(x: origin.x + size.width / 2,
                         y: origin.y + size.height / 2)
    if mainScreen.frame.contains(center) { return (origin, false) }

    // Place window at the top-left of the main screen's visible area,
    // offset slightly (like a WM cascade placement).
    let vs = mainScreen.visibleFrame
    var newX = vs.minX + 40
    var newY = vs.maxY - size.height - 40  // below menu bar, accounting for title bar

    // Clamp to screen bounds
    if newX + size.width > vs.maxX { newX = vs.maxX - size.width }
    if newX < vs.minX { newX = vs.minX }
    if newY < vs.minY { newY = vs.minY }

    #if DEBUG
    print("[WM_PLACE] window at X11(0,0) adjusted from \(origin) to (\(newX), \(newY)) on main screen \(mainScreen.frame)")
    #endif
    return (NSPoint(x: newX, y: newY), true)
  }

  /// Virtual desktop max-Y in macOS global screen coordinates.
  /// Used by WarpPointer to convert macOS screen coords to CG coords.
  static var virtualDesktopMaxY: CGFloat {
    NSScreen.screens.map { $0.frame.maxY }.max() ?? 0
  }
  
  private var showDamageLogs: () -> Bool = { true }   // default

  // injected hooks (set once from SwiftUI/XServerController)
  private var logAppend: ((String) -> Void)?
  private var isLogPaused: (() -> Bool)?
  private var pendingPresentByXid: Set<UInt32> = []
  
  // xid relationship tracking
  private var infoByXid: [UInt32: X11WindowInfo] = [:]
  private let X11_ROOT: UInt32 = 0x00000001
  private var parentByXid: [UInt32: UInt32] = [:]
  private var childrenByParent: [UInt32: Set<UInt32>] = [:]   // optional, but useful
  private let rootXid: UInt32 = 1

  // avoid rootless_resize thrashing
  // For each xid, the size we just asked Cocoa to apply via CONFIGURE
  
  private var suppressExpectedSize: [UInt32: (w: Int32, h: Int32)] = [:]
  // During initial window construction/layout, Cocoa emits resize callbacks
  // (often 1x1 -> 2x2 jitter). We must NOT echo those back into X11 until
  // the X11 window is mapped/visible.
  private var ignoreCocoaResizeUntilMapped: Set<UInt32> = []

  // WindowRegistry.swift
  private var applyingX11Resize: Set<UInt32> = []

  // Budget of how many callbacks to suppress before giving up (layout can generate intermediate sizes)
  private var suppressBudget: [UInt32: Int] = [:]

  // resize related
  private var pendingX11Resize: [UInt32: (w: CGFloat, h: CGFloat)] = [:]
  private var resizeWorkScheduled: Set<UInt32> = []
  
  // When X11 drives a resize, swallow Cocoa resize callbacks until we converge.
  private var suppressCocoaResizeExpected: [UInt32: (wX11: Int32, hX11: Int32)] = [:]
  private var suppressCocoaResizeBudget:   [UInt32: Int] = [:]
  // xxx temp
  private var lastSizePrintTimeByXid: [UInt32: CFTimeInterval] = [:]
  
  // Debug: enable verbose snapshot diagnostics
  var debugSnapshotRouting: Bool = false
  
  private func isTopLevelX11Window(_ xid: UInt32) -> Bool {
    // Top-level means parent is the X11 root (1). If unknown, assume top-level
    // (defensive; better to show a window than hide it).
    if let info = infoByXid[xid] { return info.parentXid == rootXid }
    if let p = parentByXid[xid] { return p == rootXid }
    return true
  }
  
  func isMapped(xid: UInt32) -> Bool {
    mappedXids.contains(xid)
  }

  func isOverrideRedirect(xid: UInt32) -> Bool {
    infoByXid[xid]?.overrideRedirect ?? false
  }

  func setSettingsHooks(showDamageLogs: @escaping () -> Bool) {
    self.showDamageLogs = showDamageLogs
  }
  
  @MainActor
  private func hostXid(_ xid: UInt32) -> UInt32 {
    return topLevelAncestor(of: xid)
  }
    
  // call this once at startup
  func attachLogHooks(
    logAppend: @escaping (String) -> Void,
    isLogPaused: @escaping () -> Bool
  ) {
    self.logAppend = logAppend
    self.isLogPaused = isLogPaused
  }
  
  private init() {}
  
  private var windows: [UInt32: X11WindowController] = [:]
  private var repaintWorkItemByXid: [UInt32: DispatchWorkItem] = [:]
  private var latestHostSizePxByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  private var latestHostSizePtByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  // Tracks the last host size we actually *sent* to X11 via Configure.
  // This is distinct from the latest observed Cocoa sizes.
  private var lastSentHostSizePxByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  private var lastSentHostSizePtByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  private var lastRepaintTimeByXid: [UInt32: CFTimeInterval] = [:]
  private var windowObserversByXid: [UInt32: [NSObjectProtocol]] = [:]
  private var suppressNextRaiseFromCocoa: Set<UInt32> = []
  private var suppressNextResizeFromCocoa: Set<UInt32> = []
  private var suppressNextMapFromCocoa: Set<UInt32> = []
  private var suppressNextUnmapFromCocoa: Set<UInt32> = []
  private var closingXids = Set<UInt32>()
  private var mappedXids = Set<UInt32>()
  // Override-redirect windows whose mapWindow() deferred orderFront
  // because the geometry wasn't available yet. moveWindow() will show them.
  private var pendingORShow = Set<UInt32>()

  // Metal is now required — no software fallback.
  
  private func installWindowObservers(xid: UInt32, window: NSWindow) {
    removeWindowObservers(xid: xid)
    
    let center = NotificationCenter.default
    
    var toks: [NSObjectProtocol] = []
    
    let didBecomeKey = center.addObserver(
      forName: NSWindow.didBecomeKeyNotification,
      object: window,
      queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      MainActor.assumeIsolated {
        assert(Thread.isMainThread)
        
        // Prevent feedback loop when raise came from X11 -> Swift.
        if self.suppressNextRaiseFromCocoa.contains(xid) {
          self.suppressNextRaiseFromCocoa.remove(xid)
          return
        }
        
        // Cocoa -> X11
        DispatchQueue.main.async {
          x11_post_focus_event(xid, true)
          x11_post_window_raise(xid)
        }
      }
    }
    toks.append(didBecomeKey)
    
    let didResignKey = center.addObserver(
      forName: NSWindow.didResignKeyNotification,
      object: window,
      queue: .main
    ) { _ in
      MainActor.assumeIsolated {
        x11_post_focus_event(xid, false)
      }
    }
    toks.append(didResignKey)
    
    let didMini = center.addObserver(
      forName: NSWindow.didMiniaturizeNotification, object: window, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      MainActor.assumeIsolated {
        if self.consumeSuppressUnmapFromCocoa(xid: xid) { return }
        DispatchQueue.main.async {
          x11_post_window_unmap(xid)
        }
      }
    }
    toks.append(didMini)
    
    let didDeMini = center.addObserver(
      forName: NSWindow.didDeminiaturizeNotification, object: window, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      MainActor.assumeIsolated {
        if self.consumeSuppressMapFromCocoa(xid: xid) { return }
        DispatchQueue.main.async {
          x11_post_window_map(xid)
        }
      }
    }
    toks.append(didDeMini)
    
    windowObserversByXid[xid] = toks
  }
  
  private func removeWindowObservers(xid: UInt32) {
    guard let toks = windowObserversByXid.removeValue(forKey: xid) else { return }
    let center = NotificationCenter.default
    for t in toks { center.removeObserver(t) }
  }
  
  func mapWindow(xid: UInt32) {
    if var info = infoByXid[xid] {
      info.mapped = true
      infoByXid[xid] = info
    }

    guard isTopLevelX11Window(xid) else {
      schedulePresent(xid: xid)
      return
    }

    let host = topLevelAncestor(of: xid)

    guard !closingXids.contains(host) else { return }
    guard let controller = windows[host],
          let win = controller.window else { return }

    // Now that X11 says mapped, allow Cocoa resizes to flow back to X11.
    ignoreCocoaResizeUntilMapped.remove(host)

    if X11Trace.lifecycle { logAppend?("[MAP] host=0x\(String(host, radix:16)) mapped; awaiting X11_UI_RESIZE") }
    
    mappedXids.insert(host)

    X11View.logIfInLayout("mapWindow: makeKeyAndOrderFront host=0x\(String(host, radix: 16))",
                          view: controller.x11View)

    suppressNextMapFromCocoa.insert(host)

    // Override-redirect windows (menus/tooltips) should not steal keyboard focus.
    // Use orderFront instead of makeKeyAndOrderFront for these windows.
    // Also reposition to current X11 geometry (may have been updated by ConfigureWindow
    // between CreateWindow and MapWindow — e.g., Xt popup menus).
    if infoByXid[host]?.overrideRedirect == true {
      // Query current X11 position from WindowTable (may differ from creation position)
      var x11x: Int32 = 0, x11y: Int32 = 0, x11w: Int32 = 0, x11h: Int32 = 0
      var isOR: Bool = false
      if x11_get_window_geometry(host, &x11x, &x11y, &x11w, &x11h, &isOR) {
        // No size floor — use actual X11 geometry.
        let rawOrigin = WindowRegistry.x11RootToMacOSOrigin(
          x11X: CGFloat(x11x), x11Y: CGFloat(x11y), height: CGFloat(x11h))
        let popupSize = NSSize(width: max(1, CGFloat(x11w)), height: max(1, CGFloat(x11h)))
        let (origin, adjusted) = WindowRegistry.adjustOROriginForCursorScreen(
          origin: rawOrigin, size: popupSize)
        if adjusted {
          // Sync X11 WindowTable so input event coordinates match the adjusted position
          let (newX11X, newX11Y) = WindowRegistry.macOSOriginToX11Root(
            macOrigin: origin, height: popupSize.height)
          x11_set_window_position(host, newX11X, newX11Y)
        }
        let frame = NSRect(origin: origin, size: popupSize)
        print("[POPUP_MAP] xid=0x\(String(format:"%X", host)) x11=(\(x11x),\(x11y),\(x11w)x\(x11h)) macFrame=\(frame) adjusted=\(adjusted)")
        win.setFrame(frame, display: true)
        // Defer orderFront until the first present succeeds so the popup
        // doesn't flash blank.  Metal needs at least one frame uploaded before
        // the window becomes visible.  snapshotAndPresentNow will call
        // orderFront via pendingORShow once content is ready.
        pendingORShow.insert(host)
        // Safety: force-show after 150ms in case present never arrives
        // (defensive against edge cases like drawable creation failure).
        let hostCopy = host
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
          guard let self else { return }
          if self.pendingORShow.remove(hostCopy) != nil {
            print("[POPUP_SAFETY] xid=0x\(String(format:"%X", hostCopy)) force-showing after 150ms timeout")
            self.windows[hostCopy]?.window?.orderFront(nil)
          }
        }
      } else {
        print("[POPUP_MAP] xid=0x\(String(format:"%X", host)) geometry query FAILED — deferring show to moveWindow")
        // Don't show yet. The X11_UI_MOVE command (pushed by MapWindow handler)
        // will position and show the window with correct coordinates.
        pendingORShow.insert(host)
      }
    } else {
      // Sync NSWindow size and position to current X11 geometry BEFORE showing.
      // WM_NORMAL_HINTS may have resized/repositioned the window via UI commands
      // that are deferred (DispatchQueue.main.asyncAfter in applyX11Resize).
      // The C++ MAP_HINTS code may also have resized from WM_NORMAL_HINTS at
      // map time (emulating quartz-wm MapRequest behavior).
      let hostCopy = host
      var x11x: Int32 = 0, x11y: Int32 = 0, x11w: Int32 = 0, x11h: Int32 = 0
      var isOR: Bool = false
      let gotGeom = x11_get_window_geometry(host, &x11x, &x11y, &x11w, &x11h, &isOR)

      #if DEBUG
      print("[MAP_SHOW] xid=0x\(String(format:"%X", host)) geom=\(x11w)×\(x11h)")
      #endif

      // WM_HINTS IconicState: client wants this window to start minimized.
      // Don't show or miniaturize — miniaturize(nil) on a non-visible window
      // causes macOS to briefly show it with a minimize animation (the "sliding
      // window" glitch).  Just leave it hidden; the client thinks it's mapped
      // (MapNotify was sent) but the NSWindow stays off-screen.
      if pendingIconicState.remove(hostCopy) != nil {
        #if DEBUG
        print("[MAP_ICONIC] xid=0x\(String(format:"%X", hostCopy)) IconicState — keeping hidden")
        #endif
        // Window stays hidden — no orderFront, no miniaturize
      } else {
        syncAndShowNonORWindow(host: hostCopy, x11x: x11x, x11y: x11y, x11w: x11w, x11h: x11h)
      }
    }

    // For OR windows, skip the initial present — the surface is blank at this point
    // because the client hasn't received Expose yet.  The damage-triggered present
    // (after client draws) will show content AND reveal the window via pendingORShow.
    // For non-OR windows, present immediately to show the window contents.
    if infoByXid[host]?.overrideRedirect != true {
      schedulePresent(xid: host)
    }
  }

  /// Sync NSWindow to current X11 geometry and show it (non-OR path).
  /// Called either immediately (if geometry is known) or after a 50ms defer.
  private func showNonORWindow(host: UInt32) {
    guard let controller = windows[host], let win = controller.window else { return }
    var x11x: Int32 = 0, x11y: Int32 = 0, x11w: Int32 = 0, x11h: Int32 = 0
    var isOR: Bool = false
    if x11_get_window_geometry(host, &x11x, &x11y, &x11w, &x11h, &isOR) {
      syncAndShowNonORWindow(host: host, x11x: x11x, x11y: x11y, x11w: x11w, x11h: x11h)
    } else {
      // Fallback: show at whatever size the NSWindow has
      NSApp.activate(ignoringOtherApps: true)
      win.makeKeyAndOrderFront(nil)
    }
  }

  /// Apply X11 geometry to NSWindow and make it visible.
  private func syncAndShowNonORWindow(host: UInt32, x11x: Int32, x11y: Int32, x11w: Int32, x11h: Int32) {
    guard let controller = windows[host], let win = controller.window else { return }

    let newSize = NSSize(width: max(1, CGFloat(x11w)), height: max(1, CGFloat(x11h)))
    let curSize = win.contentView?.frame.size ?? win.contentLayoutRect.size
    if abs(curSize.width - newSize.width) > 1 || abs(curSize.height - newSize.height) > 1 {
      suppressCocoaResizeExpected[host] = (wX11: x11w, hX11: x11h)
      suppressCocoaResizeBudget[host] = 12
      suppressNextResizeFromCocoa.insert(host)
      win.setContentSize(newSize)
    }
    // Apply position: convert X11 root coords to macOS screen coords
    let rawOrigin = WindowRegistry.x11RootToMacOSOrigin(
      x11X: CGFloat(x11x), x11Y: CGFloat(x11y), height: newSize.height)
    // For default-position windows (0,0), place on the main screen instead
    // of the top-left of the virtual desktop (which may be the laptop screen).
    let (origin, adjusted) = WindowRegistry.adjustNonOROriginForMainScreen(
      origin: rawOrigin, size: newSize, x11x: x11x, x11y: x11y)
    if adjusted {
      let (newX11X, newX11Y) = WindowRegistry.macOSOriginToX11Root(
        macOrigin: origin, height: newSize.height)
      x11_set_window_position(host, newX11X, newX11Y)
    }
    win.setFrameOrigin(origin)

    NSApp.activate(ignoringOtherApps: true)
    win.makeKeyAndOrderFront(nil)
    // Sync NSWindow's actual screen position back to X11 WindowTable.
    let hostCopy = host
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      guard let controller = self.windows[hostCopy],
            let win = controller.window else { return }
      let contentFrame = win.contentView?.frame ?? win.contentLayoutRect
      let (x11X, x11Y) = WindowRegistry.macOSOriginToX11Root(
        macOrigin: win.frame.origin, height: contentFrame.size.height)
      x11_set_window_position(hostCopy, x11X, x11Y)
    }
  }

  @MainActor
  func viewForHostXid(_ hostXid: UInt32) -> X11View? {
    windows[hostXid]?.x11View
  }
  

  @MainActor
  func viewForAnyXid(_ anyXid: UInt32) -> X11View? {
    return windows[anyXid]?.x11View
  }
  
  func unmapWindow(xid: UInt32) {
    let host = topLevelAncestor(of: xid)

    // Track unmapped state for the specific X window (best-effort).
    if var info = infoByXid[xid] {
      info.mapped = false
      infoByXid[xid] = info
    }

    // Only hide a Cocoa window if this is the top-level X window.
    guard isTopLevelX11Window(host) else { return }
    guard let controller = windows[host] else { return }

    // Mark unmapped so future damage does not schedule presents.
    mappedXids.remove(host)

    // Cancel anything already scheduled/coalesced for presentation.
    pendingPresentByXid.remove(host)
    repaintWorkItemByXid[host]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: host)

    X11View.logIfInLayout("unmapWindow: orderOut host=0x\(String(host, radix: 16))", view: controller.x11View)

    pendingORShow.remove(host)
    suppressNextUnmapFromCocoa.insert(host)
    controller.window?.orderOut(nil)
  }
  
  @MainActor
  func noteX11WindowDestroyed(xid: UInt32) {
    // 0) Cancel any scheduled present/snapshot bookkeeping for this xid as either host or source.
    pendingPresentByXid.remove(xid)
    pendingPresentByHost.remove(xid)
    latestSourceByHost.removeValue(forKey: xid)
    ignoreCocoaResizeUntilMapped.remove(xid)
    pendingORShow.remove(xid)
    // If this xid was a *source* for some host, clear that too.
    // (Safe O(n), n is small.)
    for (host, src) in latestSourceByHost where src == xid {
      latestSourceByHost.removeValue(forKey: host)
      pendingPresentByXid.remove(host)
    }

    // 1) Clear “poison” / visibility sets so XID reuse works.
    closingXids.remove(xid)
    mappedXids.remove(xid)

    // 2) Cancel resize/debounce workitems keyed by this xid.
    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: xid)
    latestHostSizePxByXid.removeValue(forKey: xid)
    latestHostSizePtByXid.removeValue(forKey: xid)
    lastRepaintTimeByXid.removeValue(forKey: xid)

    // 3) Remove hierarchy bookkeeping (child link + child set).
    if let p = parentByXid.removeValue(forKey: xid) {
      childrenByParent[p]?.remove(xid)
      if childrenByParent[p]?.isEmpty == true { childrenByParent.removeValue(forKey: p) }
    }
    childrenByParent.removeValue(forKey: xid)
    infoByXid.removeValue(forKey: xid)

    // 4) If it’s a top-level Cocoa host, actually close the NSWindow.
    if isTopLevelX11Window(xid) {
      // IMPORTANT: closeWindow currently inserts into closingXids; we just removed it above,
      // but closeWindow may re-insert. Fix closeWindow (see below).
      closeWindow(xid: xid)

      // If closeWindow re-added poison flags, remove again.
      closingXids.remove(xid)
      mappedXids.remove(xid)
      pendingPresentByXid.remove(xid)
      latestSourceByHost.removeValue(forKey: xid)
    }
  }
  
  
  @MainActor
  func noteX11WindowCreated(xid: UInt32,
                            parentXid: UInt32,
                            title: String,
                            x: Int = 0,
                            y: Int = 0,
                            width: Int,
                            height: Int,
                            overrideRedirect: Bool = false)
  {
    let xids = String(format: "0x%X", xid)
    let parent_xids = String(format: "0x%X", parentXid)

    if overrideRedirect {
      print("[OR_CREATE] xid=\(xids) parent=\(parent_xids) \(width)x\(height) @(\(x),\(y)) override_redirect=true")
    }
    if X11Trace.lifecycle { logAppend?("noteX11WindowCreated: xid=\(xids), parent=\(parent_xids), \(width)x\(height) @(\(x),\(y)) or=\(overrideRedirect)") }
    // Update/insert metadata (idempotent).
    infoByXid[xid] = X11WindowInfo(
      xid: xid,
      parentXid: parentXid,
      x: x,
      y: y,
      width: width,
      height: height,
      title: title,
      mapped: false,
      overrideRedirect: overrideRedirect
    )

    // Track hierarchy: parent relationship is required for topLevelAncestor() / presentation routing.
    parentByXid[xid] = parentXid
    // Track hierarchy (optional, but helps debugging and future correctness)
    noteSubwindow(childXid: xid, parentXid: parentXid)

    // Only create Cocoa windows for top-level windows.
    if parentXid == rootXid {
      // Top-level host: ignore Cocoa resize callbacks until X11 maps it.
      ignoreCocoaResizeUntilMapped.insert(xid)
      ensureControllerForTopLevel(xid: xid)
    } else {
      // No Cocoa window. Child windows should later be composited into their top-level,
      // but for now we’re at least preventing double-window creation and XID confusion.
    }
  }
  
  
  @MainActor
  func noteSubwindow(childXid: UInt32, parentXid: UInt32) {
    // Ensure parent relationship is recorded even if we hear about the child later.
    parentByXid[childXid] = parentXid

    // Track direct parent->children (useful for debugging and future composition).
    childrenByParent[parentXid, default: []].insert(childXid)
  }
  
  
  @MainActor
  private func ensureControllerForTopLevel(xid: UInt32) {
    if windows[xid] != nil { return }
    guard let info = infoByXid[xid] else { return }

    // C++ CreateWindow applies a size floor for tiny windows (< 10px → 200×100).
    // Floored windows are deferred in mapWindow — not shown until the client
    // sends ConfigureWindow/WM_NORMAL_HINTS with the real size.
    let controller = X11WindowController(
      xid: xid,
      title: info.title,
      x: info.x,
      y: info.y,
      width: info.width,
      height: info.height,
      overrideRedirect: info.overrideRedirect
    )
    ignoreCocoaResizeUntilMapped.insert(xid)

    controller.logAppend = { [weak self] line in
      guard let self else { return }
      guard self.isLogPaused?() != true else { return }
      self.logAppend?(line)
    }
    windows[xid] = controller
    controller.window?.orderOut(nil) // start hidden/unmapped

    if let win = controller.window {
      installWindowObservers(xid: xid, window: win)
    }
  }
  
  
  
  func closeWindow(xid: UInt32) {
    
    // If a child is being destroyed, DO NOT close the Cocoa host window.
    guard isTopLevelX11Window(xid) else {
      // child teardown: remove bookkeeping only
      pendingPresentByXid.remove(topLevelAncestor(of: xid))
      pendingPresentByHost.remove(xid)
      // if you store parent/child mappings, remove those here too
      return
    }


    let host = xid

    removeWindowObservers(xid: host)
    suppressNextRaiseFromCocoa.remove(host)
    suppressNextResizeFromCocoa.remove(host)
    suppressNextMapFromCocoa.remove(host)
    suppressNextUnmapFromCocoa.remove(host)

    repaintWorkItemByXid[host]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: host)

    pendingPresentByXid.remove(host)
    mappedXids.remove(host)

    latestHostSizePxByXid.removeValue(forKey: host)
    latestHostSizePtByXid.removeValue(forKey: host)
    lastRepaintTimeByXid.removeValue(forKey: host)

    closingXids.insert(host)
    defer { closingXids.remove(host) }

    guard let controller = windows.removeValue(forKey: host) else { return }
    X11View.logIfInLayout("destroy: controller.close xid=0x\(String(host, radix: 16))", view: controller.x11View)
    controller.close()
  }
  
  
  func presentFrame(xid: UInt32, bgra: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    guard let controller = windows[xid], let view = controller.x11View else { return }
    view.presentBGRA(framebuffer: bgra, width: width, height: height, bytesPerRow: bytesPerRow)
  }
      

  
  @MainActor
  private func scanBGRA(_ data: Data, width: Int, height: Int, bytesPerRow: Int) -> (nonwhite: Int, samples: [UInt32]) {
    guard width > 0, height > 0, bytesPerRow >= width * 4, data.count >= bytesPerRow * height else {
      return (0, [])
    }

    // Sample a few locations (BGRA little-endian -> UInt32)
    func px(atX x: Int, _ y: Int) -> UInt32 {
      let off = y * bytesPerRow + x * 4
      return data.withUnsafeBytes { raw in
        raw.load(fromByteOffset: off, as: UInt32.self)
      }
    }

    let sx = [width / 4, width / 2, (width * 3) / 4].map { max(0, min(width - 1, $0)) }
    let sy = max(0, min(height - 1, height / 2))

    let samples: [UInt32] = sx.map { px(atX: $0, sy) }

    // Count nonwhite (BGRA 0xFFFFFFFF is white in your logs)
    var nonwhite = 0
    data.withUnsafeBytes { raw in
      let p = raw.bindMemory(to: UInt32.self)
      let nWords = (bytesPerRow * height) / 4
      for i in 0..<nWords {
        if p[i] != 0xFFFFFFFF { nonwhite += 1 }
      }
    }
    return (nonwhite, samples)
  }
  
  
  /// If this xid is an OR (popup) window waiting to be shown, show it now.
  /// Called after a successful presentBGRA so the window becomes visible
  /// only after Metal has content to display — eliminates blank popup flash.
  ///
  /// As an extra safety check, scans the presented data for non-white pixels.
  /// If the surface is all white (0xFFFFFFFF), the client hasn't drawn yet and
  /// we defer showing to the next present cycle.
  private func showPendingORWindowIfNeeded(xid: UInt32, data: Data, width: Int, height: Int, bytesPerRow: Int) {
    guard pendingORShow.contains(xid) else { return }

    // Quick scan: check a few sample locations for non-white pixels.
    // White = 0xFFFFFFFF (BGRA opaque white). If ALL samples are white,
    // the client likely hasn't drawn yet — keep the window hidden.
    let hasContent: Bool = data.withUnsafeBytes { raw in
      let p = raw.bindMemory(to: UInt32.self)
      let stride = bytesPerRow / 4
      guard stride > 0, height > 0 else { return false }
      // Sample center, quarters, and a few more locations
      let sampleYs = [height / 4, height / 2, (height * 3) / 4]
      let sampleXs = [width / 4, width / 2, (width * 3) / 4]
      for sy in sampleYs {
        for sx in sampleXs {
          let idx = sy * stride + sx
          if idx < p.count && p[idx] != 0xFFFFFFFF {
            return true
          }
        }
      }
      return false
    }

    if hasContent {
      pendingORShow.remove(xid)
      #if DEBUG
      print("[POPUP_SHOW] xid=0x\(String(format:"%X", xid)) showing after present with content")
      #endif
      windows[xid]?.window?.orderFront(nil)
    } else {
      #if DEBUG
      print("[POPUP_DEFER] xid=0x\(String(format:"%X", xid)) surface still blank — deferring show")
      #endif
    }
  }

  private func snapshotAndPresentNow(sourceXid: UInt32, presentXid: UInt32, damageRect: DamageRect? = nil) {
    guard let controller = windows[presentXid] else { return }
    guard !closingXids.contains(presentXid) else { return }

    // If the view has a retained display frame (from a recent resize), present
    // that instead of copying from the C++ drawing surface.  The display frame
    // is the last complete frame before reallocation — no white strips.
    // EXCEPTION: shaped windows — the retained frame is at the old size and
    // the shape mask won't match, causing distorted transparency during resize.
    // Better to let the client redraw at the new size immediately.
    if let view = controller.x11View,
       !view.isShaped,
       let df = view.retainedDisplayFrame() {
      // Force full upload (damage rect is stale for the display frame)
      presentBGRA(xid: presentXid, data: df.data,
                  width: df.width, height: df.height, bytesPerRow: df.bytesPerRow,
                  damageRect: nil)
      showPendingORWindowIfNeeded(xid: presentXid, data: df.data,
                                  width: df.width, height: df.height, bytesPerRow: df.bytesPerRow)
      return
    }

    func querySize() -> (w: Int32, h: Int32, bpr: Int32)? {
      var w: Int32 = 0, h: Int32 = 0, bpr: Int32 = 0
      _ = x11_server_copy_window_bgra(sourceXid, nil, 0, &w, &h, &bpr)
      guard w > 0, h > 0, bpr > 0 else { return nil }
      return (w, h, bpr)
    }

    guard var sz = querySize() else { return }

    func copyFrame(sz: (w: Int32, h: Int32, bpr: Int32)) -> (data: Data, ok: Bool) {
      let byteCount = Int(sz.bpr) * Int(sz.h)
      if byteCount <= 0 { return (Data(), false) }

      var data = Data(count: byteCount)
      var outW = sz.w, outH = sz.h, outBpr = sz.bpr
      var okCopy: Int32 = 0

      data.withUnsafeMutableBytes { raw in
        guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
        okCopy = x11_server_copy_window_bgra(sourceXid, base, Int32(byteCount), &outW, &outH, &outBpr)
      }

      let ok = (okCopy != 0 && outW == sz.w && outH == sz.h && outBpr == sz.bpr)
      return (data, ok)
    }

    if X11Trace.present { logAppend?("[SNAP_Q] host=0x\(String(presentXid,radix:16)) source=0x\(String(sourceXid,radix:16)) sz=\(sz.w)x\(sz.h) bpr=\(sz.bpr)") }
    let a1 = copyFrame(sz: sz)
    if !a1.ok {
      guard let newSz = querySize() else { return }
      sz = newSz
      let a2 = copyFrame(sz: sz)
      guard a2.ok else { return }

      let dataToPresent = a2.data
      let scan = scanBGRA(dataToPresent, width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
      if debugSnapshotRouting {
        let samplesStr = scan.samples
          .map { String(format: "0x%08X", $0) }
          .joined(separator: ",")

        logAppend?(
          "[SNAP_DBG] attempt 2 routing host=0x\(String(presentXid, radix:16)) " +
          "source=0x\(String(sourceXid, radix:16)) " +
          "size=\(sz.w)x\(sz.h) bpr=\(sz.bpr) " +
          "nonwhite=\(scan.nonwhite) " +
          "samples=\(samplesStr)"
        )
      }

      // Size changed mid-copy → force full upload (damage rect is stale)
      presentBGRA(xid: presentXid, data: dataToPresent,
                  width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr),
                  damageRect: nil)
      showPendingORWindowIfNeeded(xid: presentXid, data: dataToPresent,
                                  width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
      return
    }

    let dataToPresent = a1.data
    let scan = scanBGRA(dataToPresent, width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
    // Verbose OR_SNAP trace removed (was diagnostic-only for v1.10.4–v1.10.7).
    if debugSnapshotRouting {
      let samplesStr = scan.samples
        .map { String(format: "0x%08X", $0) }
        .joined(separator: ",")

      logAppend?(
        "[SNAP_DBG] attempt 1 routing host=0x\(String(presentXid, radix:16)) " +
        "source=0x\(String(sourceXid, radix:16)) " +
        "size=\(sz.w)x\(sz.h) bpr=\(sz.bpr) " +
        "nonwhite=\(scan.nonwhite) " +
        "samples=\(samplesStr)"
      )
    }
    presentBGRA(xid: presentXid, data: dataToPresent,
                width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr),
                damageRect: damageRect)
    showPendingORWindowIfNeeded(xid: presentXid, data: dataToPresent,
                                width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
  }


  private func snapshotAndPresentNow(xid: UInt32) {
    guard windows[xid] != nil else { return }
    guard !closingXids.contains(xid) else { return }
    
    // 1) Query the current image geometry
    func querySize() -> (w: Int32, h: Int32, bpr: Int32)? {
      var w: Int32 = 0, h: Int32 = 0, bpr: Int32 = 0
      _ = x11_server_copy_window_bgra(xid, nil, 0, &w, &h, &bpr)
      guard w > 0, h > 0, bpr > 0 else { return nil }
      return (w, h, bpr)
    }
    
    guard var sz = querySize() else { return }
    
    // Helper does *one* copy attempt into fresh Data of the right size.
    // IMPORTANT: no logging inside withUnsafeMutableBytes.
    func copyFrame(sz: (w: Int32, h: Int32, bpr: Int32)) -> (data: Data, ok: Bool, outW: Int32, outH: Int32, outBpr: Int32, okCopy: Int32) {
      let byteCount = Int(sz.bpr) * Int(sz.h)
      if byteCount <= 0 {
        return (Data(), false, 0, 0, 0, 0)
      }
      
      var data = Data(count: byteCount)
      
      var outW: Int32 = sz.w
      var outH: Int32 = sz.h
      var outBpr: Int32 = sz.bpr
      var okCopy: Int32 = 0
      
      // Do NOT call logAppend/self/server inside this closure.
      data.withUnsafeMutableBytes { raw in
        guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
          okCopy = 0
          return
        }
        okCopy = x11_server_copy_window_bgra(xid, base, Int32(byteCount), &outW, &outH, &outBpr)
      }
      
      // ok iff copy succeeded AND size didn’t change out from under us
      let ok = (okCopy != 0 && outW == sz.w && outH == sz.h && outBpr == sz.bpr)
      return (data, ok, outW, outH, outBpr, okCopy)
    }
    
    // 2) First attempt
    let attempt1 = copyFrame(sz: sz)
    if X11Trace.present { logAppend?("snap(copy#1): xid=0x\(String(xid, radix: 16)) okCopy=\(attempt1.okCopy) ok=\(attempt1.ok) outW=\(attempt1.outW) outH=\(attempt1.outH) outBpr=\(attempt1.outBpr) expect=\(sz.w)x\(sz.h) bpr=\(sz.bpr)") }
    
    // 3) If size changed mid-copy, retry once after re-query
    if !attempt1.ok {
      guard let newSz = querySize() else { return }
      sz = newSz
      
      let attempt2 = copyFrame(sz: sz)
      if X11Trace.present { logAppend?("snap(copy#2): xid=0x\(String(xid, radix: 16)) okCopy=\(attempt2.okCopy) ok=\(attempt2.ok) outW=\(attempt2.outW) outH=\(attempt2.outH) outBpr=\(attempt2.outBpr) expect=\(sz.w)x\(sz.h) bpr=\(sz.bpr)") }
      
      guard attempt2.ok else {
        if X11Trace.present { logAppend?("snap: EARLY RETURN (retry failed)") }
        return
      }
      
      presentBGRA(xid: xid,
                  data: attempt2.data,
                  width: Int(sz.w),
                  height: Int(sz.h),
                  bytesPerRow: Int(sz.bpr))
      return
    }
    
    // 4) Present
    presentBGRA(xid: xid,
                data: attempt1.data,
                width: Int(sz.w),
                height: Int(sz.h),
                bytesPerRow: Int(sz.bpr))
  }
  
  
  func setTitle(xid: UInt32, title: String) {
    guard let controller = windows[xid] else { return }
    controller.window?.title = title
    infoByXid[xid]?.title = title
  }
  
  func raiseWindow(xid: UInt32) {
    let host = topLevelAncestor(of: xid)

    guard let controller = windows[xid], let win = controller.window else { return }

    X11View.logIfInLayout("raiseWindow: makeKeyAndOrderFront host=0x\(String(host, radix: 16))", view: controller.x11View)

    // Suppress the next didBecomeKey notification since we're causing it.
    suppressNextRaiseFromCocoa.insert(xid)
    if X11Trace.lifecycle { print("[MAKEKEY] about to makeKeyAndOrderFront window=\(String(describing: win))") }

    NSApp.activate(ignoringOtherApps: true)
    win.makeKeyAndOrderFront(nil)
  }

  func moveWindow(xid: UInt32, x11X: Int, x11Y: Int) {
    guard let controller = windows[xid], let win = controller.window else { return }

    // Query fresh geometry for the height (needed for Y conversion)
    var x11w: Int32 = 0, x11h: Int32 = 0
    var dummy1: Int32 = 0, dummy2: Int32 = 0
    var dummyOR: Bool = false
    if x11_get_window_geometry(xid, &dummy1, &dummy2, &x11w, &x11h, &dummyOR) {
      // Convert X11 root coords (y-down) to macOS screen coords (y-up)
      // Uses virtual desktop union bounds so windows on any monitor work correctly.
      let rawOrigin = WindowRegistry.x11RootToMacOSOrigin(
        x11X: CGFloat(x11X), x11Y: CGFloat(x11Y), height: CGFloat(x11h))
      // For OR (popup) windows, adjust position if cursor is on a different screen
      // (handles clients with stale screen dimensions after monitor hot-plug)
      let isOR = infoByXid[xid]?.overrideRedirect ?? false
      let origin: NSPoint
      if isOR {
        let popupSize = NSSize(width: CGFloat(x11w), height: CGFloat(x11h))
        let (adj, adjusted) = WindowRegistry.adjustOROriginForCursorScreen(
          origin: rawOrigin, size: popupSize)
        origin = adj
        if adjusted {
          // Sync X11 WindowTable so input event coordinates match the adjusted position
          let (newX11X, newX11Y) = WindowRegistry.macOSOriginToX11Root(
            macOrigin: origin, height: popupSize.height)
          x11_set_window_position(xid, newX11X, newX11Y)
        }
      } else {
        origin = rawOrigin
      }
      win.setFrameOrigin(origin)

      // Position updated. Don't show the window here — let
      // snapshotAndPresentNow reveal it via pendingORShow once the
      // client has drawn actual content (prevents blank popup flash).
      if pendingORShow.contains(xid) {
        print("[POPUP_MOVE] xid=0x\(String(format:"%X", xid)) position updated to (\(x11X),\(x11Y)) — still waiting for content")
      }
    }
  }

  private func sendConfigureAsync(xid: UInt32, w: Int32, h: Int32) {
    // Normalize to the top-level host. Configure/resize only applies to Cocoa host windows.
    let host = hostXid(xid)

    if debugSnapshotRouting, host != xid {
      logAppend?("[SIZE] normalize sendConfigureAsync child=0x\(String(xid, radix:16)) -> host=0x\(String(host, radix:16))")
    }

    let w = max(1, w)
    let h = max(1, h)

    let scale = windows[host]?.window?.backingScaleFactor ?? -1
    if X11Trace.resize { logAppend?("[CFG_SEND] host=0x\(String(host,radix:16)) sendX11=\(w)x\(h)pt winScale=\(scale)") }
    
    // Record the size we are *actually sending* to X11 for the HOST.
    lastSentHostSizePtByXid[host] = (w: w, h: h)

    // Suppress Cocoa echo for the HOST (these are keyed by the window id we resize).
    suppressExpectedSize[host] = (w: w, h: h)
    suppressBudget[host] = 8   // swallow a few intermediate callbacks

    DispatchQueue.main.async {
      x11_post_window_resize(host, w, h)
    }
  }
  
  func windowResized(xid: UInt32, sizePoints: CGSize, sizePixels: CGSize, scale _: CGFloat) {

    let host = topLevelAncestor(of: xid)
    
    // Ignore bogus transient sizes (prevents Metal 0-height)
    guard sizePoints.width >= 1, sizePoints.height >= 1 else { return }
    guard sizePixels.width >= 1, sizePixels.height >= 1 else { return }

    
    // Logical (points) — ok to store for UI/debug if you want
    let wPt = Int32(max(1, Int(sizePoints.width.rounded(.down))))
    let hPt = Int32(max(1, Int(sizePoints.height.rounded(.down))))

    // X11 units (points) — MUST be used for protocol/configure
    // Pixels are for backing buffers / rendering only.
    let wPx = Int32(max(1, Int(sizePixels.width.rounded(.down))))
    let hPx = Int32(max(1, Int(sizePixels.height.rounded(.down))))
        
    // HARD GATE: until X11 maps the host window, never echo Cocoa→X11 resizes.
    if !mappedXids.contains(host) {
      latestHostSizePxByXid[host] = (w: wPx, h: hPx)
      latestHostSizePtByXid[host] = (w: wPt, h: hPt)
      return
    }

    // If we're still bootstrapping this window (not mapped yet), do NOT echo size back into X11.
    // Just remember the latest pixel size and return.
//    if ignoreCocoaResizeUntilMapped.contains(xid) {
    if ignoreCocoaResizeUntilMapped.contains(host) {
      latestHostSizePxByXid[host] = (w: wPx, h: hPx)
      latestHostSizePtByXid[host] = (w: wPt, h: hPt)
      return
    }
        
    // If we’re in an X11-driven resize cascade, never echo Cocoa→X11.
    // BUT we DO want to observe convergence and clear suppression.
    //if let exp = suppressCocoaResizeExpected[xid] {
    if let exp = suppressCocoaResizeExpected[host] {

      if exp.wX11 == wPt && exp.hX11 == hPt {
        suppressCocoaResizeExpected.removeValue(forKey: host)
        suppressCocoaResizeBudget.removeValue(forKey: host)
        suppressNextResizeFromCocoa.remove(host)
        applyingX11Resize.remove(host)
        // Keep latestHostSizePxByXid in sync
        latestHostSizePxByXid[host] = (w: wPx, h: hPx)
        latestHostSizePtByXid[host] = (w: wPt, h: hPt)
        return
      }

      let b = suppressCocoaResizeBudget[host] ?? 0
      if b > 0 {
        suppressCocoaResizeBudget[host] = b - 1
        // Still updating latest helps later “no change” checks
        latestHostSizePxByXid[host] = (w: wPx, h: hPx)
        latestHostSizePtByXid[host] = (w: wPt, h: hPt)
        return
      }

      // Fail-safe: give up suppression if it never converges.
      suppressCocoaResizeExpected.removeValue(forKey: host)
      suppressCocoaResizeBudget.removeValue(forKey: host)
      suppressNextResizeFromCocoa.insert(host)

      // Important: don’t clear applyingX11Resize here; we’re no longer “expecting”
      // but we still want to avoid immediate ping-pong for 1 callback.
    }

    // --------------------------------------------------------------------
    // Stage Manager / transient layout collapse guard:
    //
    // Sometimes Cocoa reports a tiny content size (e.g. 1x1pt -> 2x2px) during
    // window transitions. If we echo that back into X11, the host FB collapses
    // to 2x2 and clients (xterm) appear to "hang" because the window is invisible.
    //
    // Policy:
    //   If the host is mapped and we have previously sent a reasonable size to X11,
    //   ignore any new Cocoa sizes that are "tiny".
    // --------------------------------------------------------------------
    let minStablePx: Int32 = 32
    if suppressCocoaResizeExpected[host] == nil,
       let lastSent = lastSentHostSizePxByXid[host] {
      let lastW = lastSent.w
      let lastH = lastSent.h
      if lastW >= minStablePx, lastH >= minStablePx,
         (wPx < minStablePx || hPx < minStablePx) {

        logAppend?(
          "[RESIZE Cocoa→X11] IGNORE tiny \(wPx)x\(hPx) " +
          "(lastSent \(lastW)x\(lastH)) host=0x\(String(host, radix:16))"
        )

        // Still record latest observed sizes for debugging, but do NOT send Configure.
        latestHostSizePxByXid[host] = (w: wPx, h: hPx)
        latestHostSizePtByXid[host] = (w: wPt, h: hPt)
        return
      }
    }

    
    // If we are actively applying X11 resize, swallow Cocoa callbacks.
    if applyingX11Resize.contains(host) {
      latestHostSizePxByXid[host] = (w: wPx, h: hPx)
      latestHostSizePtByXid[host] = (w: wPt, h: hPt)
      return
    }

    // One-shot suppression damping
    if shouldSuppressResizeFromCocoa(xid: host) {
      latestHostSizePxByXid[host] = (w: wPx, h: hPx)
      latestHostSizePtByXid[host] = (w: wPt, h: hPt)
      return
    }

    // Update latest observed sizes.
    latestHostSizePxByXid[host] = (w: wPx, h: hPx)
    latestHostSizePtByXid[host] = (w: wPt, h: hPt)

    // Only act if the observed pixel size differs from what we've last sent to X11.
    let lastSent = lastSentHostSizePtByXid[host]
    let changed = (lastSent?.w != wPt || lastSent?.h != hPt)
    if !changed { return }

    // Throttle: allow at most ~30fps during live resize
    let now = CACurrentMediaTime()
    let lastT = lastRepaintTimeByXid[host] ?? 0
    if now - lastT >= (1.0 / 30.0) {
      lastRepaintTimeByXid[host] = now
      repaintWorkItemByXid[host]?.cancel()
      repaintWorkItemByXid.removeValue(forKey: host)


      guard mappedXids.contains(host) else { return }
      //if !mappedXids.contains(xid) { return }

      // Debug
      if X11Trace.resize { logAppend?("[RESIZE Cocoa→X11] xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) wPx=\(wPx) hPx=\(hPx) wPt=\(wPt) hPt=\(hPt)") }

      sendConfigureAsync(xid: host, w: wPt, h: hPt)
      return
    }

    // Debounce to the final size shortly
    repaintWorkItemByXid[host]?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self else { return }
      guard let sz = self.latestHostSizePtByXid[host] else { return }
      
      guard self.mappedXids.contains(host) else { return }

      self.lastRepaintTimeByXid[host] = CACurrentMediaTime()
      self.sendConfigureAsync(xid: host, w: sz.w, h: sz.h)
    }
    repaintWorkItemByXid[host] = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.02, execute: work)
  }
  
  
  @MainActor
  func applyX11Resize(xid: UInt32, wX11: Int32, hX11: Int32) {

    let host = topLevelAncestor(of: xid)

    guard let controller = windows[host],
          let win = controller.window,
          let _ = controller.x11View else { return }

    // Suppress Cocoa echo immediately
    suppressCocoaResizeExpected[host] = (wX11: wX11, hX11: hX11)
    suppressCocoaResizeBudget[host] = 12
    suppressNextResizeFromCocoa.insert(host)

    let wPoints = max(1.0, CGFloat(wX11))
    let hPoints = max(1.0, CGFloat(hX11))
    //let wPoints = max(1.0, CGFloat(wPx) / scale)
    //let hPoints = max(1.0, CGFloat(hPx) / scale)

    pendingX11Resize[host] = (wPoints, hPoints)
    guard !resizeWorkScheduled.contains(host) else { return }
    resizeWorkScheduled.insert(host)

    DispatchQueue.main.asyncAfter(deadline: .now() + 0.0) { [weak self, weak win] in
      guard let self, let win else { return }
      self.resizeWorkScheduled.remove(host)
      guard let sz = self.pendingX11Resize.removeValue(forKey: host) else { return }

      // Never allow 0-size (protect Metal)
      if sz.w < 1.0 || sz.h < 1.0 { return }

      // Defer during live resize (don’t recurse immediately)
      if win.inLiveResize {
        self.pendingX11Resize[host] = sz
        self.resizeWorkScheduled.insert(host)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.016) { [weak self] in
          self?.applyX11Resize(xid: host, wX11: wX11, hX11: hX11)
        }
        return
      }

      // If already effectively at target, don’t call setContentSize
      let cur = win.contentLayoutRect.size
      if abs(cur.width - sz.w) < 1.0, abs(cur.height - sz.h) < 1.0 {
        // We're already there; stop suppression immediately.
        suppressCocoaResizeExpected.removeValue(forKey: host)
        suppressCocoaResizeBudget.removeValue(forKey: host)
        suppressNextResizeFromCocoa.remove(host)
        applyingX11Resize.remove(host)
        return
      }
      
      // Right before win.setContentSize(...)
      if let v = controller.x11View, v.isInLayoutPass {
        // try again next tick
        DispatchQueue.main.async { [weak self] in
          self?.applyX11Resize(xid: host, wX11: wX11, hX11: hX11)
        }
        return
      }
      
      // Mark “we are applying” and keep it set until windowResized sees convergence.
      self.applyingX11Resize.insert(host)
      win.setContentSize(NSSize(width: sz.w, height: sz.h))
    }
  }

  // MARK: - ICCCM / EWMH WM compliance

  /// WM_NORMAL_HINTS: apply min/max size and resize increment to NSWindow.
  @MainActor
  func applySizeHints(xid: UInt32, minW: Int, minH: Int, maxW: Int, maxH: Int, incW: Int, incH: Int) {
    let host = topLevelAncestor(of: xid)
    guard let controller = windows[host], let win = controller.window else { return }

    // Apply minimum size (use server floor as absolute minimum)
    if minW > 0 || minH > 0 {
      let effMinW = max(CGFloat(minW), 100)
      let effMinH = max(CGFloat(minH), 50)
      win.contentMinSize = NSSize(width: effMinW, height: effMinH)
    }

    // Apply maximum size
    if maxW > 0 && maxH > 0 {
      win.contentMaxSize = NSSize(width: CGFloat(maxW), height: CGFloat(maxH))
    }

    // Apply resize increment (e.g., xterm character cell size)
    if incW > 0 && incH > 0 {
      win.contentResizeIncrements = NSSize(width: CGFloat(incW), height: CGFloat(incH))
    }

    print("[WM_SIZE_HINTS] xid=0x\(String(format:"%X", host)) min=\(minW)x\(minH) max=\(maxW)x\(maxH) inc=\(incW)x\(incH)")
  }

  /// _NET_WM_WINDOW_TYPE: adjust NSWindow style based on EWMH window type.
  /// Also handles _NET_WM_STATE encoded as synthetic type atoms (0x80000001=modal, 0x80000002=fullscreen).
  @MainActor
  func applyWindowType(xid: UInt32, typeAtom: UInt32) {
    let host = topLevelAncestor(of: xid)
    guard let controller = windows[host], let win = controller.window else { return }

    // _NET_WM_STATE synthetic flags
    if typeAtom == 0x80000001 {
      // MODAL: raise to modal panel level
      win.level = .modalPanel
      print("[NET_WM_STATE] xid=0x\(String(format:"%X", host)) → modal")
      return
    }
    if typeAtom == 0x80000002 {
      // FULLSCREEN
      if !win.styleMask.contains(.fullScreen) {
        win.toggleFullScreen(nil)
      }
      print("[NET_WM_STATE] xid=0x\(String(format:"%X", host)) → fullscreen")
      return
    }

    // _NET_WM_WINDOW_TYPE atoms (values from ClipboardAtoms.hpp)
    let typeNormal:  UInt32 = 82
    let typeDialog:  UInt32 = 83
    let typeToolbar: UInt32 = 84
    let typeUtility: UInt32 = 85
    let typeMenu:    UInt32 = 86
    let typeTooltip: UInt32 = 87
    let typeSplash:  UInt32 = 88

    switch typeAtom {
    case typeDialog:
      // Dialog: titled, closable, non-resizable, floating if appropriate
      win.styleMask = [.titled, .closable]
      win.level = .floating
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → dialog")

    case typeToolbar, typeUtility:
      // Toolbar/Utility: titled, closable, utility level
      win.styleMask = [.titled, .closable, .resizable]
      win.level = .floating
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → toolbar/utility")

    case typeMenu:
      // Menu: borderless, floating (same as override-redirect)
      win.styleMask = [.borderless]
      win.level = .floating
      win.hasShadow = true
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → menu")

    case typeTooltip:
      // Tooltip: borderless, floating, no shadow
      win.styleMask = [.borderless]
      win.level = .floating
      win.hasShadow = false
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → tooltip")

    case typeSplash:
      // Splash: borderless, floating, centered
      win.styleMask = [.borderless]
      win.level = .floating
      win.hasShadow = true
      // Center on the screen where the window currently appears
      win.center()
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → splash")

    case typeNormal:
      // Normal: standard decoration
      win.styleMask = [.titled, .closable, .resizable, .miniaturizable]
      win.level = .normal
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) → normal")

    default:
      // Unknown type atom — treat as normal
      print("[NET_WM_TYPE] xid=0x\(String(format:"%X", host)) unknown type_atom=\(typeAtom)")
    }
  }

  /// WM_HINTS initial_state: 1=NormalState, 3=IconicState (start minimized).
  /// If IconicState and window hasn't been mapped yet, queue miniaturization.
  @MainActor
  func applyInitialState(xid: UInt32, state: UInt32) {
    let host = topLevelAncestor(of: xid)
    if state == 3 { // IconicState
      // Queue miniaturize for when the window maps.
      // If already mapped, miniaturize immediately.
      if mappedXids.contains(host) {
        if let win = windows[host]?.window {
          win.miniaturize(nil)
        }
      } else {
        pendingIconicState.insert(host)
      }
      print("[WM_HINTS] xid=0x\(String(format:"%X", host)) initial_state=IconicState(3)")
    }
  }

  /// Track windows that should start minimized (WM_HINTS IconicState)
  private var pendingIconicState = Set<UInt32>()

  func flushRepaintNow(xid: UInt32) {
    let host = hostXid(xid)

    repaintWorkItemByXid[host]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: host)

    guard let sz = latestHostSizePtByXid[host] else { return }
    guard mappedXids.contains(host) else { return }   // IMPORTANT: don’t resize unmapped hosts

    sendConfigureAsync(xid: host, w: sz.w, h: sz.h)
  }
  
  func closeAll() {
    for (xid, controller) in windows {
      removeWindowObservers(xid: xid)
      suppressNextRaiseFromCocoa.remove(xid)
      controller.close()
    }
    windows.removeAll()
  }
  
  func shouldSuppressResizeFromCocoa(xid: UInt32) -> Bool {
    // Consume-once suppression: if present, remove and suppress exactly one callback.
    if suppressNextResizeFromCocoa.contains(xid) {
      suppressNextResizeFromCocoa.remove(xid)
      return true
    }
    return false
  }
  
  func consumeSuppressMapFromCocoa(xid: UInt32) -> Bool {
    if suppressNextMapFromCocoa.contains(xid) {
      suppressNextMapFromCocoa.remove(xid)
      return true
    }
    return false
  }
  
  func consumeSuppressUnmapFromCocoa(xid: UInt32) -> Bool {
    if suppressNextUnmapFromCocoa.contains(xid) {
      suppressNextUnmapFromCocoa.remove(xid)
      return true
    }
    return false
  }

  func noteDamage(xid: UInt32, x: Int32, y: Int32, w: Int32, h: Int32) {
    // later: union dirty rects here (preferably per-host)
    let host = topLevelAncestor(of: xid)
    if debugSnapshotRouting, host != xid {
      logAppend?("[DAMAGE] xid=0x\(String(xid, radix:16)) -> host=0x\(String(host, radix:16))")
    }
    schedulePresent(xid: host)
  }
  
  private var pendingPresentByHost: Set<UInt32> = []
  private var latestSourceByHost: [UInt32: UInt32] = [:]
  
  func topLevelAncestor(of xid: UInt32) -> UInt32 {
    var cur = xid
    var safety = 0
    while safety < 64 {
      safety += 1
      guard let p = parentByXid[cur] else { return cur }     // unknown parent => assume top-level
      if p == X11_ROOT { return cur }                        // direct child of root => top-level
      cur = p
    }
    return xid
  }
  
  
  private func schedulePresent(xid: UInt32) {
    let host = topLevelAncestor(of: xid)

    if debugSnapshotRouting {
      self.logAppend?(
        "PRESENT_REQ xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) " +
        "mappedHost=\(self.mappedXids.contains(host)) hasWin=\(self.windows[host] != nil)"
      )
    }

    // Hard gates
    guard !closingXids.contains(host) else { return }
    guard windows[host] != nil else { return }
    guard mappedXids.contains(host) else { return }

    // Host-only compositor mode:
    // We intentionally do NOT track latestSourceByHost here, because the C compositor
    // will always composite host + mapped descendants.
    if debugSnapshotRouting {
      let last = latestSourceByHost[host] ?? host
      self.logAppend?(
        "[PRESENT_DBG] (host-only) ignoring source update; " +
        "lastSource[0x\(String(host, radix:16))]=0x\(String(last, radix:16))"
      )
    }

    // De-dupe per-host present scheduling
    if pendingPresentByXid.contains(host) { return }
    pendingPresentByXid.insert(host)

    DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in
      guard let self else { return }

      // CRITICAL: never leave host stuck “pending”
      defer { self.pendingPresentByXid.remove(host) }

      // Re-check gates at fire time
      guard !self.closingXids.contains(host) else { return }
      guard self.windows[host] != nil else { return }
      guard self.mappedXids.contains(host) else { return }

      if self.debugSnapshotRouting {
        self.logAppend?("[PRESENT_DBG] (host-only) firing host=0x\(String(host, radix:16))")
      }

      // Read the accumulated damage rect directly from the shared C++ accumulator.
      // This bypasses the UI command queue drain latency — the accumulator is written
      // by C++ draw ops at draw time and read here at present time, so it always
      // reflects ALL damage up to this instant.
      var sdX: Int32 = 0, sdY: Int32 = 0, sdW: Int32 = 0, sdH: Int32 = 0
      let hasShared = x11_shared_damage_consume(host, &sdX, &sdY, &sdW, &sdH)

      let damage: DamageRect?
      if hasShared && sdW > 0 && sdH > 0 {
        damage = DamageRect(x: Int(sdX), y: Int(sdY),
                            w: Int(sdW), h: Int(sdH))
      } else {
        damage = nil  // full-frame upload
      }

      // Always snapshot/present the HOST. The C side composites children onto host.
      self.snapshotAndPresentNow(sourceXid: host, presentXid: host, damageRect: damage)
    }
  }
  
  @MainActor
  func shouldSuppressRootlessResize(xid: UInt32, w_pt: Int32, h_pt: Int32) -> Bool {
      guard let exp = suppressExpectedSize[xid] else { return false }

      // If we've reached the requested size, consume suppression.
      if exp.w == w_pt && exp.h == h_pt {
          suppressExpectedSize.removeValue(forKey: xid)
          suppressBudget.removeValue(forKey: xid)
          return true
      }

      // Otherwise suppress a limited number of callbacks (layout jitter).
      let b = suppressBudget[xid] ?? 0
      if b > 0 {
          suppressBudget[xid] = b - 1
          return true
      }

      // Fail-safe: stop suppressing if it never converges.
      suppressExpectedSize.removeValue(forKey: xid)
      suppressBudget.removeValue(forKey: xid)
      return false
  }
  
  // WindowRegistry.swift
  @MainActor
  func applyCursor(hostXid: UInt32, shapeRaw: Int32) {
    let host = topLevelAncestor(of: hostXid)
    guard let view = windows[host]?.x11View else { return }
    view.applyCursorShapeRaw(shapeRaw)
  }
  
}



extension WindowRegistry {
  func presentBGRA(xid: UInt32, data: Data, width: Int, height: Int, bytesPerRow: Int,
                   damageRect: DamageRect? = nil) {
    guard let controller = windows[xid] else { return }
    guard let view = controller.x11View else { return }

    // `X11View.presentBGRA` copies the bytes internally, so it’s safe to pass a pointer
    // that’s only valid for the duration of this closure.
    data.withUnsafeBytes { raw in
      guard let base = raw.baseAddress else { return }
      if X11Trace.present { logAppend?("snap: calling presentBGRA xid=0x\(String(xid, radix: 16)) w=\(width) h=\(height) bpr=\(bytesPerRow)") }
      view.presentBGRA(framebuffer: base, width: width, height: height, bytesPerRow: bytesPerRow,
                       damageRect: damageRect)
    }
  }
}


