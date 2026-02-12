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
  
  private var showDamageLogs: () -> Bool = { true }   // default

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

  // De-dupe for rootless resize enqueue
  private var lastRootlessSent: [UInt32: (w: Int32, h: Int32)] = [:]

  // resize related
  private var pendingX11Resize: [UInt32: (w: CGFloat, h: CGFloat)] = [:]
  private var resizeWorkScheduled: Set<UInt32> = []
  
  // When X11 drives a resize, swallow Cocoa resize callbacks until we converge.
  private var suppressCocoaResizeExpected: [UInt32: (wPx: Int32, hPx: Int32)] = [:]
  private var suppressCocoaResizeBudget:   [UInt32: Int] = [:]
  // xxx temp
  private var lastSizePrintTimeByXid: [UInt32: CFTimeInterval] = [:]
  
  // Debug: enable verbose snapshot diagnostics
  var debugSnapshotRouting: Bool = true
  
  private func isTopLevelX11Window(_ xid: UInt32) -> Bool {
    // Top-level means parent is the X11 root (1). If unknown, assume top-level
    // (defensive; better to show a window than hide it).
    if let info = infoByXid[xid] { return info.parentXid == rootXid }
    if let p = parentByXid[xid] { return p == rootXid }
    return true
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
  
  var useMetalForNewWindows: Bool = false  // set from UI on launch / changes
  
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

    // (C) One-time sync: tell X11 what Cocoa’s current pixel size is.
    if let sz = latestPixelSizeByXid[host] {
      sendConfigureAsync(xid: host, w: sz.w, h: sz.h)
    }

    mappedXids.insert(host)

    X11View.logIfInLayout("mapWindow: orderFront host=0x\(String(host, radix: 16))",
                          view: controller.x11View)

    suppressNextMapFromCocoa.insert(host)
    win.orderFront(nil)

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

    X11View.logIfInLayout("unmapWindow: orderOut host=0x\(String(host, radix: 16))", view: controller.x11View)

    suppressNextUnmapFromCocoa.insert(host)
    controller.window?.orderOut(nil)
  }
  
  
  @MainActor
  func noteDamageRect(xid: UInt32, x: Int32, y: Int32, w: Int32, h: Int32) {
    print("[DAMAGE RECT PRINT] entering noteDamageRect xid=0x\(String(xid, radix:16))")

    let host = topLevelAncestor(of: xid)
    let lastid =  latestSourceByHost[host] ?? host
    logAppend?("[DAMAGE RECT] xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) mappedHost=\(mappedXids.contains(host)) hasWindowHost=\(windows[host] != nil) latestSourceByHost[host]=0x\(String(lastid, radix:16))")

    noteDamage(xid: xid, x: x, y: y, w: w, h: h)
  }
  
  
  @MainActor
  func noteX11WindowDestroyed(xid: UInt32) {
    // 0) Cancel any scheduled present/snapshot bookkeeping for this xid as either host or source.
    pendingPresentByXid.remove(xid)
    pendingPresentByHost.remove(xid)
    latestSourceByHost.removeValue(forKey: xid)
    ignoreCocoaResizeUntilMapped.remove(xid)
    
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

    let controller = X11WindowController(
      xid: xid,
      title: info.title,
      width: info.width,
      height: info.height,
      useMetal: useMetalForNewWindows
    )
    ignoreCocoaResizeUntilMapped.insert(xid)

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

    latestPixelSizeByXid.removeValue(forKey: host)
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
  func handleDamageRect(xid: UInt32, x: Int32, y: Int32, w: Int32, h: Int32) {
    let hasWindow = (windows[xid] != nil)
    if showDamageLogs() == true {
      logAppend?("handleDamageRect: xid=0x\(String(xid, radix: 16).uppercased()) rect=(\(x),\(y)) \(w)x\(h) hasWindow=\(hasWindow)")
    }
    noteDamage(xid: xid, x: x, y: y, w: w, h: h)
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

    var samples: [UInt32] = sx.map { px(atX: $0, sy) }

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
      presentBGRA(xid: presentXid, data: dataToPresent,
                  width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
//      presentBGRA(xid: presentXid, data: a2.data,
//                  width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
      return
    }

    let dataToPresent = a1.data 
    let scan = scanBGRA(dataToPresent, width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
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
                width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))

//    presentBGRA(xid: presentXid, data: a1.data,
//                width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr))
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
    let host = topLevelAncestor(of: xid)

    guard let controller = windows[xid], let win = controller.window else { return }
    
    X11View.logIfInLayout("raiseWindow: makeKeyAndOrderFront host=0x\(String(host, radix: 16))", view: controller.x11View)

    // Suppress the next didBecomeKey notification since we're causing it.
    suppressNextRaiseFromCocoa.insert(xid)
    print("[MAKEKEY] abiout to makeKeyAndOrderFront window=\(String(describing: win))")

    win.makeKeyAndOrderFront(nil)
  }
  
  private func sendConfigureAsync(xid: UInt32, w: Int32, h: Int32) {
    // xxx temp ----
    let host = hostXid(xid)
    if host != xid {
      print("[SIZE] WARNING sendConfigureAsync called with child xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16))")
    }
    // xxx ---- temp
    let w = max(1, w)
    let h = max(1, h)
    
    suppressExpectedSize[xid] = (w: w, h: h)
    suppressBudget[xid] = 8   // swallow a few intermediate callbacks

    DispatchQueue.main.async {
      x11_post_window_resize(xid, w, h)
    }
  }
  
  func windowResized(xid: UInt32, sizePoints _: CGSize, sizePixels: CGSize, scale _: CGFloat) {

    let host = topLevelAncestor(of: xid)
    
    // Ignore bogus transient sizes (prevents Metal 0-height)
    if sizePixels.width < 1 || sizePixels.height < 1 {
      return
    }

    let w = Int32(max(1, Int(sizePixels.width.rounded(.down))))
    let h = Int32(max(1, Int(sizePixels.height.rounded(.down))))

    // HARD GATE: until X11 maps the host window, never echo Cocoa→X11 resizes.
    if !mappedXids.contains(host) {
      latestPixelSizeByXid[host] = (w: w, h: h)
      return
    }

    // If we're still bootstrapping this window (not mapped yet), do NOT echo size back into X11.
    // Just remember the latest pixel size and return.
//    if ignoreCocoaResizeUntilMapped.contains(xid) {
    if ignoreCocoaResizeUntilMapped.contains(host) {
      let w = Int32(max(1, Int(sizePixels.width.rounded(.down))))
      let h = Int32(max(1, Int(sizePixels.height.rounded(.down))))
//      latestPixelSizeByXid[xid] = (w: w, h: h)
      latestPixelSizeByXid[host] = (w: w, h: h)
      return
    }
    
    // If we’re in an X11-driven resize cascade, never echo Cocoa→X11.
    // BUT we DO want to observe convergence and clear suppression.
    //if let exp = suppressCocoaResizeExpected[xid] {
    if let exp = suppressCocoaResizeExpected[host] {

      if exp.wPx == w && exp.hPx == h {
        // Converged: clear suppression + allow normal resizes again.
        //suppressCocoaResizeExpected.removeValue(forKey: xid)
        //suppressCocoaResizeBudget.removeValue(forKey: xid)
        //suppressNextResizeFromCocoa.remove(xid)
        //applyingX11Resize.remove(xid)
        //// Keep latestPixelSize in sync
        //latestPixelSizeByXid[xid] = (w: w, h: h)
        suppressCocoaResizeExpected.removeValue(forKey: host)
        suppressCocoaResizeBudget.removeValue(forKey: host)
        suppressNextResizeFromCocoa.remove(host)
        applyingX11Resize.remove(host)
        // Keep latestPixelSize in sync
        latestPixelSizeByXid[host] = (w: w, h: h)
        return
      }

//      let b = suppressCocoaResizeBudget[xid] ?? 0
//      if b > 0 {
//        suppressCocoaResizeBudget[xid] = b - 1
//        // Still updating latest helps later “no change” checks
//        latestPixelSizeByXid[xid] = (w: w, h: h)
//        return
//      }
      let b = suppressCocoaResizeBudget[host] ?? 0
      if b > 0 {
        suppressCocoaResizeBudget[host] = b - 1
        // Still updating latest helps later “no change” checks
        latestPixelSizeByXid[host] = (w: w, h: h)
        return
      }

      // Fail-safe: give up suppression if it never converges.
//      suppressCocoaResizeExpected.removeValue(forKey: xid)
//      suppressCocoaResizeBudget.removeValue(forKey: xid)
//      suppressNextResizeFromCocoa.insert(xid)
      suppressCocoaResizeExpected.removeValue(forKey: host)
      suppressCocoaResizeBudget.removeValue(forKey: host)
      suppressNextResizeFromCocoa.insert(host)

      // Important: don’t clear applyingX11Resize here; we’re no longer “expecting”
      // but we still want to avoid immediate ping-pong for 1 callback.
    }

    // If we are actively applying X11 resize, swallow Cocoa callbacks.
//    if applyingX11Resize.contains(xid) {
//      latestPixelSizeByXid[xid] = (w: w, h: h)
//      return
//    }
    if applyingX11Resize.contains(host) {
      latestPixelSizeByXid[host] = (w: w, h: h)
      return
    }

    // One-shot suppression damping
//    if shouldSuppressResizeFromCocoa(xid: xid) {
//      latestPixelSizeByXid[xid] = (w: w, h: h)
//      return
//    }
    if shouldSuppressResizeFromCocoa(xid: host) {
      latestPixelSizeByXid[host] = (w: w, h: h)
      return
    }

//    // Now “no change” short-circuit is safe
//    if let last = latestPixelSizeByXid[xid], last.w == w && last.h == h {
//      return
//    }
//
//    latestPixelSizeByXid[xid] = (w: w, h: h)

    // Now “no change” short-circuit is safe
    if let last = latestPixelSizeByXid[host], last.w == w && last.h == h {
      return
    }

    latestPixelSizeByXid[host] = (w: w, h: h)

    // Throttle: allow at most ~30fps during live resize
    let now = CACurrentMediaTime()
//    let lastT = lastRepaintTimeByXid[xid] ?? 0
    let lastT = lastRepaintTimeByXid[host] ?? 0
    if now - lastT >= (1.0 / 30.0) {
//      lastRepaintTimeByXid[xid] = now
//      repaintWorkItemByXid[xid]?.cancel()
//      repaintWorkItemByXid.removeValue(forKey: xid)
      lastRepaintTimeByXid[host] = now
      repaintWorkItemByXid[host]?.cancel()
      repaintWorkItemByXid.removeValue(forKey: host)


      guard mappedXids.contains(host) else { return }
      //if !mappedXids.contains(xid) { return }

      // Debug
      let xidHex = String(format:"0x%X", xid)
      print("[SIZE][Cocoa→X11] xid=\(xidHex) host=0x\(String(host, radix:16)) px=\(w)x\(h)")

//      sendConfigureAsync(xid: xid, w: w, h: h)
      sendConfigureAsync(xid: host, w: w, h: h)
      return
    }

    // Debounce to the final size shortly
//    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid[host]?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self else { return }
      //guard let sz = self.latestPixelSizeByXid[xid] else { return }
      //
      //guard self.mappedXids.contains(xid) else { return }
//
      //self.lastRepaintTimeByXid[xid] = CACurrentMediaTime()
      //self.sendConfigureAsync(xid: xid, w: sz.w, h: sz.h)
      guard let sz = self.latestPixelSizeByXid[host] else { return }
      
      guard self.mappedXids.contains(host) else { return }

      self.lastRepaintTimeByXid[host] = CACurrentMediaTime()
      self.sendConfigureAsync(xid: host, w: sz.w, h: sz.h)
    }
    //repaintWorkItemByXid[xid] = work
    repaintWorkItemByXid[host] = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.02, execute: work)
  }
  
  
  @MainActor
  func applyX11Resize(xid: UInt32, wPx: Int32, hPx: Int32) {
    
    let host = topLevelAncestor(of: xid)

    guard let controller = windows[host],
          let win = controller.window,
          let _ = controller.x11View else { return }

    // Suppress Cocoa echo immediately
    suppressCocoaResizeExpected[host] = (wPx: wPx, hPx: hPx)
    suppressCocoaResizeBudget[host] = 12
    suppressNextResizeFromCocoa.insert(host)

    let scale = win.backingScaleFactor
    let wPoints = max(1.0, CGFloat(wPx) / scale)
    let hPoints = max(1.0, CGFloat(hPx) / scale)

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
          self?.applyX11Resize(xid: host, wPx: wPx, hPx: hPx)
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
      
      // Mark “we are applying” and keep it set until windowResized sees convergence.
      self.applyingX11Resize.insert(host)
      win.setContentSize(NSSize(width: sz.w, height: sz.h))
    }
  }
  
  func flushRepaintNow(xid: UInt32) {
    let host = hostXid(xid)

    repaintWorkItemByXid[host]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: host)

    guard let sz = latestPixelSizeByXid[host] else { return }
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
  
  func setUseMetalForAllWindows(_ enabled: Bool) {
    useMetalForNewWindows = enabled
    
    for (xid, controller) in windows {
      controller.setUseMetal(enabled)
      
      if let win = controller.window {
        let sizePoints = win.contentLayoutRect.size
        let scale = win.backingScaleFactor
        let wPx = Int32(max(1, Int((sizePoints.width * scale).rounded(.down))))
        let hPx = Int32(max(1, Int((sizePoints.height * scale).rounded(.down))))
        let host = hostXid(xid)
        guard mappedXids.contains(host) else { continue }
        sendConfigureAsync(xid: host, w: wPx, h: hPx)
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
    self.logAppend?("PRESENT_REQ xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) mappedHost=\(self.mappedXids.contains(host)) hasWin=\(self.windows[host] != nil)")

    guard !closingXids.contains(host) else { return }
    guard windows[host] != nil else { return }
    guard mappedXids.contains(host) else { return }

    // Track the most recent source drawable for this host.
    latestSourceByHost[host] = xid
    if debugSnapshotRouting {
      logAppend?("[PRESENT_DBG] set latestSourceByHost[0x\(String(host, radix:16))] = 0x\(String(xid, radix:16))")
    }
    if pendingPresentByXid.contains(host) { return }
    pendingPresentByXid.insert(host)

    DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in
      guard let self else { return }

      // CRITICAL: never leave host stuck “pending”
      defer { self.pendingPresentByXid.remove(host) }

      guard !self.closingXids.contains(host) else { return }
      guard self.windows[host] != nil else { return }
      guard self.mappedXids.contains(host) else { return }

      let source = self.latestSourceByHost[host] ?? host
      if debugSnapshotRouting {
        self.logAppend?("[PRESENT_DBG] firing host=0x\(String(host, radix:16)) source=0x\(String(source, radix:16)) tl(source)=0x\(String(self.topLevelAncestor(of: source), radix:16))")
      }
      // TEMP: for xeyes only: if host has a latestSource and it's a child, snapshot it
      let src = latestSourceByHost[host] ?? host
      snapshotAndPresentNow(sourceXid: src, presentXid: host)
// xxx temp      self.snapshotAndPresentNow(sourceXid: source, presentXid: host)
    }
    //DispatchQueue.main.async { [weak self] in
    //  guard let self else { return }
    //  self.pendingPresentByXid.remove(host)
    //  guard !self.closingXids.contains(host) else { return }
    //  guard self.windows[host] != nil else { return }
    //  guard self.mappedXids.contains(host) else { return }
    //  
    //  let source = self.latestSourceByHost[host] ?? host
    //  // Sanity guard: only present a source drawable that belongs to this host.
    //  // xxx temp ----
    //  if self.topLevelAncestor(of: source) != host {
    //    self.logAppend?("⚠️ PRESENT source mismatch: host=0x\(String(host, radix:16)) source=0x\(String(source, radix:16)) topLevel(source)=0x\(String(self.topLevelAncestor(of: source), radix:16)) parentKnown=\(self.parentByXid[source] != nil)")
    //  }
    //  // xxx ---- temp
    //  // xxx temp for debugging if self.topLevelAncestor(of: source) != host {
    //  // xxx temp for debugging   source = host
    //  // xxx temp for debugging   self.latestSourceByHost[host] = host
    //  // xxx temp for debugging }
    //  // xxx temp for debugging logAppend?("[PRESENT_FIRE] host=0x\(String(host, radix:16)) source=0x\(String(source, radix:16))")
    //  // xxx temp for debugging self.snapshotAndPresentNow(sourceXid: source, presentXid: host)    }
    //  // xxx temp ----
    //  //let source = self.latestSourceByHost[host] ?? host
    //  let src = self.latestSourceByHost[host] ?? host
    //  self.logAppend?("[PRESENT_FIRE] host=0x\(String(host, radix:16)) src=0x\(String(src, radix:16)) tl(src)=0x\(String(self.topLevelAncestor(of: src), radix:16))")
    //  self.snapshotAndPresentNow(sourceXid: source, presentXid: host)
    //  // xxx ---- temp
    //}
  }
  
  
  @MainActor
  func shouldSuppressRootlessResize(xid: UInt32, w_px: Int32, h_px: Int32) -> Bool {
      guard let exp = suppressExpectedSize[xid] else { return false }

      // If we've reached the requested size, consume suppression.
      if exp.w == w_px && exp.h == h_px {
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


