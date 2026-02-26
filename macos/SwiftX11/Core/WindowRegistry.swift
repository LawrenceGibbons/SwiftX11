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

  // resize related
  private var pendingX11Resize: [UInt32: (w: CGFloat, h: CGFloat)] = [:]
  private var resizeWorkScheduled: Set<UInt32> = []
  
  // When X11 drives a resize, swallow Cocoa resize callbacks until we converge.
  private var suppressCocoaResizeExpected: [UInt32: (wX11: Int32, hX11: Int32)] = [:]
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
  
  //var useMetalForNewWindows: Bool = false  // set from UI on launch / changes
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

    logAppend?("[MAP] host=0x\(String(host, radix:16)) mapped; awaiting X11_UI_RESIZE")
    
    mappedXids.insert(host)

    X11View.logIfInLayout("mapWindow: orderFront host=0x\(String(host, radix: 16))",
                          view: controller.x11View)

    suppressNextMapFromCocoa.insert(host)
    win.orderFront(nil)

    schedulePresent(xid: host)
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

    suppressNextUnmapFromCocoa.insert(host)
    controller.window?.orderOut(nil)
  }
  
  private struct DirtyRectU {
    var x0: Int32
    var y0: Int32
    var x1: Int32   // exclusive
    var y1: Int32   // exclusive

    mutating func union(x: Int32, y: Int32, w: Int32, h: Int32) {
      guard w > 0, h > 0 else { return }
      let nx0 = x
      let ny0 = y
      let nx1 = x &+ w
      let ny1 = y &+ h

      x0 = min(x0, nx0)
      y0 = min(y0, ny0)
      x1 = max(x1, nx1)
      y1 = max(y1, ny1)
    }

    mutating func clamp(maxW: Int32, maxH: Int32) {
      guard maxW > 0, maxH > 0 else { return }
      x0 = max(0, min(x0, maxW))
      y0 = max(0, min(y0, maxH))
      x1 = max(0, min(x1, maxW))
      y1 = max(0, min(y1, maxH))
      if x1 < x0 { x1 = x0 }
      if y1 < y0 { y1 = y0 }
    }

    var isEmpty: Bool { x1 <= x0 || y1 <= y0 }
  }

  private var dirtyByHostU: [UInt32: DirtyRectU] = [:]


  private func hostSizeU(_ host: UInt32) -> (w: Int32, h: Int32)? {
    if let s = latestHostSizePtByXid[host] { return s }
    if let s = lastSentHostSizePtByXid[host] { return s }
    return nil
  }
  
  @MainActor
  func noteDamageRect(xid: UInt32, x: Int32, y: Int32, w: Int32, h: Int32) {
    guard xid != 0 else { return }

    let host = topLevelAncestor(of: xid)

    if debugSnapshotRouting {
      let lastid = latestSourceByHost[host] ?? host
      logAppend?(
        "[DAMAGE RECT] xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) " +
        "mappedHost=\(mappedXids.contains(host)) hasWindowHost=\(windows[host] != nil) " +
        "latestSourceByHost[host]=0x\(String(lastid, radix:16)) rect=\(x),\(y) \(w)x\(h)"
      )
    }

    // For now: treat damage rect as host-space only when xid==host.
    // If xid!=host, you can later translate child->host using geometry.
    var rx = x, ry = y, rw = w, rh = h

    if xid != host {
      // Safe fallback: mark whole host (correct, just less efficient)
      rx = 0; ry = 0
      if let sz = hostSizeU(host) {
        rw = sz.w; rh = sz.h
      } else {
        // last resort: ensure non-degenerate
        rw = max(1, rw); rh = max(1, rh)
      }
    }

    guard rw > 0, rh > 0 else { return }

    var d = dirtyByHostU[host] ?? DirtyRectU(x0: rx, y0: ry, x1: rx, y1: ry)
    d.union(x: rx, y: ry, w: rw, h: rh)

    if let sz = hostSizeU(host) {
      d.clamp(maxW: sz.w, maxH: sz.h)
    }

    if !d.isEmpty {
      dirtyByHostU[host] = d
    }

    // Schedule present for the HOST (your compositor snapshots host anyway)
    schedulePresent(xid: host)
  }  
  
  @MainActor
  func noteX11WindowDestroyed(xid: UInt32) {
    // 0) Cancel any scheduled present/snapshot bookkeeping for this xid as either host or source.
    pendingPresentByXid.remove(xid)
    pendingPresentByHost.remove(xid)
    latestSourceByHost.removeValue(forKey: xid)
    ignoreCocoaResizeUntilMapped.remove(xid)
    dirtyByHostU.removeValue(forKey: xid)
    
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
  
  
  private func snapshotAndPresentNow(sourceXid: UInt32, presentXid: UInt32, damageRect: DamageRect? = nil) {
    guard windows[presentXid] != nil else {
      print(String(format: "[SNAPSHOT] presentXid=0x%08X SKIP (no window entry)", presentXid))
      return
    }
    guard !closingXids.contains(presentXid) else { return }

    func querySize() -> (w: Int32, h: Int32, bpr: Int32)? {
      var w: Int32 = 0, h: Int32 = 0, bpr: Int32 = 0
      _ = x11_server_copy_window_bgra(sourceXid, nil, 0, &w, &h, &bpr)
      guard w > 0, h > 0, bpr > 0 else { return nil }
      return (w, h, bpr)
    }

    guard var sz = querySize() else {
      print(String(format: "[SNAPSHOT] sourceXid=0x%08X presentXid=0x%08X SKIP (querySize returned nil)",
            sourceXid, presentXid))
      return
    }
    print(String(format: "[SNAPSHOT] sourceXid=0x%08X presentXid=0x%08X sz=%dx%d bpr=%d",
          sourceXid, presentXid, sz.w, sz.h, sz.bpr))

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

    logAppend?("[SNAP_Q] host=0x\(String(presentXid,radix:16)) source=0x\(String(sourceXid,radix:16)) sz=\(sz.w)x\(sz.h) bpr=\(sz.bpr)")
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
                width: Int(sz.w), height: Int(sz.h), bytesPerRow: Int(sz.bpr),
                damageRect: damageRect)
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
    // Normalize to the top-level host. Configure/resize only applies to Cocoa host windows.
    let host = hostXid(xid)

    if debugSnapshotRouting, host != xid {
      logAppend?("[SIZE] normalize sendConfigureAsync child=0x\(String(xid, radix:16)) -> host=0x\(String(host, radix:16))")
    }

    let w = max(1, w)
    let h = max(1, h)

    let scale = windows[host]?.window?.backingScaleFactor ?? -1
    logAppend?("[CFG_SEND] host=0x\(String(host,radix:16)) sendX11=\(w)x\(h)pt winScale=\(scale)")
    
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
      logAppend?("[RESIZE Cocoa→X11] xid=0x\(String(xid, radix:16)) host=0x\(String(host, radix:16)) wPx=\(wPx) hPx=\(hPx) wPt=\(wPt) hPt=\(hPt)")

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

    let scale = win.backingScaleFactor
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

  func setUseMetalForAllWindows(_ enabled: Bool) {
    useMetalForNewWindows = enabled

    for (xid, controller) in windows {
      controller.setUseMetal(enabled)

      guard let win = controller.window else { continue }
      let sizePoints = win.contentLayoutRect.size

      let wX11 = Int32(max(1, Int(sizePoints.width.rounded(.down))))
      let hX11 = Int32(max(1, Int(sizePoints.height.rounded(.down))))

      let host = hostXid(xid)
      guard mappedXids.contains(host) else { continue }

      // X11 units are points now
      sendConfigureAsync(xid: host, w: wX11, h: hX11)
    }
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

      // Consume the accumulated damage rect.
      // NOTE: damage rect geometry from C++ is currently unreliable (the damage
      // push carries only an XID, no rect).  Force full-frame uploads until the
      // C++ side is updated to propagate actual geometry through damageOrDirty.
      self.dirtyByHostU.removeValue(forKey: host)
      let damage: DamageRect? = nil   // TODO: use dirtyByHostU once C++ sends rects

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
      logAppend?("snap: calling presentBGRA xid=0x\(String(xid, radix: 16)) w=\(width) h=\(height) bpr=\(bytesPerRow)")
      view.presentBGRA(framebuffer: base, width: width, height: height, bytesPerRow: bytesPerRow,
                       damageRect: damageRect)
    }
  }
}


