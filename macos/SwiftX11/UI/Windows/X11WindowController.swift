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
    window.setContentSize(NSSize(width: width, height: height))
    
    self.x11View = viewHolder.view
    window.delegate = self
  }
  
  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }
  
  func setUseMetal(_ enabled: Bool) {
    x11View?.setUseMetal(enabled)
  }
  
  func windowDidResize(_ notification: Notification) {
    guard let win = window else { return }
    
    // Size in points (logical)
    let sizePoints = win.contentLayoutRect.size
    
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
  
  func windowDidBecomeKey(_ notification: Notification) {
      x11_post_focus_event(xid, true)
      x11_post_window_raise(xid)
    
      if shouldLogQueueStats?() == true {
          let s = x11_events_stats()
          logAppend?("EVQ (focus in) count=\(s.count) co=\(s.motion_overwrites) drops=\(s.push_drops)")
      }
  }

  func windowDidResignKey(_ notification: Notification) {
      x11_post_focus_event(xid, false)
    
      if shouldLogQueueStats?() == true {
          let s = x11_events_stats()
          logAppend?("EVQ (focus out) count=\(s.count) co=\(s.motion_overwrites) drops=\(s.push_drops)")
      }
  }

  func windowWillClose(_ notification: Notification) {
      // 1) Tell the backend/event-queue that this X11 window is gone
      x11_post_window_destroy(xid)

      // 2) (Optional but recommended) clear focus + pointer ownership so you don’t “stick”
      x11_post_focus_event(xid, false)
      x11_post_pointer_leave(xid, 0, 0, 0)   // only if your shim has it; otherwise omit
  }  
}

final class X11ViewHolder {
  var view: X11View?
}

