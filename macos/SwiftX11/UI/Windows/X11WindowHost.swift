import SwiftUI
import AppKit
import Cocoa
import MetalKit
import CoreGraphics
import X11LowLevel


final class X11MTKView: MTKView {
  weak var owner: X11View?
  var xid: UInt32 = 0
  
  var currentCursor: NSCursor = .arrow {
    didSet {
      self.discardCursorRects()

      window?.invalidateCursorRects(for: self)

      // If mouse is already inside this view, apply immediately.
      if let win = window {
        let p = convert(win.mouseLocationOutsideOfEventStream, from: nil)
        if bounds.contains(p) { currentCursor.set() }
      }
    }
  }

  override func resetCursorRects() {
    super.resetCursorRects()
    addCursorRect(bounds, cursor: currentCursor)
  }


  override var acceptsFirstResponder: Bool { true }
  
  override func mouseDown(with event: NSEvent) { owner?.mouseDown(with: event) }
  override func mouseUp(with event: NSEvent) { owner?.mouseUp(with: event) }
  override func rightMouseDown(with event: NSEvent) { owner?.rightMouseDown(with: event) }
  override func rightMouseUp(with event: NSEvent) { owner?.rightMouseUp(with: event) }
  override func otherMouseDown(with event: NSEvent) { owner?.otherMouseDown(with: event) }
  override func otherMouseUp(with event: NSEvent) { owner?.otherMouseUp(with: event) }
  
  override func mouseMoved(with event: NSEvent) { owner?.mouseMoved(with: event) }
  override func mouseDragged(with event: NSEvent) { owner?.mouseDragged(with: event) }
  override func rightMouseDragged(with event: NSEvent) { owner?.rightMouseDragged(with: event) }
  override func otherMouseDragged(with event: NSEvent) { owner?.otherMouseDragged(with: event) }
  
  override func mouseEntered(with event: NSEvent) { owner?.mouseEntered(with: event) }
  override func mouseExited(with event: NSEvent) { owner?.mouseExited(with: event) }
  
  override func scrollWheel(with event: NSEvent) { owner?.scrollWheel(with: event) }
  
  override func keyDown(with event: NSEvent) { owner?.keyDown(with: event) }
  override func keyUp(with event: NSEvent) { owner?.keyUp(with: event) }
  override func flagsChanged(with event: NSEvent) { owner?.flagsChanged(with: event) }
}

final class X11View: NSView {
  // MARK: - Mode
  var usingMetal: Bool = false
  var wantsMetal: Bool = false
  
  // MARK: - Software path
  private var imageLayer: CALayer?
  private var lastFrameData: Data?
  private var trackingArea: NSTrackingArea?
  
  // MARK: - Metal path
  private var mtkView: X11MTKView?
  private var device: MTLDevice?
  private weak var trackingHost: NSView?
  private var renderer: X11MetalRenderer?
  private var mtkDelegate: X11Renderer?
  // Avoid triggering MTKView delegate callbacks during NSView.layout (can cause layout recursion).
  private var pendingDrawableSize: CGSize?
  private var drawableSizeUpdateScheduled: Bool = false
  private var pendingEnableMetal = false
  
  // MARK: -- interface to X11
  private var buttonMask: UInt32 = 0
  private var scrollAccumX: CGFloat = 0
  private var scrollAccumY: CGFloat = 0
  private var lastModifierFlags: NSEvent.ModifierFlags = []
  
  // MARK: -- be authoritative for the tracking events 
  private var lastInsideForSyntheticCrossing: Bool = false
  private var isPointerInside: Bool = false
  
  
  // MARK: -- know which window to use
  var xid: UInt32 = 0 {
    didSet {
      // Propagate xid to the MTKView and delegate.
      mtkView?.xid = xid
      mtkDelegate?.setXid(xid)
    }
  }
  private var didNotifyPresentable = false
  override var acceptsFirstResponder: Bool { true }
  
  
  // MARK: -- suppress circular rootless resize calls
  // When the server drives a configure resize, we suppress rootless resize callbacks
  // until the drawable reaches the expected pixel size (or a small budget expires).
  private var suppressBudget: Int = 0
  
  override init(frame frameRect: NSRect) {
    super.init(frame: frameRect)
    wantsLayer = true
    setupSoftwareLayer()
    // Don’t setup metal yet until setUseMetal() is called.
  }
  
  
  // MARK: -- debug
#if DEBUG
  private var swFrameSeq: UInt64 = 0
  private var swLastAppliedSeq: UInt64 = 0
  private var swLastAppliedWH: (w: Int, h: Int, bpr: Int) = (0, 0, 0)
#endif
  
  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }
  
  private var applyingDrawableSize = false
  
  private func scheduleDrawableSizeUpdate(_ size: CGSize) {
    pendingDrawableSize = size
    guard !drawableSizeUpdateScheduled else { return }
    drawableSizeUpdateScheduled = true
    
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.drawableSizeUpdateScheduled = false
      guard let mv = self.mtkView else { return }
      guard let size = self.pendingDrawableSize else { return }
      self.pendingDrawableSize = nil
      
      guard !self.applyingDrawableSize else { return }
      self.applyingDrawableSize = true
      defer { self.applyingDrawableSize = false }
      
      logIfInLayout( "In scheduleDrawableSizeUpdate (size: \(size))", view:self)
      if mv.drawableSize != size {
        print("[MTK] drawableSize about to set xid=\(xid) size=\(size) window=\(String(describing: mv.window))")
        mv.drawableSize = size
        //mv.drawableSize = size
      }
    }
  }  
  
  
  final class ThreadLocalInt {
    private let key = "SwiftX11.layoutDepthTLS.\(UUID().uuidString)"
    var value: Int {
      get { Thread.current.threadDictionary[key] as? Int ?? 0 }
      set { Thread.current.threadDictionary[key] = newValue }
    }
  }
  private static var layoutDepthTLS = ThreadLocalInt()
  
  private var inLayout: Bool { Self.layoutDepthTLS.value > 0 }
  var isInLayoutPass: Bool { inLayout }
  
  override func layout() {
    Self.layoutDepthTLS.value += 1
    defer { Self.layoutDepthTLS.value -= 1 }
    super.layout()
    
    imageLayer?.frame = bounds
    mtkView?.frame = bounds
    
    if pendingEnableMetal, wantsMetal,
       self.window != nil,
       bounds.width >= 1, bounds.height >= 1 {
      pendingEnableMetal = false
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        self.setUseMetal(true)
      }
    }
  }
  
  final class LayoutLogGate {
    static let shared = LayoutLogGate()
    private var lastTick = 0
    
    func shouldLog() -> Bool {
      let tick = CFRunLoopGetCurrent().hashValue
      if tick == lastTick { return false }
      lastTick = tick
      return true
    }
  }
  
  func logIfInLayout(_ label: String, view: X11View?) {
    if view == nil {
      print("[LAYOUT?] \(label) (view=nil)")
      return
    }
    
    guard let view, view.inLayout else { return }
    guard LayoutLogGate.shared.shouldLog() else { return }
    
    print("⚠️ [SwiftX11] \(label) called during layout")
    Thread.callStackSymbols.prefix(25).forEach { print($0) }
  }
  
  final class LogOncePerTick {
    static let shared = LogOncePerTick()
    private var lastTick = 0
    private var lastLabel: String?
    
    func shouldLog(_ label: String) -> Bool {
      let tick = CFRunLoopGetCurrent().hashValue // cheap-ish “tick-ish” marker
      if tick == lastTick, lastLabel == label { return false }
      lastTick = tick
      lastLabel = label
      return true
    }
  }
  
  private var pendingMakeFirstResponder = false
  
  private func requestFirstResponderCoalesced() {
    guard !pendingMakeFirstResponder else { return }
    pendingMakeFirstResponder = true
    
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.pendingMakeFirstResponder = false
      
      guard let win = self.window else { return }
      
      // Only attempt if we’re not already first responder.
      if win.firstResponder !== self {
        logIfInLayout("About to makeFirstResponder for xid=0x\(String(self.xid, radix: 16).uppercased())", view: self)
        print("[FRSTRESP] abiout to makeFirstResponder window=\(String(describing: self.window))")
        
        win.makeFirstResponder(self)
      }
    }
  }  
  
  private var attachSettleScheduled = false
  private weak var lastKnownWindow: NSWindow?
  
  override func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    
    // Prevent re-running attach logic for the same window repeatedly.
    let w = self.window
    if lastKnownWindow === w, attachSettleScheduled {
      return
    }
    lastKnownWindow = w
    
    scheduleAttachSettle()
  }
  
  
  private func syncSoftwareLayerScale() {
    let s = window?.backingScaleFactor ?? 1.0
    // Make both the root layer and the image sublayer match.
    self.layer?.contentsScale = s
    self.imageLayer?.contentsScale = s
  }
  
  
  override func viewDidChangeBackingProperties() {
    super.viewDidChangeBackingProperties()
    syncSoftwareLayerScale()
  }
  
  
  private func notifyPresentableOnce() {
    guard !didNotifyPresentable else { return }
    guard self.window != nil else { return }
    guard xid != 0 else { return }
    didNotifyPresentable = true
    x11_post_window_presentable(xid)
  }
  
  private func scheduleAttachSettle() {
    guard !attachSettleScheduled else { return }
    attachSettleScheduled = true
    
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.attachSettleScheduled = false
      
      // fix scale once we truly have a window
      self.syncSoftwareLayerScale()
      
      // All “attach-time” mutating work happens here, never inside viewDidMoveToWindow.
      self.window?.acceptsMouseMovedEvents = true
      
      // Don’t set responder synchronously on attach; coalesce.
      self.requestFirstResponderCoalesced()
      
      // Don’t refresh tracking areas synchronously on attach; coalesce.
      self.requestTrackingRefreshCoalesced()
      
      
      // make host presentable in software or metal mode
      self.notifyPresentableOnce()
      
    }
  }
  
  
  private var trackingRefreshScheduled = false
  
  private func requestTrackingRefreshCoalesced() {
    guard !trackingRefreshScheduled else { return }
    trackingRefreshScheduled = true
    
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.trackingRefreshScheduled = false
      self._refreshTrackingAreas()
    }
  }
  
  
  @objc private func _refreshTrackingAreas() {
    refreshTrackingArea()
  }
  
  override func updateTrackingAreas() {
    logIfInLayout("updateTrackingAreas()", view: self)
    
    super.updateTrackingAreas()
    // DO NOT mutate tracking areas synchronously here.
    // updateTrackingAreas is often invoked during layout.
    requestTrackingRefreshCoalesced()
    
    //        refreshTrackingArea()
  }  
  
  override func hitTest(_ point: NSPoint) -> NSView? {
    // When Metal is active, route input to the MTKView so it can forward to us.
    if usingMetal, let mv = mtkView {
      return mv
    }
    return super.hitTest(point)
  }
  
  // MARK: - Public
  func setUseMetal(_ enabled: Bool) {
    wantsMetal = enabled
    
    if !enabled {
      pendingEnableMetal = false   // ✅ cancel any deferred enable
    }
    
    if enabled {
      // Defer Metal until we have a real AppKit window and non-zero bounds
      guard self.window != nil else {
        pendingEnableMetal = true
        return
      }
      guard bounds.width >= 1, bounds.height >= 1 else {
        pendingEnableMetal = true
        return
      }
    }
    
    if enabled, let dev = MTLCreateSystemDefaultDevice() {
      // Switch to Metal
      if !usingMetal {
        setupMetal(device: dev)
        usingMetal = true
      }
      imageLayer?.isHidden = true
      mtkView?.isHidden = false
    } else {
      // Switch to Software
      usingMetal = false
      mtkView?.isHidden = true
      imageLayer?.isHidden = false
    }
    
    // refreshTrackingArea()
    requestTrackingRefreshCoalesced()
  }
  
  /// Back-compat entrypoint used by older callers.
  /// Treats the incoming buffer as BGRA8888 little-endian.
  func updateFrameBuffer(_ framebuffer: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    presentBGRA(framebuffer: framebuffer, width: width, height: height, bytesPerRow: bytesPerRow)
  }
  
  /// Present BGRA8888 little-endian framebuffer.
  ///
  /// Callers may pass a pointer that is only valid for the duration of this call.
  /// We always copy into `Data` first to decouple lifetime from the C backend.
  func presentBGRA(framebuffer: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    guard width > 0, height > 0, bytesPerRow > 0 else { return }
    
    // This is an NSView API; we expect to be called on the main thread.
    // WindowRegistry.schedulePresent runs on DispatchQueue.main, so this should hold.
    assert(Thread.isMainThread)
    
    let byteCount = bytesPerRow * height
    guard byteCount > 0 else { return }
    
    // Copy bytes so the backing memory remains valid after C returns/frees.
    let data = Data(bytes: framebuffer, count: byteCount)
    
    // debug only
    let p0: UInt32 = data.withUnsafeBytes { raw in
      raw.load(fromByteOffset: 0, as: UInt32.self)
    }
    print(String(format: "PRESENT xid=%u p0=0x%08X", xid, p0))
    
    
    if usingMetal {
      // Metal path: upload texture now; actual present happens in MTKViewDelegate.draw(in:)
      guard let mv = self.mtkView else { return }
      guard mv.drawableSize.width > 0, mv.drawableSize.height > 0 else { return }
      
      if usingMetal, let mv = self.mtkView {
        let ds = mv.drawableSize
        let scale = self.window?.backingScaleFactor ?? -1
        print("[PRESENT][Metal] xid=\(xid) src=\(width)x\(height) drawable=\(Int(ds.width))x\(Int(ds.height)) scale=\(scale)")
      }
      
      self.renderer?.updateTexture(with: data, width: width, height: height, bytesPerRow: bytesPerRow)
      
      // Ask MTKView to draw exactly once (draw(in:) will use currentDrawable and present).
      mv.setNeedsDisplay(mv.bounds)
    } else {
#if DEBUG
      swFrameSeq &+= 1
      let seq = swFrameSeq
      let tid = pthread_mach_thread_np(pthread_self())
      print(String(format: "[SWFRAME] recv xid=0x%08X seq=%llu wh=%dx%d bpr=%d tid=0x%X main=%d",
                   xid, seq, width, height, bytesPerRow, tid, Thread.isMainThread ? 1 : 0))
#endif
      // Software path: build a CGImage and put it into the layer.
      presentSoftware(data: data, width: width, height: height, bytesPerRow: bytesPerRow)
    }
  }
  
  // MARK: - Software rendering
  private func setupSoftwareLayer() {
    let layer = CALayer()
    layer.frame = bounds
    layer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
    layer.contentsGravity = .resize
    layer.magnificationFilter = .nearest
    layer.minificationFilter = .nearest
    self.layer?.addSublayer(layer)
    imageLayer = layer
  }
  
  private func presentSoftware(data: Data, width: Int, height: Int, bytesPerRow: Int) {
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bitmapInfo: CGBitmapInfo = [
      .byteOrder32Little,
      CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
    ]
    
    guard let provider = CGDataProvider(data: data as CFData) else { return }
    
    guard let cgImage = CGImage(
      width: width,
      height: height,
      bitsPerComponent: 8,
      bitsPerPixel: 32,
      bytesPerRow: bytesPerRow,
      space: colorSpace,
      bitmapInfo: bitmapInfo,
      provider: provider,
      decode: nil,
      shouldInterpolate: false,
      intent: .defaultIntent
    ) else { return }
    
#if DEBUG
    let seq = swFrameSeq   // or capture from caller
    let recvWH = (w: width, h: height, bpr: bytesPerRow)
#endif
    
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
#if DEBUG
      let tid = pthread_mach_thread_np(pthread_self())
      let last = self.swLastAppliedWH
      let lastSeq = self.swLastAppliedSeq
      let isStaleBySeq = seq < lastSeq
      let isStaleBySize = (recvWH.w < last.w || recvWH.h < last.h) && (last.w > 0 && last.h > 0)
      
      print(String(format:
                    "[SWFRAME] apply xid=0x%08X seq=%llu (last=%llu) wh=%dx%d bpr=%d lastWh=%dx%d lastBpr=%d staleSeq=%d staleSize=%d tid=0x%X",
                   self.xid, seq, lastSeq,
                   recvWH.w, recvWH.h, recvWH.bpr,
                   last.w, last.h, last.bpr,
                   isStaleBySeq ? 1 : 0,
                   isStaleBySize ? 1 : 0,
                   tid))
      
      self.swLastAppliedSeq = max(self.swLastAppliedSeq, seq)
      self.swLastAppliedWH = recvWH
#endif
      self.lastFrameData = data
      
#if DEBUG
      let layer = self.imageLayer
      print("""
        [LAYERMAP] xid=0x\(String(self.xid, radix:16)) \
        gravity=\(layer?.contentsGravity.rawValue ?? "nil") \
        contentsRect=\(layer?.contentsRect ?? .zero) \
        contentsCenter=\(layer?.contentsCenter ?? .zero) \
        contentsScale=\(layer?.contentsScale ?? -1) \
        winScale=\(self.window?.backingScaleFactor ?? -1)
        """)
      let winScale = self.window?.backingScaleFactor ?? -1
      let layerScale = layer?.contentsScale ?? -2
      let grav = layer?.contentsGravity.rawValue ?? "nil"
      let b = self.bounds
      let lf = layer?.frame ?? .zero
      print("""
              [SWAPPLY] xid=0x\(String(self.xid, radix:16)) \
              img=\(width)x\(height) bpr=\(bytesPerRow) \
              view.bounds=\(b) layer.frame=\(lf) \
              winScale=\(winScale) layerScale=\(layerScale) gravity=\(grav) \
              main=\(Thread.isMainThread)
              """)
#endif
      
      syncSoftwareLayerScale()
      self.imageLayer?.contents = cgImage
    }
  }
  
  // MARK: - Metal setup
  private func setupMetal(device: MTLDevice) {
    self.device = device
    
    let view = X11MTKView(frame: bounds, device: device)
    
    print("[MTK] create MTKView xid=\(xid) bounds=\(bounds) window=\(String(describing: self.window)) backingScale=\(self.window?.backingScaleFactor ?? -1)")
    
    view.owner = self
    view.xid = self.xid
    view.autoresizingMask = [.width, .height]
    
    let del = X11Renderer(owner: self)
    del.setXid(self.xid)
    self.mtkDelegate = del
    view.delegate = del
    
    view.enableSetNeedsDisplay = true
    view.isPaused = true
    view.framebufferOnly = false
    view.colorPixelFormat = .bgra8Unorm
    view.preferredFramesPerSecond = 0
    
    //self.renderer = X11MetalRenderer(device: device)
    self.renderer = X11MetalRenderer(view: view)
    
    addSubview(view, positioned: .above, relativeTo: nil)
    self.mtkView = view
    
    //if let win = self.window {
    //  let scale = win.backingScaleFactor
    //  let wF = bounds.width * scale
    //  let hF = bounds.height * scale
    //  if wF >= 1, hF >= 1 {
    //    view.drawableSize = CGSize(width: floor(wF), height: floor(hF))
    //  }
    //}
    
    // DO NOT touch drawableSize or setNeedsDisplay here.
    // Wait for MTKViewDelegate.mtkView(_:drawableSizeWillChange:) once the view is real.
    //// ✅ DO NOT kick draw synchronously here
    //DispatchQueue.main.async { [weak self] in
    //  guard let self else { return }
    //  guard let mv = self.mtkView else { return }
    //  guard mv.window != nil else { return }
    //
    //  let scale = mv.window?.backingScaleFactor ?? 1.0
    //  let wF = mv.bounds.width * scale
    //  let hF = mv.bounds.height * scale
    //  guard wF >= 1, hF >= 1 else { return }
    //
    //  let ds = CGSize(width: floor(wF), height: floor(hF))
    //  if mv.drawableSize != ds { mv.drawableSize = ds }
    //
    //  mv.setNeedsDisplay(mv.bounds)
    //}
  }
  
  private func kickMetalOnceIfReady() {
    guard usingMetal, let mv = mtkView else { return }
    guard mv.window != nil else { return }
    let ds = mv.drawableSize
    guard ds.width >= 1, ds.height >= 1 else { return }
    mv.setNeedsDisplay(mv.bounds)
  }
  
  
  private func refreshTrackingArea() {
    // Hard gate: never mutate tracking areas during layout.
    if inLayout {
      requestTrackingRefreshCoalesced()
      return
    }
    
    logIfInLayout("refreshTrackingArea (usingMetal=\(usingMetal))", view: self)
    
    if let trackingArea {
      trackingHost?.removeTrackingArea(trackingArea)
      self.trackingArea = nil
      trackingHost = nil
    }
    
    let target: NSView = (usingMetal ? (mtkView ?? self) : self)
    
    let opts: NSTrackingArea.Options = [
      .mouseEnteredAndExited,
      .mouseMoved,
      .cursorUpdate,
      .activeInActiveApp,
      .enabledDuringMouseDrag,
      .inVisibleRect
    ]
    
    let area = NSTrackingArea(rect: .zero, options: opts, owner: self, userInfo: nil)
    target.addTrackingArea(area)
    self.trackingArea = area
    self.trackingHost = target
  }
  
  override func cursorUpdate(with event: NSEvent) {
    currentCursor.set()
  }
  
  // MARK: X11 interface
  private func bitForButton(_ button: Int) -> UInt32 {
    // button: 1..8
    guard button >= 1 && button <= 31 else { return 0 }
    return 1 << UInt32(button - 1)
  }
  
  // MARK: - Metal draw entrypoint (called by X11Renderer)
  fileprivate func metalDraw(in view: MTKView) {
    // Single, authoritative presentation path:
    // MTKView calls X11Renderer.draw(in:), which forwards here.
    guard usingMetal else { return }
    guard view.drawableSize.width >= 1, view.drawableSize.height >= 1 else { return }
    guard let renderer = self.renderer else { return }
    print("MTK draw(in:) xid=\(xid) hasTex=\(renderer.hasTexture) drawable=\(String(describing: view.currentDrawable))")
    renderer.draw(on: view)
  }
  
  // MARK: - Input helpers
  private func mods(_ flags: NSEvent.ModifierFlags) -> UInt32 {
    var m: UInt32 = 0
    if flags.contains(.shift) { m |= 1 << 0 }
    if flags.contains(.control) { m |= 1 << 1 }
    if flags.contains(.option) { m |= 1 << 2 }
    if flags.contains(.command) { m |= 1 << 3 }
    return m
  }
  
  
  // private func pointInPixels(_ event: NSEvent, clampToView: Bool) -> (Int32, Int32) {
  //   // View coords in points (origin bottom-left)
  //   let pInWindow = event.locationInWindow
  //   let p = convert(pInWindow, from: nil)
  //   
  //   let scale = window?.backingScaleFactor ?? 1.0
  //   
  //   // Backing pixel dimensions of this view
  //   let wPt = max(1, Int(floor(bounds.width * scale)))
  //   let hPt = max(1, Int(floor(bounds.height * scale)))
  //   
  //   // Convert to pixels in Cocoa-up coordinates
  //   var xF = p.x * scale
  //   var yF = p.y * scale
  //   
  //   if clampToView {
  //     // Clamp in points then scale (matches bounds.contains logic)
  //     let clampedXPt = min(max(p.x, 0), bounds.width)
  //     let clampedYPt = min(max(p.y, 0), bounds.height)
  //     xF = clampedXPt * scale
  //     yF = clampedYPt * scale
  //     
  //     // Clamp in pixels to [0 .. sizePt-1] (still Cocoa-up)
  //     xF = min(max(xF, 0), CGFloat(wPt - 1))
  //     yF = min(max(yF, 0), CGFloat(hPt - 1))
  //   }
  //   
  //   // Flip Y to X11 top-down (0 at top)
  //   yF = CGFloat(hPt - 1) - yF
  //   
  //   if clampToView {
  //     // After flip, ensure still in range (paranoia)
  //     yF = min(max(yF, 0), CGFloat(hPt - 1))
  //   }
  //   
  //   return (Int32(floor(xF)), Int32(floor(yF)))
  // }
  
  private func pointInX11(_ event: NSEvent, clampToView: Bool) -> (Int32, Int32) {
    let pInWindow = event.locationInWindow
    let p = convert(pInWindow, from: nil) // points, origin bottom-left

    let wPt = bounds.width
    let hPt = bounds.height

    var xF = p.x
    var yF = p.y

    if clampToView {
      let clampedX = min(max(xF, 0), wPt)
      let clampedY = min(max(yF, 0), hPt)
      xF = clampedX
      yF = clampedY
      xF = min(max(xF, 0), wPt - 1)
      yF = min(max(yF, 0), hPt - 1)
    }

    // Flip Y to X11 top-down (0 at top) in *points*
    yF = (hPt - 1) - yF
    if clampToView {
      yF = min(max(yF, 0), hPt - 1)
    }

    return (Int32(floor(xF)), Int32(floor(yF)))
  }
  
  // private func rootPointInPixelsTopLeft() -> (Int32, Int32) {
  //   // Global mouse location in screen points (origin bottom-left of global space)
  //   let gp = NSEvent.mouseLocation
  //   
  //   // Virtual desktop bounds in points
  //   let screens = NSScreen.screens
  //   let vminX = screens.map { $0.frame.minX }.min() ?? 0
  //   let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0
  //   
  //   // Use scale of the screen containing the pointer if possible
  //   let screen = screens.first(where: { $0.frame.contains(gp) }) ?? NSScreen.main
  //   let scale = screen?.backingScaleFactor ?? 1.0
  //   
  //   // Root coords in pixels, top-left origin of virtual desktop
  //   let xPt = Int32(((gp.x - vminX) * scale).rounded(.toNearestOrAwayFromZero))
  //   let yPt = Int32(((vmaxY - gp.y) * scale).rounded(.toNearestOrAwayFromZero)) - 1
  //   
  //   return (max(0, xPt), max(0, yPt))
  // }
  
  
  private func rootPointInX11TopLeft() -> (Int32, Int32) {
    let gp = NSEvent.mouseLocation // points, origin bottom-left in global space

    let screens = NSScreen.screens
    let vminX = screens.map { $0.frame.minX }.min() ?? 0
    let vmaxY = screens.map { $0.frame.maxY }.max() ?? 0

    // Root coords in points, top-left origin of virtual desktop
    let x = Int32((gp.x - vminX).rounded(.toNearestOrAwayFromZero))
    let y = Int32((vmaxY - gp.y).rounded(.toNearestOrAwayFromZero)) - 1

    return (max(0, x), max(0, y))
  }
  
  // MARK: - Mouse
  private func sendMotion(_ event: NSEvent) {
    // Points
    let p = convert(event.locationInWindow, from: nil)
    let insideNow = bounds.contains(p)
    let dragging = (buttonMask != 0)
    
    // Enter/leave synth
    if insideNow && !isPointerInside {
      isPointerInside = true
      let (x, y) = pointInX11(event, clampToView: true)
      x11_post_pointer_enter(xid, x, y, mods(event.modifierFlags))
    } else if !insideNow && isPointerInside && !dragging {
      isPointerInside = false
      let (x, y) = pointInX11(event, clampToView: true)
      x11_post_pointer_leave(xid, x, y, mods(event.modifierFlags))
    }
    
    // controls whether we will send events.  Only send while inside, unless dragging
    let deliver: UInt8 = (insideNow || dragging) ? 1 : 0
    
    // Window-local coords:
    // - if inside, use true coords
    // - if outside and not dragging, clamp to edge so winX/winY stay sane
    let (winX, winY) = pointInX11(event, clampToView: !dragging)
    
    // Global root coords (top-left)
    let (rootX, rootY) = rootPointInX11TopLeft()
    
    GlobalPointerTracker.shared.updateActiveWindow(xid: xid, lastWinXY: (winX, winY))
    
    // Always update pointer state in server (so QueryPointer follows everywhere)
    x11_post_pointer_move2(xid, winX, winY, rootX, rootY, deliver, buttonMask, mods(event.modifierFlags))
    
  }
  
  private func sendButton(_ isPress: Bool, button: UInt8, _ event: NSEvent) {
    let (x, y) = pointInX11(event, clampToView: false)
    x11_post_pointer_button(xid, isPress, button, x, y, buttonMask, mods(event.modifierFlags))
  }
  
  override func mouseDown(with event: NSEvent) {
    lastInsideForSyntheticCrossing = true
    requestFirstResponderCoalesced()
    buttonMask |= bitForButton(1)
    sendButton(true, button: 1, event)
  }
  
  override func mouseUp(with event: NSEvent) {
    // report release while still "down", then clear
    sendButton(false, button: 1, event)
    buttonMask &= ~bitForButton(1)
  }
  
  override func rightMouseDown(with event: NSEvent) {
    lastInsideForSyntheticCrossing = true
    requestFirstResponderCoalesced()
    buttonMask |= bitForButton(3)
    sendButton(true, button: 3, event)
  }
  
  override func rightMouseUp(with event: NSEvent) {
    sendButton(false, button: 3, event)
    buttonMask &= ~bitForButton(3)
  }
  
  override func otherMouseDown(with event: NSEvent) {
    requestFirstResponderCoalesced()
    lastInsideForSyntheticCrossing = true
    
    // NSEvent.buttonNumber is 0-based; X11 uses 1-based
    let btn = UInt8(min(max(event.buttonNumber + 1, 1), 31))
    buttonMask |= bitForButton(Int(btn))
    sendButton(true, button: btn, event)
  }
  
  override func otherMouseUp(with event: NSEvent) {
    let btn = UInt8(min(max(event.buttonNumber + 1, 1), 31))
    sendButton(false, button: btn, event)
    buttonMask &= ~bitForButton(Int(btn))
  }
  
  override func mouseMoved(with event: NSEvent) { sendMotion(event) }
  override func mouseDragged(with event: NSEvent) { sendMotion(event) }
  override func rightMouseDragged(with event: NSEvent) { sendMotion(event) }
  override func otherMouseDragged(with event: NSEvent) { sendMotion(event) }
  
  override func mouseEntered(with event: NSEvent) {
    lastInsideForSyntheticCrossing = true
    isPointerInside = true
    let (x, y) = pointInX11(event, clampToView: true)
    x11_post_pointer_enter(xid, x, y, mods(event.modifierFlags))
  }
  
  override func mouseExited(with event: NSEvent) {
    lastInsideForSyntheticCrossing = false
    // If a drag is in progress, we keep “grab” semantics; don’t force a leave.
    if buttonMask == 0 {
      isPointerInside = false
    }
    let (x, y) = pointInX11(event, clampToView: true)
    x11_post_pointer_leave(xid, x, y, mods(event.modifierFlags))
  }
  
  override func scrollWheel(with event: NSEvent) {
    let dy = event.scrollingDeltaY
    let dx = event.scrollingDeltaX
    
    // Trackpads are often “precise”; accumulate until we cross a tick threshold.
    // Tune: 8–20 feels reasonable. Start at 12.
    let tick: CGFloat = 12
    
    scrollAccumY += dy
    scrollAccumX += dx
    
    // Only deliver scroll when inside, unless dragging/grab is active.
    let p = convert(event.locationInWindow, from: nil)
    let insideNow = bounds.contains(p)
    let dragging = (buttonMask != 0)
    if !insideNow && !dragging { return }
    
    let (x, y) = pointInX11(event, clampToView: true)
    let m = mods(event.modifierFlags)
    
    func postScroll(axis: x11_scroll_axis_t, ticks: Int16) {
      x11_post_scroll_ticks(xid, axis, ticks, x, y, buttonMask, m)
    }
    
    while scrollAccumY >= tick { scrollAccumY -= tick; postScroll(axis: X11_SCROLL_VERT, ticks: +1) }
    while scrollAccumY <= -tick { scrollAccumY += tick; postScroll(axis: X11_SCROLL_VERT, ticks: -1) }
    
    while scrollAccumX >=  tick { scrollAccumX -= tick; postScroll(axis: X11_SCROLL_HORZ, ticks: +1) }
    while scrollAccumX <= -tick { scrollAccumX += tick; postScroll(axis: X11_SCROLL_HORZ, ticks: -1) }
  }
  
  // MARK: - Keyboard
  private func x11Keycode(_ event: NSEvent) -> UInt32 {
    let kc = UInt32(event.keyCode) &+ 8
    return min(kc, 255)
  }
  
  override func flagsChanged(with event: NSEvent) {
    // Convert Cocoa modifier changes into X11-style key up/down events.
    // We track transitions because `flagsChanged` fires for both press and release.
    let newFlags = event.modifierFlags
    let oldFlags = lastModifierFlags
    lastModifierFlags = newFlags
    
    // We only care about the four primary modifiers we encode in `mods()`.
    let tracked: [(NSEvent.ModifierFlags, String)] = [
      (.shift, "shift"),
      (.control, "control"),
      (.option, "option"),
      (.command, "command")
    ]
    
    for (flag, _) in tracked {
      let wasDown = oldFlags.contains(flag)
      let isDown  = newFlags.contains(flag)
      if wasDown == isDown { continue }
      
      // Cocoa reports the physical key in `event.keyCode`.
      // This matches the key that triggered the modifier transition.
      x11_post_key_event(xid, isDown, UInt32(event.keyCode), mods(newFlags), nil)
    }
  }
  
  override func keyDown(with event: NSEvent) {
    lastModifierFlags = event.modifierFlags
    
    let text = event.characters ?? ""
    text.withCString { cstr in
      x11_post_key_event(xid, true, UInt32(event.keyCode), mods(event.modifierFlags), cstr)
    }
  }
  
  override func keyUp(with event: NSEvent) {
    lastModifierFlags = event.modifierFlags
    x11_post_key_event(xid, false, UInt32(event.keyCode), mods(event.modifierFlags), nil)
  }
  
  private var currentCursor: NSCursor = .arrow {
    didSet {
      // Keep metal overlay in sync
      mtkView?.currentCursor = currentCursor

      // Drop stale rects then rebuild
      self.discardCursorRects()
      if let mv = mtkView { mv.discardCursorRects() }

      // Invalidate cursor rects for whichever view is “on top”
      window?.invalidateCursorRects(for: self)
      if let mv = mtkView { window?.invalidateCursorRects(for: mv) }

      // If pointer is currently inside, apply immediately.
      if let win = window {
        let p = convert(win.mouseLocationOutsideOfEventStream, from: nil)
        if bounds.contains(p) { currentCursor.set() }
      }
    }
  }

  func applyCursorShapeRaw(_ shapeRaw: Int32) {
    let shape = X11CursorShape(rawValue: shapeRaw) ?? .arrow
    currentCursor = nsCursor(for: shape)

    // Force apply now (C++ already checked “pointer is in this host”)
    currentCursor.set()

    // Also force AppKit to rebuild cursor rects
    discardCursorRects()
    window?.invalidateCursorRects(for: self)

  #if DEBUG
    print("[CURSOR] applyCursorShapeRaw xid=0x\(String(xid, radix:16)) shapeRaw=\(shapeRaw) windowNil=\(window == nil)")
  #endif
  }
  
  override func resetCursorRects() {
    super.resetCursorRects()
    // This covers the software path (and is harmless if metal is on top)
    addCursorRect(bounds, cursor: currentCursor)
  }

  
}

struct X11WindowHost: NSViewRepresentable {
  let useMetal: Bool
  let onViewReady: (X11View) -> Void
  
  func makeNSView(context: Context) -> X11View {
    let v = X11View(frame: .zero)
    v.wantsMetal = useMetal
    v.setUseMetal(useMetal)
    onViewReady(v)
    return v
  }
  
  func updateNSView(_ nsView: X11View, context: Context) {
    nsView.wantsMetal = useMetal
    nsView.setUseMetal(useMetal)
  }
  
}


final class X11Renderer: NSObject, MTKViewDelegate {
  private weak var owner: X11View?
  private var xid: UInt32 = 0
  
  private var didNotifyPresentable = false
  private var lastDrawablePt: (w: Int32, h: Int32) = (0, 0)
  
  init(owner: X11View) {
    self.owner = owner
    super.init()
  }
  
  func setXid(_ xid: UInt32) {
    self.xid = xid
    self.didNotifyPresentable = false
    self.lastDrawablePt = (0, 0)
  }
  
  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
#if DEBUG
    let w = Int(size.width.rounded())
    let h = Int(size.height.rounded())
    if h == 0 || w <= 4 || h <= 4 {
      print("[MTK] drawableSizeWillChange (pre guard) xid=\(String(xid, radix:16)) -> \(w)x\(h) window=\(String(describing: view.window)) frame=\(view.frame)")
    }
#endif
    
    let wPt = Int32(size.width.rounded(.down))
    let hPt = Int32(size.height.rounded(.down))
    guard wPt >= 1, hPt >= 1 else { return }
    guard size.width >= 1, size.height >= 1 else { return }
    
#if DEBUG
    if h == 0 || w <= 4 || h <= 4 {
      print("[MTK] drawableSizeWillChange (post guard) xid=\(String(xid, radix:16)) -> \(w)x\(h) window=\(String(describing: view.window)) frame=\(view.frame)")
    }
#endif
    
    handleDrawableSize(size)
  }
  
  func draw(in view: MTKView) {
    // Hard gate: never draw with invalid drawableSize
    let ds = view.drawableSize
    guard ds.width >= 1, ds.height >= 1 else { return }
    guard view.currentDrawable != nil else { return } // optional but helps
                                                      // Do NOT call handleDrawableSize here
    owner?.metalDraw(in: view)
  }
  
  private var pendingResizeTask: DispatchWorkItem? = nil
  private var pendingSize: (w: Int32, h: Int32)? = nil
  
  
  
  private func handleDrawableSize(_ size: CGSize) {
    guard xid != 0 else { return }
    
    // MTKView drawableSize is in *pixels*.
    let wPx = Int32(size.width.rounded(.toNearestOrAwayFromZero))
    let hPx = Int32(size.height.rounded(.toNearestOrAwayFromZero))
    
    // ---- HARD GATES ----
    guard wPx >= 1, hPx >= 1 else { return }
    guard wPx >= 16, hPx >= 16 else { return }
    guard owner?.window != nil else { return }
    
    if !didNotifyPresentable {
      didNotifyPresentable = true
      x11_post_window_presentable(xid)
    }
    
    if wPx == lastDrawablePt.w && hPx == lastDrawablePt.h { return }
    lastDrawablePt = (wPx, hPx)
    
    pendingResizeTask?.cancel()
    pendingSize = (w: wPx, h: hPx)
    
    let task = DispatchWorkItem { [weak self] in
      guard let self = self, let s = self.pendingSize else { return }
      
      // Convert backing pixels -> X11 units (points) under the new model.
      let scale = self.owner?.window?.backingScaleFactor ?? 1.0
      let wX11 = Int32(max(1, Int((CGFloat(s.w) / scale).rounded(.down))))
      let hX11 = Int32(max(1, Int((CGFloat(s.h) / scale).rounded(.down))))
      
      x11_post_window_resize(self.xid, wX11, hX11)
    }
    
    pendingResizeTask = task
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05, execute: task)
  }
  
}

extension X11View {
  static func logIfInLayout(_ label: String, view: X11View?) {
    view?.logIfInLayout(label, view: view)
  }
}
