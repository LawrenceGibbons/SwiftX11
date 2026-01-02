import AppKit
import SwiftUI
import X11LowLevel

final class X11WindowController: NSWindowController, NSWindowDelegate {
  private(set) var x11View: X11View?
  private let xid: UInt32
  private var localMonitors: [Any] = []
  
  init(xid: UInt32, title: String, width: Int, height: Int, useMetal: Bool) {
    self.xid = xid
    
    let viewHolder = X11ViewHolder()
    let host = X11WindowHost(useMetal: useMetal) { view in
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
    window.acceptsMouseMovedEvents = true
    installEventMonitors(for: window)
    
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
  }

  func windowDidResignKey(_ notification: Notification) {
      x11_post_focus_event(xid, false)
  }

  private func installEventMonitors(for win: NSWindow) {
    // Mouse + scroll
    let mouseMask: NSEvent.EventTypeMask = [
      .leftMouseDown, .leftMouseUp,
      .rightMouseDown, .rightMouseUp,
      .otherMouseDown, .otherMouseUp,
      .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged,
      .scrollWheel
    ]
    
    localMonitors.append(NSEvent.addLocalMonitorForEvents(matching: mouseMask) { [weak self] event in
      guard let self, event.window === win else { return event }
      self.forwardMouseEvent(event, in: win)
      return event
    } as Any)
    
    // Keyboard
    let keyMask: NSEvent.EventTypeMask = [.keyDown, .keyUp, .flagsChanged]
    localMonitors.append(NSEvent.addLocalMonitorForEvents(matching: keyMask) { [weak self] event in
      guard let self, event.window === win else { return event }
      self.forwardKeyEvent(event)
      return event
    } as Any)
  }
  
  private func forwardMouseEvent(_ event: NSEvent, in win: NSWindow) {
    // Convert window coords -> content view coords (points)
    guard let content = win.contentView else { return }
    let p = content.convert(event.locationInWindow, from: nil)
    
    let scale = win.backingScaleFactor
    let x = Int32(max(0, Int((p.x * scale).rounded(.down))))
    let y = Int32(max(0, Int((p.y * scale).rounded(.down))))
    let mods = modifiersMask(event.modifierFlags)
    
    switch event.type {
    case .leftMouseDown:
      x11_post_pointer_event(xid, X11_PTR_DOWN, x, y, 1, mods)
    case .leftMouseUp:
      x11_post_pointer_event(xid, X11_PTR_UP, x, y, 1, mods)
      
    case .rightMouseDown:
      x11_post_pointer_event(xid, X11_PTR_DOWN, x, y, 1 << 2, mods) // Button3
    case .rightMouseUp:
      x11_post_pointer_event(xid, X11_PTR_UP, x, y, 1 << 2, mods)
      
    case .otherMouseDown:
      x11_post_pointer_event(xid, X11_PTR_DOWN, x, y, 1 << 1, mods) // Button2-ish
    case .otherMouseUp:
      x11_post_pointer_event(xid, X11_PTR_UP, x, y, 1 << 1, mods)
      
    case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
      x11_post_pointer_event(xid, X11_PTR_MOVE, x, y, 0, mods)
      
    case .scrollWheel:
      let dx = Int32(event.scrollingDeltaX)
      let dy = Int32(event.scrollingDeltaY)
      let packed = (UInt32(bitPattern: dx) & 0xFFFF) | ((UInt32(bitPattern: dy) & 0xFFFF) << 16)
      x11_post_pointer_event(xid, X11_SCROLL, x, y, packed, mods)
      
    default:
      break
    }
  }
  
  private func forwardKeyEvent(_ event: NSEvent) {
    let mods = modifiersMask(event.modifierFlags)
    switch event.type {
    case .keyDown:
      let text = event.characters ?? ""
      text.withCString { cstr in
        x11_post_key_event(xid, true, UInt32(event.keyCode), mods, cstr)
      }
    case .keyUp:
      let text = event.characters ?? ""
      text.withCString { cstr in
        x11_post_key_event(xid, false, UInt32(event.keyCode), mods, cstr)
      }
    case .flagsChanged:
      // optional: log modifiers changes later
      break
    default:
      break
    }
  }
  
  private func modifiersMask(_ flags: NSEvent.ModifierFlags) -> UInt32 {
    var m: UInt32 = 0
    if flags.contains(.shift) { m |= 1 << 0 }
    if flags.contains(.control) { m |= 1 << 1 }
    if flags.contains(.option) { m |= 1 << 2 }
    if flags.contains(.command) { m |= 1 << 3 }
    return m
  }
  
  deinit {
    for m in localMonitors {
      NSEvent.removeMonitor(m)
    }
    localMonitors.removeAll()
  }
}

final class X11ViewHolder {
  var view: X11View?
}
