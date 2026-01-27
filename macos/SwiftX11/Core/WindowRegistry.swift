//
//  WindowRegistry.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 12/31/25.
//

import AppKit
import QuartzCore
import X11LowLevel

struct X11WindowInfo {
  var xid: UInt32
  var parentXid: UInt32
  var width: Int
  var height: Int
  var title: String
  var mapped: Bool
}

@MainActor
final class WindowRegistry {
  static let shared = WindowRegistry()
  
  // injected hooks (set once from SwiftUI/XServerController)
  private var logAppend: ((String) -> Void)?
  private var isLogPaused: (() -> Bool)?
  private var showQueueStats: (() -> Bool)?
  private var pendingPresentByXid: Set<UInt32> = []
  
  // xid relationship tracking
  private var infoByXid: [UInt32: X11WindowInfo] = [:]
  private let X11_ROOT: UInt32 = 0x00000001
  private var parentByXid: [UInt32: UInt32] = [:]
  private var childrenByParent: [UInt32: Set<UInt32>] = [:]   // optional, but useful
  private let rootXid: UInt32 = 1

  private func isTopLevelX11Window(_ xid: UInt32) -> Bool {
    // Top-level means parent is the X11 root (1). If unknown, assume top-level
    // (defensive; better to show a window than hide it).
    if let info = infoByXid[xid] { return info.parentXid == rootXid }
    if let p = parentByXid[xid] { return p == rootXid }
    return true
  }
  
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
  private var closingXids = Set<UInt32>()
  private var mappedXids = Set<UInt32>()
  
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
    
    //let didMini = center.addObserver(
    //  forName: NSWindow.didMiniaturizeNotification, object: window, queue: .main
    //) { _ in
    //  MainActor.assumeIsolated {
    //    // Prevent feedback loop when unmap came from X11 -> Swift.
    //    if self.consumeSuppressUnmapFromCocoa(xid: xid) {
    //      return
    //    }
    //    // Cocoa -> X11
    //    x11_client_unmap_window(xid)
    //  }
    //}
    let didMini = center.addObserver(
      forName: NSWindow.didMiniaturizeNotification, object: window, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      MainActor.assumeIsolated {
        if self.consumeSuppressUnmapFromCocoa(xid: xid) { return }
        DispatchQueue.main.async {
          x11_client_unmap_window(xid)
        }
      }
    }
    toks.append(didMini)
    
    //let didDeMini = center.addObserver(
    //  forName: NSWindow.didDeminiaturizeNotification, object: window, queue: .main
    //) { _ in
    //  MainActor.assumeIsolated {
    //    // Prevent feedback loop when map came from X11 -> Swift.
    //    if self.consumeSuppressMapFromCocoa(xid: xid) {
    //      return
    //    }
    //    // Cocoa -> X11
    //    x11_client_map_window(xid)
    //  }
    //}
    let didDeMini = center.addObserver(
      forName: NSWindow.didDeminiaturizeNotification, object: window, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      MainActor.assumeIsolated {
        if self.consumeSuppressMapFromCocoa(xid: xid) { return }
        DispatchQueue.main.async {
          x11_client_map_window(xid)
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
    // Track mapped state for the specific X window.
    if var info = infoByXid[xid] {
      info.mapped = true
      infoByXid[xid] = info
    }

    // Child maps should NOT create/show Cocoa windows,
    // but they should cause a present of the host (composited later).
    guard isTopLevelX11Window(xid) else {
      schedulePresent(xid: xid)
      return
    }

    let host = topLevelAncestor(of: xid)

    // If we're closing or don't have a host window controller, do nothing.
    guard !closingXids.contains(host) else { return }
    guard let win = windows[host]?.window else { return }

    // Mark mapped so damage can schedule presents again.
    mappedXids.insert(host)

    // Prevent feedback loop when map came from X11 -> Swift.
    suppressNextMapFromCocoa.insert(host)

    // Show without forcing key
    win.orderFront(nil)

    // Kick an initial present for newly-mapped windows.
    schedulePresent(xid: host)
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

    suppressNextUnmapFromCocoa.insert(host)
    controller.window?.orderOut(nil)
  }
  
  
  @MainActor
  func noteX11WindowDestroyed(xid: UInt32) {
    // 0) Cancel any scheduled present/snapshot bookkeeping for this xid as either host or source.
    pendingPresentByXid.remove(xid)
    pendingPresentByHost.remove(xid)
    latestSourceByHost.removeValue(forKey: xid)

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
    latestPixelSizeByXid.removeValue(forKey: xid)
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
                            width: Int,
                            height: Int)
  {
    let xids = String(format: "0x%X", xid)
    let parent_xids = String(format: "0x%X", parentXid)

    logAppend?("Entered noteX11WIndowCreated: xid=\(xids), parent=\(parent_xids), \(width)x\(height)")
    // Update/insert metadata (idempotent).
    infoByXid[xid] = X11WindowInfo(
      xid: xid,
      parentXid: parentXid,
      width: width,
      height: height,
      title: title,
      mapped: false
    )

    // Track hierarchy: parent relationship is required for topLevelAncestor() / presentation routing.
    parentByXid[xid] = parentXid
    // Track hierarchy (optional, but helps debugging and future correctness)
    noteSubwindow(childXid: xid, parentXid: parentXid)

    // Only create Cocoa windows for top-level windows.
    if parentXid == rootXid {
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

    let controller = X11WindowController(
      xid: xid,
      title: info.title,
      width: info.width,
      height: info.height,
      useMetal: useMetalForNewWindows
    )

    controller.logAppend = { [weak self] line in
      guard let self else { return }
      guard self.isLogPaused?() != true else { return }
      self.logAppend?(line)
    }
    controller.shouldLogQueueStats = { [weak self] in
      self?.showQueueStats?() ?? false
    }

    windows[xid] = controller
    controller.window?.orderOut(nil) // start hidden/unmapped

    if let win = controller.window {
      installWindowObservers(xid: xid, window: win)
    }
  }
  
  
  
  func closeWindow(xid: UInt32) {
    removeWindowObservers(xid: xid)
    suppressNextRaiseFromCocoa.remove(xid)
    suppressNextResizeFromCocoa.remove(xid)
    suppressNextMapFromCocoa.remove(xid)
    suppressNextUnmapFromCocoa.remove(xid)
    
    // Cancel repaint work items
    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: xid)
    
    // Cancel snapshot/present pipeline
    pendingPresentByXid.remove(xid)
    mappedXids.remove(xid)

    latestPixelSizeByXid.removeValue(forKey: xid)
    lastRepaintTimeByXid.removeValue(forKey: xid)
    
    closingXids.insert(xid)
    defer { closingXids.remove(xid) }

    guard let controller = windows.removeValue(forKey: xid) else { return }
    controller.close()
  }
  
  
  func presentFrame(xid: UInt32, bgra: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    guard let controller = windows[xid], let view = controller.x11View else { return }
    view.presentBGRA(framebuffer: bgra, width: width, height: height, bytesPerRow: bytesPerRow)
  }
  
  func handleDamageEvent(_ ev: x11_event_t) {
    let xid = ev.xid
    let x = ev.u.win_damage.x_px
    let y = ev.u.win_damage.y_px
    let w = ev.u.win_damage.w_px
    let h = ev.u.win_damage.h_px
    
    let hasWindow = (windows[xid] != nil)
    logAppend?("handleDamageEvent: xid=0x\(String(xid, radix: 16).uppercased()) rect=(\(x),\(y)) \(w)x\(h) hasWindow=\(hasWindow)")
    
    // Record damage (later: coalesce dirty rects). This also schedules a single present
    // on the main queue (see schedulePresent).
    noteDamage(xid: xid, x: x, y: y, w: w, h: h)
  }
    
  
  private func snapshotAndPresentNow(sourceXid: UInt32, presentXid: UInt32) {
    guard windows[presentXid] != nil else { return }
    guard !closingXids.contains(presentXid) else { return }

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

    let a1 = copyFrame(sz: sz)
    if !a1.ok {
      guard let newSz = querySize() else { return }
      sz = newSz
      let a2 = copyFrame(sz: sz)
      guard a2.ok else { return }

      presentBGRA(xid: presentXid, data: a2.data,
                  width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
      return
    }

    presentBGRA(xid: presentXid, data: a1.data,
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
    logAppend?("snap(copy#1): xid=0x\(String(xid, radix: 16)) okCopy=\(attempt1.okCopy) ok=\(attempt1.ok) outW=\(attempt1.outW) outH=\(attempt1.outH) outBpr=\(attempt1.outBpr) expect=\(sz.w)x\(sz.h) bpr=\(sz.bpr)")
    
    // 3) If size changed mid-copy, retry once after re-query
    if !attempt1.ok {
      guard let newSz = querySize() else { return }
      sz = newSz
      
      let attempt2 = copyFrame(sz: sz)
      logAppend?("snap(copy#2): xid=0x\(String(xid, radix: 16)) okCopy=\(attempt2.okCopy) ok=\(attempt2.ok) outW=\(attempt2.outW) outH=\(attempt2.outH) outBpr=\(attempt2.outBpr) expect=\(sz.w)x\(sz.h) bpr=\(sz.bpr)")
      
      guard attempt2.ok else {
        logAppend?("snap: EARLY RETURN (retry failed)")
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
  
  func noteDamage(xid: UInt32, x: Int32, y: Int32, w: Int32, h: Int32) {
    // later: union dirty rects here
    schedulePresent(xid: xid)
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

    guard !closingXids.contains(host) else { return }
    guard windows[host] != nil else { return }
    guard mappedXids.contains(host) else { return }

    // Track the most recent source drawable for this host.
    latestSourceByHost[host] = xid

    if pendingPresentByXid.contains(host) { return }
    pendingPresentByXid.insert(host)

    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.pendingPresentByXid.remove(host)
      guard !self.closingXids.contains(host) else { return }
      guard self.windows[host] != nil else { return }
      guard self.mappedXids.contains(host) else { return }

      var source = self.latestSourceByHost[host] ?? host
      // Sanity guard: only present a source drawable that belongs to this host.
      if self.topLevelAncestor(of: source) != host {
        source = host
        self.latestSourceByHost[host] = host
      }
      self.snapshotAndPresentNow(sourceXid: source, presentXid: host)    }
  }
}



extension WindowRegistry {
  func presentBGRA(xid: UInt32, data: Data, width: Int, height: Int, bytesPerRow: Int) {
    guard let controller = windows[xid] else { return }
    guard let view = controller.x11View else { return }

    // `X11View.presentBGRA` copies the bytes internally, so it’s safe to pass a pointer
    // that’s only valid for the duration of this closure.
    data.withUnsafeBytes { raw in
      guard let base = raw.baseAddress else { return }
      logAppend?("snap: calling presentBGRA xid=0x\(String(xid, radix: 16)) w=\(width) h=\(height) bpr=\(bytesPerRow)")
      view.presentBGRA(framebuffer: base, width: width, height: height, bytesPerRow: bytesPerRow)
    }
  }
}


