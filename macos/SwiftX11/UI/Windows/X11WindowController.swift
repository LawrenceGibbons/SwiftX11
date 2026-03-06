import AppKit
import X11LowLevel

final class X11WindowController: NSWindowController, NSWindowDelegate {
  private(set) var x11View: X11View?
  private let xid: UInt32
  
  var logAppend: ((String) -> Void)?
  var shouldLogQueueStats: (() -> Bool)?

  init(xid: UInt32, title: String, width: Int, height: Int, useMetal: Bool) {
    self.xid = xid

    // Create X11View directly as the window's contentView — NO SwiftUI hosting.
    // NSHostingController wraps content in NSHostingView which uses Auto Layout
    // constraints.  When the system sends FBSScene updates (Stage Manager,
    // display wake, etc.), those constraints can trigger a layout → frame change
    // notification → window resize → layout cycle (_NSDetectedLayoutRecursion).
    // Bypassing SwiftUI eliminates the Auto Layout constraint chain entirely.
    let contentRect = NSRect(x: 0, y: 0, width: width, height: height)
    let window = NSWindow(contentRect: contentRect,
                          styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered,
                          defer: false)
    window.isRestorable = false
    window.title = title.isEmpty ? "SwiftX11 Window" : title
    window.isReleasedWhenClosed = false

    let view = X11View(frame: contentRect)
    view.xid = xid
    view.wantsMetal = useMetal
    view.autoresizingMask = [.width, .height]
    window.contentView = view

    super.init(window: window)

    self.x11View = view
    view.setUseMetal(useMetal)

    window.acceptsMouseMovedEvents = true
    window.delegate = self
  }
  
  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }
  
  func setUseMetal(_ enabled: Bool) {
    x11View?.setUseMetal(enabled)
  }
  
  func windowDidResize(_ notification: Notification) {
    assert(Thread.isMainThread)
    if WindowRegistry.shared.shouldSuppressResizeFromCocoa(xid: xid) {
      return
    }

    guard let win = window else { return }
    
    // Size in points (logical)
    let sizePoints = win.contentView?.bounds.size ?? win.contentLayoutRect.size

    // Scale factor (1.0 on non-Retina, 2.0 on Retina, etc.)
    let scale = win.backingScaleFactor
    
    // Size in pixels (physical)
    let sizePixels = CGSize(width: sizePoints.width * scale,
                            height: sizePoints.height * scale)
    
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
      x11_post_focus_event(xid, true)
      // Cocoa → X11 event injection (intentionally NOT using client API)
      // This reflects a native window-manager action, not a client request.
      x11_post_window_raise(xid)
    
      if shouldLogQueueStats?() == true {
          logAppend?("EVQ (focus in)")
      }
  }

  func windowDidResignKey(_ notification: Notification) {
      assert(Thread.isMainThread)
      postSyntheticLeaveForCurrentMouseLocation()
      x11_post_focus_event(xid, false)
    
      if shouldLogQueueStats?() == true {
          logAppend?("EVQ (focus out)")
      }
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
    assert(Thread.isMainThread)
    guard let win = window else { return }

    // Treat "not visible" (occluded/hidden/off-screen) as an Unmap, and visible as a Map.
    // This is the most reliable way to observe hide/unhide semantics on macOS.
    let isVisible = win.occlusionState.contains(.visible)
    if isVisible {
      if WindowRegistry.shared.consumeSuppressMapFromCocoa(xid: xid) { return }
      x11_post_window_map(xid)
    } else {
      if WindowRegistry.shared.consumeSuppressUnmapFromCocoa(xid: xid) { return }
      x11_post_window_unmap(xid)
    }
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

