import AppKit
import SwiftUI
import X11LowLevel

final class X11WindowController: NSWindowController, NSWindowDelegate {
  private(set) var x11View: X11View?
  private let xid: UInt32
  
  var logAppend: ((String) -> Void)?
  var shouldLogQueueStats: (() -> Bool)?

  init(xid: UInt32, title: String, width: Int, height: Int, useMetal: Bool) {
    self.xid = xid
    
    let viewHolder = X11ViewHolder()
    let host = X11WindowHost(useMetal: useMetal) { view in
      view.xid = xid
      viewHolder.view = view
    }
    let hosting = NSHostingController(rootView: host)
    
    let window = NSWindow(contentViewController: hosting)
    window.isRestorable = false
    window.title = title.isEmpty ? "SwiftX11 Window" : title
    window.setContentSize(NSSize(width: width, height: height))
    window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
    window.isReleasedWhenClosed = false
    
    super.init(window: window)
    
    // SwiftUI creates the NSView later (makeNSView). Capture it when it becomes available.
    viewHolder.onReady = { [weak self] v in
      self?.x11View = v
    }

    window.setContentSize(NSSize(width: width, height: height))
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
    // Force an immediate final repaint at the final size
    windowDidResize(notification)
    WindowRegistry.shared.flushRepaintNow(xid: xid)
  }
  
  private func currentMouseLocationInContentPixels() -> (x: Int32, y: Int32) {
    guard let win = window else { return (0, 0) }
    guard let content = win.contentView else { return (0, 0) }

    // Mouse location is in screen coordinates with origin at bottom-left.
    let mouseInScreen = NSEvent.mouseLocation

    // Convert screen -> window -> contentView coords (points)
    let mouseInWindow = win.convertPoint(fromScreen: mouseInScreen)
    let p = content.convert(mouseInWindow, from: nil)

    let scale = win.backingScaleFactor
    let x = Int32(max(0, Int((p.x * scale).rounded(.down))))
    let y = Int32(max(0, Int((p.y * scale).rounded(.down))))
    return (x, y)
  }

  private func postSyntheticEnterForCurrentMouseLocation() {
    let (x, y) = currentMouseLocationInContentPixels()

    // Force backend pointer ownership to follow the newly-key window.
    // This is important on macOS because changing key window does not
    // reliably generate enter/leave transitions.
    x11_post_pointer_enter(xid, x, y, 0)
  }

  private func postSyntheticLeaveForCurrentMouseLocation() {
    let (x, y) = currentMouseLocationInContentPixels()
    x11_post_pointer_leave(xid, x, y, 0)
  }

  func windowDidBecomeKey(_ notification: Notification) {
      postSyntheticEnterForCurrentMouseLocation()
      x11_post_focus_event(xid, true)
      x11_post_window_raise(xid)
    
      if shouldLogQueueStats?() == true {
          let s = x11_events_stats()
          logAppend?("EVQ (focus in) count=\(s.count) co=\(s.motion_overwrites) drops=\(s.push_drops)")
      }
  }

  func windowDidResignKey(_ notification: Notification) {
      postSyntheticLeaveForCurrentMouseLocation()
      x11_post_focus_event(xid, false)
    
      if shouldLogQueueStats?() == true {
          let s = x11_events_stats()
          logAppend?("EVQ (focus out) count=\(s.count) co=\(s.motion_overwrites) drops=\(s.push_drops)")
      }
  }

  @objc
  func windowWillClose(_ notification: Notification) {
    // make state look clean before arrival of the destroy event
    x11_post_focus_event(xid, false)
    postSyntheticLeaveForCurrentMouseLocation()

    // Tell the backend/event-queue that this X11 window is gone
    x11_post_window_destroy(xid)
    
#if DEBUG
    // warn if somehow it's still alive
    if x11_debug_get_window_alive(xid) != 0 {
      print("WARN: xid still alive after destroy: 0x\(String(xid, radix: 16))")
    }
#endif

  }  
}

final class X11ViewHolder {
  private var _onReady: ((X11View) -> Void)?

  /// Called when the SwiftUI-created X11View becomes available.
  /// If the view is already set, this delivers immediately.
  var onReady: ((X11View) -> Void)? {
    get { _onReady }
    set {
      _onReady = newValue
      if let v = view {
        _onReady?(v)
      }
    }
  }

  var view: X11View? {
    didSet {
      if let v = view {
        _onReady?(v)
      }
    }
  }
}
