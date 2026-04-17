import AppKit
import X11LowLevel

final class X11WindowController: NSWindowController, NSWindowDelegate {
  private(set) var x11View: X11View?
  private let xid: UInt32
  
  var logAppend: ((String) -> Void)?

  init(xid: UInt32, title: String, x: Int = 0, y: Int = 0, width: Int, height: Int,
       overrideRedirect: Bool = false) {
    self.xid = xid

    // Create X11View directly as the window's contentView — NO SwiftUI hosting.
    // NSHostingController wraps content in NSHostingView which uses Auto Layout
    // constraints.  When the system sends FBSScene updates (Stage Manager,
    // display wake, etc.), those constraints can trigger a layout → frame change
    // notification → window resize → layout cycle (_NSDetectedLayoutRecursion).
    // Bypassing SwiftUI eliminates the Auto Layout constraint chain entirely.

    // For override-redirect windows (menus/tooltips), convert X11 root coordinates
    // to macOS screen coordinates. X11: origin top-left, y-down. macOS: origin
    // bottom-left, y-up.
    let contentRect: NSRect
    if overrideRedirect {
      // Convert X11 root coords (top-left, y-down) to macOS global coords.
      // Uses virtual desktop union bounds so windows on any monitor work correctly.
      // C++ already applies a size floor for tiny windows, so width/height are
      // pre-enlarged.  Use them directly.
      let origin = WindowRegistry.x11RootToMacOSOrigin(
        x11X: CGFloat(x), x11Y: CGFloat(y), height: CGFloat(height))
      contentRect = NSRect(origin: origin,
                           size: NSSize(width: max(1, CGFloat(width)),
                                        height: max(1, CGFloat(height))))
    } else {
      // C++ already applies a size floor for tiny windows (< 10px → 200×100),
      // so width/height are pre-enlarged.  The mapWindow path defers SHOWING
      // floored windows until the client sends the real size.
      contentRect = NSRect(x: 0, y: 0, width: max(1, width), height: max(1, height))
    }

    // Override-redirect windows (menus, tooltips, popups) get no WM decoration.
    // Standard X11 behavior: the WM does not intercept or decorate these windows.
    let styleMask: NSWindow.StyleMask = overrideRedirect
      ? [.borderless]
      : [.titled, .closable, .resizable, .miniaturizable]

    let window = NSWindow(contentRect: contentRect,
                          styleMask: styleMask,
                          backing: .buffered,
                          defer: false)
    window.isRestorable = false
    window.isReleasedWhenClosed = false
    // Allow the window server to capture window content for Stage Manager
    // thumbnails and Mission Control.  Default is .readOnly on modern macOS
    // which can cause blank captures for Metal-backed windows.
    window.sharingType = .readWrite

    if overrideRedirect {
      // Override-redirect: float above normal windows (popup/menu behavior).
      window.level = .floating
      window.hasShadow = true
    } else {
      window.title = title.isEmpty ? "SwiftX11 Window" : title
      // WM minimum size: prevent the user from resizing below a usable size.
      window.contentMinSize = NSSize(width: 100, height: 50)
    }

    let view = X11View(frame: contentRect)
    view.xid = xid
    view.autoresizingMask = [.width, .height]
    window.contentView = view

    super.init(window: window)

    self.x11View = view
    view.ensureMetalSetup()

    // acceptsMouseMovedEvents delivers mouseMoved for ANY mouse position
    // (including outside the view / in the title bar).  sendMotion sets
    // deliver=0 for outside-view events so they only update root coords
    // (for QueryPointer / xeyes global tracking) without sending
    // MotionNotify.  HostCommandQueue coalescing protects deliver=1
    // events from being overwritten by subsequent deliver=0 events.
    window.acceptsMouseMovedEvents = true
    window.delegate = self
  }

  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }
  
  func windowDidMove(_ notification: Notification) {
    assert(Thread.isMainThread)
    guard let win = window else { return }
    // Only sync position for mapped, non-OR windows.  OR windows are positioned
    // by the server (ConfigureWindow/MapWindow), not by macOS window management.
    // Unmapped windows may fire windowDidMove during initial setup before the
    // X11 lifecycle is ready.
    guard WindowRegistry.shared.isMapped(xid: xid) else { return }
    guard !(WindowRegistry.shared.isOverrideRedirect(xid: xid)) else { return }
    // Sync the NSWindow's actual screen position back to X11 WindowTable.
    // Without this, TranslateCoordinates returns stale root coordinates
    // and popup menus (override-redirect windows) appear at wrong positions.
    let contentFrame = win.contentView?.frame ?? win.contentLayoutRect
    let (x11X, x11Y) = WindowRegistry.macOSOriginToX11Root(
      macOrigin: win.frame.origin, height: contentFrame.size.height)
    x11_set_window_position(xid, x11X, x11Y)
  }

  func windowDidResize(_ notification: Notification) {
    assert(Thread.isMainThread)
    if WindowRegistry.shared.shouldSuppressResizeFromCocoa(xid: xid) {
      return
    }

    guard let win = window else { return }

    // Sync position FIRST — left/top edge resize changes origin + size
    // simultaneously.  Without this, applyRootlessResize reads stale x/y
    // from WindowTable, sending ConfigureNotify with wrong origin and
    // causing pointer coordinate offsets in the client.
    if WindowRegistry.shared.isMapped(xid: xid),
       !(WindowRegistry.shared.isOverrideRedirect(xid: xid)) {
      let contentFrame = win.contentView?.frame ?? win.contentLayoutRect
      let (x11X, x11Y) = WindowRegistry.macOSOriginToX11Root(
        macOrigin: win.frame.origin, height: contentFrame.size.height)
      x11_set_window_position(xid, x11X, x11Y)
    }

    // Size in points (logical)
    let sizePoints = win.contentView?.bounds.size ?? win.contentLayoutRect.size

    // Scale factor (1.0 on non-Retina, 2.0 on Retina, etc.)
    let scale = win.backingScaleFactor

    // Size in pixels (physical)
    let sizePixels = CGSize(width: sizePoints.width * scale,
                            height: sizePoints.height * scale)

    #if DEBUG
    fputs(String(format: "[GEOM_NS] wid=0x%08X source=windowDidResize pts=%.0fx%.0f px=%.0fx%.0f scale=%.2f minSize=%.0fx%.0f maxSize=%.0fx%.0f live=%d\n",
                 xid, sizePoints.width, sizePoints.height,
                 sizePixels.width, sizePixels.height, scale,
                 win.contentMinSize.width, win.contentMinSize.height,
                 win.contentMaxSize.width, win.contentMaxSize.height,
                 win.inLiveResize ? 1 : 0), stderr)
    #endif

    WindowRegistry.shared.windowResized(
      xid: xid,
      sizePoints: sizePoints,
      sizePixels: sizePixels,
      scale: scale
    )
  }
  
  func windowDidEndLiveResize(_ notification: Notification) {
    assert(Thread.isMainThread)

    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.windowDidResize(notification)
      WindowRegistry.shared.flushRepaintNow(xid: self.xid)

      // Force Expose to all mapped children.  During live resize, each
      // ensureHostSurface reallocation clears areas outside the old overlap
      // to white.  If the user stops resizing during a flash, children
      // (scrollbar, etc.) may not get re-exposed because applyRootlessResize
      // returns early when size hasn't changed.  This ensures a full redraw.
      x11_post_expose_children(self.xid)

      // After ExposeChildren, wait for the client to process Expose events and
      // redraw into the new surface, then promote it as the display source.
      // 150ms gives the client time for: ConfigureNotify → ConfigureWindow →
      // Expose → draw (~7 present cycles).
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
        guard let self else { return }
        // Don't promote if user started another live resize
        guard self.window?.inLiveResize != true else { return }
        if let view = WindowRegistry.shared.viewForHostXid(self.xid) {
          view.promoteDisplaySurface()
        }
        // Second ExposeChildren after promote: by now all ConfigureWindow
        // requests from xterm have been processed and child geometries are
        // final.  The first ExposeChildren (above) can arrive before xterm
        // reconfigures children — e.g., a right-side scrollbar at its OLD x
        // position may be beyond the new (narrower) surface, causing
        // resolveDrawableRW to fail silently.  This second pass catches it.
        x11_post_expose_children(self.xid)
        // Force a present from the new surface.  While the display frame
        // was active, all damage was consumed and discarded by schedulePresent;
        // without this kick, the redrawn surface never gets presented.
        WindowRegistry.shared.noteDamage(xid: self.xid, x: 0, y: 0, w: 0, h: 0)
      }
    }
  }
  
  
private func currentMouseLocationInContentUnits() -> (x: Int32, y: Int32) {
  guard let win = window else { return (0, 0) }
  guard let content = win.contentView else { return (0, 0) }

  let mouseInScreen = NSEvent.mouseLocation
  let mouseInWindow = win.convertPoint(fromScreen: mouseInScreen)
  let p = content.convert(mouseInWindow, from: nil)

  let x = Int32(max(0, Int(p.x.rounded(.down))))
  let y = Int32(max(0, Int(p.y.rounded(.down))))
  return (x, y)
}

private func postSyntheticEnterForCurrentMouseLocation() {
  let (x, y) = currentMouseLocationInContentUnits()
  x11_post_pointer_enter(xid, x, y, 0)
}

private func postSyntheticLeaveForCurrentMouseLocation() {
  let (x, y) = currentMouseLocationInContentUnits()
  x11_post_pointer_leave(xid, x, y, 0)
}

  
//  private func currentMouseLocationInContentPixels() -> (x: Int32, y: Int32) {
//    guard let win = window else { return (0, 0) }
//    guard let content = win.contentView else { return (0, 0) }
//
//    // Mouse location is in screen coordinates with origin at bottom-left.
//    let mouseInScreen = NSEvent.mouseLocation
//
//    // Convert screen -> window -> contentView coords (points)
//    let mouseInWindow = win.convertPoint(fromScreen: mouseInScreen)
//    let p = content.convert(mouseInWindow, from: nil)
//
//    let scale = win.backingScaleFactor
//    let x = Int32(max(0, Int((p.x * scale).rounded(.down))))
//    let y = Int32(max(0, Int((p.y * scale).rounded(.down))))
//    return (x, y)
//  }
//
//  private func postSyntheticEnterForCurrentMouseLocation() {
//    let (x, y) = currentMouseLocationInContentPixels()
//
//    // Force backend pointer ownership to follow the newly-key window.
//    // This is important on macOS because changing key window does not
//    // reliably generate enter/leave transitions.
//    x11_post_pointer_enter(xid, x, y, 0)
//  }
//
//  private func postSyntheticLeaveForCurrentMouseLocation() {
//    let (x, y) = currentMouseLocationInContentPixels()
//    x11_post_pointer_leave(xid, x, y, 0)
//  }

  func windowDidBecomeKey(_ notification: Notification) {
      assert(Thread.isMainThread)
      postSyntheticEnterForCurrentMouseLocation()
      // Focus & raise events are posted by WindowRegistry's didBecomeKey
      // observer — it has suppressNextRaiseFromCocoa logic that prevents
      // feedback loops when a raise was initiated from the X11/C++ side.
      // Posting here too would bypass that suppression and cause the
      // WM_TAKE_FOCUS bounce loop between dialog and main window.
  }

  func windowDidResignKey(_ notification: Notification) {
      assert(Thread.isMainThread)
      postSyntheticLeaveForCurrentMouseLocation()
      // Focus-loss event posted by WindowRegistry's didResignKey observer.
  }

  @objc
  func windowWillClose(_ notification: Notification) {
    assert(Thread.isMainThread)
    // make state look clean before arrival of the close event
    x11_post_focus_event(xid, false)
    postSyntheticLeaveForCurrentMouseLocation()

    // Send WM_DELETE_WINDOW (if client supports it) or forceful disconnect.
    // This replaces the old x11_post_window_destroy which only hid the NSWindow
    // but left the X11 client process running.
    x11_post_window_close(xid)
  }  
  
  func windowDidMiniaturize(_ notification: Notification) {
    assert(Thread.isMainThread)
    if WindowRegistry.shared.consumeSuppressUnmapFromCocoa(xid: xid) { return }
    x11_post_window_unmap(xid)
  }

  func windowDidDeminiaturize(_ notification: Notification) {
    assert(Thread.isMainThread)
    if WindowRegistry.shared.consumeSuppressMapFromCocoa(xid: xid) { return }
    x11_post_window_map(xid)
  }
  
  func windowDidChangeOcclusionState(_ notification: Notification) {
    // NOTE: Occlusion state changes happen when OTHER macOS apps cover
    // our window. We must NOT treat this as an X11 unmap — that would
    // permanently hide the window (orderOut) and it would never come back.
    // The X11 window remains mapped; only Cocoa's visibility changes.
    //
    // We can optionally pause rendering when fully occluded (future
    // optimization), but we must never unmap/orderOut the window.
  }
  
  
  /// Present a BGRA8888 little-endian framebuffer for this X11 window.
  ///
  /// This routes to the `X11View` (the window's contentView).
  /// The buffer is provided as `Data` so callers can safely own/copy bytes.
  @MainActor
  func presentBGRA(data: Data, width: Int, height: Int, bytesPerRow: Int) {
    guard width > 0, height > 0, bytesPerRow > 0 else { return }
    guard let view = self.x11View else { return }

    data.withUnsafeBytes { raw in
      guard let base = raw.baseAddress else { return }
      view.presentBGRA(framebuffer: base, width: width, height: height, bytesPerRow: bytesPerRow)
    }
  }
  
}

