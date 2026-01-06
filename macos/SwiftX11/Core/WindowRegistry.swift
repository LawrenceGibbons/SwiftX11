//
//  WindowRegistry.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 12/31/25.
//

import AppKit
import QuartzCore
import X11LowLevel

@MainActor
final class WindowRegistry {
  static let shared = WindowRegistry()
  
  // NEW: injected hooks (set once from SwiftUI/XServerController)
  private var logAppend: ((String) -> Void)?
  private var isLogPaused: (() -> Bool)?
  private var showQueueStats: (() -> Bool)?

  // call this once at startup
  func attachLogHooks(
    logAppend: @escaping (String) -> Void,
    isLogPaused: @escaping () -> Bool,
    showQueueStats: @escaping () -> Bool
  ) {
    self.logAppend = logAppend
    self.isLogPaused = isLogPaused
    self.showQueueStats = showQueueStats
  }

  private init() {}
  
  private var windows: [UInt32: X11WindowController] = [:]
  private var repaintWorkItemByXid: [UInt32: DispatchWorkItem] = [:]
  private var latestPixelSizeByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  private var lastRepaintTimeByXid: [UInt32: CFTimeInterval] = [:]
  private var windowObserversByXid: [UInt32: [NSObjectProtocol]] = [:]
  private var suppressNextRaiseFromCocoa: Set<UInt32> = []
  private var suppressNextResizeFromCocoa: Set<UInt32> = []
  private var suppressNextMapFromCocoa: Set<UInt32> = []
  private var suppressNextUnmapFromCocoa: Set<UInt32> = []
  
  var useMetalForNewWindows: Bool = true  // set from UI on launch / changes
  
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
        x11_post_focus_event(xid, true)
        x11_post_window_raise(xid)
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
    ) { _ in
      MainActor.assumeIsolated {
        // Prevent feedback loop when unmap came from X11 -> Swift.
        if self.consumeSuppressUnmapFromCocoa(xid: xid) {
          return
        }
        // Cocoa -> X11
        x11_client_unmap_window(xid)
      }
    }
    toks.append(didMini)
    
    let didDeMini = center.addObserver(
      forName: NSWindow.didDeminiaturizeNotification, object: window, queue: .main
    ) { _ in
      MainActor.assumeIsolated {
        // Prevent feedback loop when map came from X11 -> Swift.
        if self.consumeSuppressMapFromCocoa(xid: xid) {
          return
        }
        // Cocoa -> X11
        x11_client_map_window(xid)
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
    guard let win = windows[xid]?.window else { return }
    suppressNextMapFromCocoa.insert(xid)
    win.orderFront(nil)              // show without forcing key
  }
  
  func unmapWindow(xid: UInt32) {
      guard let controller = windows[xid] else { return }
      suppressNextUnmapFromCocoa.insert(xid)
      controller.window?.orderOut(nil)
  }
  
  func createWindow(xid: UInt32, title: String, width: Int, height: Int) {
    // Avoid duplicates
    if windows[xid] != nil {
      // Don’t implicitly map/raise; just ignore duplicate create.
      return
    }
    
    let controller = X11WindowController(xid: xid,
                                         title: title, 
                                         width: width, 
                                         height: height,
                                         useMetal: useMetalForNewWindows )
    // propagate hooks into the window controller
    controller.logAppend = { [weak self] line in
      guard let self else { return }
      guard self.isLogPaused?() != true else { return }
      self.logAppend?(line)
    }
    controller.shouldLogQueueStats = { [weak self] in
      self?.showQueueStats?() ?? false
    }
    windows[xid] = controller
    controller.window?.orderOut(nil)   // start unmapped/hidden
    if let win = controller.window {
      installWindowObservers(xid: xid, window: win)
    }
    
    // IMPORTANT: request an initial resize at the *actual* pixel size once the window has laid out.
    // Defensive: if layout reports 0 while unmapped/hidden, fall back to the requested size.
    let fallbackWPoints = CGFloat(max(1, width))
    let fallbackHPoints = CGFloat(max(1, height))

    DispatchQueue.main.async { [weak controller] in
      guard let win = controller?.window else { return }

      // Prefer contentLayoutRect, but it can be 0x0 when the window is never shown.
      var sizePoints = win.contentLayoutRect.size

      if sizePoints.width < 1 || sizePoints.height < 1 {
        // Fallback to the initial requested size (points)
        sizePoints = CGSize(width: fallbackWPoints, height: fallbackHPoints)
      }

      let scale = win.backingScaleFactor
      let wPx = Int32(max(1, Int((sizePoints.width * scale).rounded(.down))))
      let hPx = Int32(max(1, Int((sizePoints.height * scale).rounded(.down))))

      x11_client_configure_window(xid, wPx, hPx)
    }
  }
  
  func closeWindow(xid: UInt32) {
    removeWindowObservers(xid: xid)
    suppressNextRaiseFromCocoa.remove(xid)
    suppressNextResizeFromCocoa.remove(xid)
    suppressNextMapFromCocoa.remove(xid)
    suppressNextUnmapFromCocoa.remove(xid)
    
    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: xid)
    latestPixelSizeByXid.removeValue(forKey: xid)
    lastRepaintTimeByXid.removeValue(forKey: xid)
    
    guard let controller = windows.removeValue(forKey: xid) else { return }
    controller.close()
  }
  
  func presentFrame(xid: UInt32, bgra: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    guard let controller = windows[xid], let view = controller.x11View else { return }
    view.presentBGRA(framebuffer: bgra, width: width, height: height, bytesPerRow: bytesPerRow)
  }
  
  func setTitle(xid: UInt32, title: String) {
    guard let controller = windows[xid] else { return }
    controller.window?.title = title
  }
  
  func raiseWindow(xid: UInt32) {
    guard let controller = windows[xid], let win = controller.window else { return }

    // Suppress the next didBecomeKey notification since we're causing it.
    suppressNextRaiseFromCocoa.insert(xid)

    win.makeKeyAndOrderFront(nil)
  }
  
  
  func windowResized(xid: UInt32, sizePoints _: CGSize, sizePixels: CGSize, scale _: CGFloat) {
    if shouldSuppressResizeFromCocoa(xid: xid) {
      return
    }

    let w = Int32(max(1, Int(sizePixels.width.rounded(.down))))
    let h = Int32(max(1, Int(sizePixels.height.rounded(.down))))
    latestPixelSizeByXid[xid] = (w: w, h: h)
    
    // Throttle: allow at most ~30fps during live resize
    let now = CACurrentMediaTime()
    let last = lastRepaintTimeByXid[xid] ?? 0
    if now - last >= (1.0 / 30.0) {
      lastRepaintTimeByXid[xid] = now
      repaintWorkItemByXid[xid]?.cancel()
      repaintWorkItemByXid.removeValue(forKey: xid)
      
      x11_client_configure_window(xid, w, h)
      return
    }
    
    // Removed nested mapWindow/unmapWindow functions here.
    
    // Otherwise debounce to the final size shortly
    repaintWorkItemByXid[xid]?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self else { return }
      guard let sz = self.latestPixelSizeByXid[xid] else { return }
      
      self.lastRepaintTimeByXid[xid] = CACurrentMediaTime()
      
      x11_client_configure_window(xid, sz.w, sz.h)
    }
    repaintWorkItemByXid[xid] = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.02, execute: work)
  }
  
  @MainActor
  func applyX11Resize(xid: UInt32, wPx: Int32, hPx: Int32) {
    guard let controller = windows[xid], let win = controller.window else { return }

    // Mark that the next Cocoa resize callback is "caused by us"
    suppressNextResizeFromCocoa.insert(xid)

    let scale = win.backingScaleFactor
    let wPoints = CGFloat(max(1, wPx)) / scale
    let hPoints = CGFloat(max(1, hPx)) / scale
    
    // Optional: avoid churn
    let cur = win.contentLayoutRect.size
    if abs(cur.width - wPoints) < 0.5, abs(cur.height - hPoints) < 0.5 { return }

    win.setContentSize(NSSize(width: wPoints, height: hPoints))
  }
  
  func flushRepaintNow(xid: UInt32) {
    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: xid)

    guard let sz = latestPixelSizeByXid[xid] else { return }

    // Client-style request: configure implies damage + wakeup on the server.
    x11_client_configure_window(xid, sz.w, sz.h)
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
  
  func setUseMetalForAllWindows(_ enabled: Bool) {
    useMetalForNewWindows = enabled
    
    for (xid, controller) in windows {
      controller.setUseMetal(enabled)
      
      if let win = controller.window {
        let sizePoints = win.contentLayoutRect.size
        let scale = win.backingScaleFactor
        let wPx = Int32(max(1, Int((sizePoints.width * scale).rounded(.down))))
        let hPx = Int32(max(1, Int((sizePoints.height * scale).rounded(.down))))
        x11_client_configure_window(xid, wPx, hPx)
      }
    }
  }
}
