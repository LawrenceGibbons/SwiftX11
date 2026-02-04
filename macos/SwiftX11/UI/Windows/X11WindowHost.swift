import SwiftUI
import AppKit
import Cocoa
import MetalKit
import CoreGraphics
import X11LowLevel


final class X11MTKView: MTKView {
    weak var owner: X11View?
    var xid: UInt32 = 0

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
    private var usingMetal: Bool = false

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
    private var suppressExpectedPx: (w: Int32, h: Int32)? = nil
    private var suppressBudget: Int = 0
  
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        setupSoftwareLayer()
        // Don’t setup metal yet until setUseMetal() is called.
    }

  
  
  
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

    override func layout() {
        Self.layoutDepthTLS.value += 1
        defer { Self.layoutDepthTLS.value -= 1 }
        super.layout()

        imageLayer?.frame = bounds
        mtkView?.frame = bounds
      
        // Keep Metal drawableSize in sync with view bounds (in pixels).
        // SwiftUI can transiently layout with a 0-height; guard to avoid warnings.
        if let mv = mtkView, mv.window != nil {
          let scale = window?.backingScaleFactor ?? 1.0
          let wF = bounds.size.width * scale
          let hF = bounds.size.height * scale
          guard wF > 0, hF > 0 else { return }
          let ds = CGSize(width: floor(wF), height: floor(hF))

          // Defer changing drawableSize until after layout completes.
          if mv.drawableSize != ds {
              scheduleDrawableSizeUpdate(ds)
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
//    override func viewDidMoveToWindow() {
//        super.viewDidMoveToWindow()
//        window?.acceptsMouseMovedEvents = true
//        requestFirstResponderCoalesced()
//        isPointerInside = false
//      
//        // Defer to next runloop turn (avoids “already being laid out” warnings)
//        NSObject.cancelPreviousPerformRequests(withTarget: self, selector: #selector(_refreshTrackingAreas), object: nil)
//        perform(#selector(_refreshTrackingAreas), with: nil, afterDelay: 0.0)
//    }
  
  
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
  
    private func scheduleAttachSettle() {
        guard !attachSettleScheduled else { return }
        attachSettleScheduled = true
  
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.attachSettleScheduled = false
  
            // All “attach-time” mutating work happens here, never inside viewDidMoveToWindow.
            self.window?.acceptsMouseMovedEvents = true
  
            // Don’t set responder synchronously on attach; coalesce.
            self.requestFirstResponderCoalesced()
  
            // Don’t refresh tracking areas synchronously on attach; coalesce.
            self.requestTrackingRefreshCoalesced()
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
        refreshTrackingArea()
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
      
        refreshTrackingArea()
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

            self.renderer?.updateTexture(with: data, width: width, height: height, bytesPerRow: bytesPerRow)

            // Ask MTKView to draw exactly once (draw(in:) will use currentDrawable and present).
            mv.setNeedsDisplay(mv.bounds)
        } else {
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

        DispatchQueue.main.async { [weak self] in
            self?.lastFrameData = data
            self?.imageLayer?.contents = cgImage
        }
    }

    // MARK: - Metal setup
    private func setupMetal(device: MTLDevice) {
      self.device = device
  
      let view = X11MTKView(frame: bounds, device: device)
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
  
      self.renderer = X11MetalRenderer(device: device)
  
      addSubview(view, positioned: .above, relativeTo: nil)
      self.mtkView = view
  
      // init drawableSize using the *actual* window scale (now more likely non-nil)
      let scale = self.window?.backingScaleFactor ?? 1.0
      let wF = bounds.size.width * scale
      let hF = bounds.size.height * scale
      if wF > 0, hF > 0 {
          let ds = CGSize(width: floor(wF), height: floor(hF))
        print("[MTK] drawableSize about to set xid=\(view.xid) size=\(ds) window=\(String(describing: view.window))")
          view.drawableSize = ds
          del.mtkView(view, drawableSizeWillChange: ds)   // force presentable notification
      }
      
      // kick once so X11Renderer posts presentable+resize immediately
      view.setNeedsDisplay(view.bounds)
      view.draw()
    }

    private func refreshTrackingArea() {
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
          .activeInActiveApp,
          .enabledDuringMouseDrag,
          .inVisibleRect
        ]

        let area = NSTrackingArea(rect: .zero, options: opts, owner: self, userInfo: nil)
        target.addTrackingArea(area)
        self.trackingArea = area
        self.trackingHost = target
    }
  
    // (ensureTexture removed; now handled by X11MetalRenderer)
  
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

private func pointInPixels(_ event: NSEvent, clampToView: Bool) -> (Int32, Int32) {
  // Convert to view coords (origin bottom-left)
  let pInWindow = event.locationInWindow
  let p = convert(pInWindow, from: nil)

  let scale = window?.backingScaleFactor ?? 1.0

  // Allow negative/outside coords when NOT clamping (drag semantics).
  var xF = p.x * scale
  var yF = p.y * scale

  if clampToView {
    // Clamp in points then scale (matches bounds.contains logic)
    let clampedXPt = min(max(p.x, 0), bounds.width)
    let clampedYPt = min(max(p.y, 0), bounds.height)
    xF = clampedXPt * scale
    yF = clampedYPt * scale

    // Clamp in pixels to [0 .. sizePx-1]
    let wPx = max(1, Int(floor(bounds.width * scale)))
    let hPx = max(1, Int(floor(bounds.height * scale)))
    xF = min(max(xF, 0), CGFloat(wPx - 1))
    yF = min(max(yF, 0), CGFloat(hPx - 1))
  }

  return (Int32(floor(xF)), Int32(floor(yF)))
}

// MARK: - Mouse
private func sendMotion(_ event: NSEvent) {
  // Compute inside in *points*
  let p = convert(event.locationInWindow, from: nil)
  let insideNow = bounds.contains(p)
  let dragging = (buttonMask != 0)

  // Synthesize enter/leave transitions (robust even if trackingArea misses)
  if insideNow && !isPointerInside {
    isPointerInside = true
    let (x, y) = pointInPixels(event, clampToView: true)
    x11_post_pointer_enter(xid, x, y, mods(event.modifierFlags))
  } else if !insideNow && isPointerInside && !dragging {
    // Only synth-leave when not dragging (during drag we want “grab” semantics)
    isPointerInside = false
    let (x, y) = pointInPixels(event, clampToView: true)
    x11_post_pointer_leave(xid, x, y, mods(event.modifierFlags))
  }

  // Deliver motion only when inside OR dragging/grab
  if !insideNow && !dragging {
    return
  }

  // During drag allow outside coords; hover clamps.
  let (x, y) = pointInPixels(event, clampToView: !dragging)
  x11_post_pointer_event(xid, X11_PTR_MOVE, x, y, buttonMask, mods(event.modifierFlags))
}

private func sendButton(_ isPress: Bool, button: UInt8, _ event: NSEvent) {
  let (x, y) = pointInPixels(event, clampToView: false)
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
  let (x, y) = pointInPixels(event, clampToView: true)
  x11_post_pointer_enter(xid, x, y, mods(event.modifierFlags))
}

override func mouseExited(with event: NSEvent) {
  lastInsideForSyntheticCrossing = false
  // If a drag is in progress, we keep “grab” semantics; don’t force a leave.
  if buttonMask == 0 {
    isPointerInside = false
  }
  let (x, y) = pointInPixels(event, clampToView: true)
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

  let (x, y) = pointInPixels(event, clampToView: true)
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

}

struct X11WindowHost: NSViewRepresentable {
  let useMetal: Bool
  let onViewReady: (X11View) -> Void
  
  func makeNSView(context: Context) -> X11View {
    let v = X11View(frame: .zero)
    v.setUseMetal(useMetal)
    onViewReady(v)
    return v
  }
  
  func updateNSView(_ nsView: X11View, context: Context) {
    nsView.setUseMetal(useMetal)
  }
}


final class X11Renderer: NSObject, MTKViewDelegate {
  private weak var owner: X11View?
  private var xid: UInt32 = 0

  private var didNotifyPresentable = false
  private var lastDrawablePx: (w: Int32, h: Int32) = (0, 0)

  init(owner: X11View) {
    self.owner = owner
    super.init()
  }

  func setXid(_ xid: UInt32) {
    self.xid = xid
    self.didNotifyPresentable = false
    self.lastDrawablePx = (0, 0)
  }

  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
    handleDrawableSize(size)
  }

  func draw(in view: MTKView) {
    // Do NOT call handleDrawableSize here
    owner?.metalDraw(in: view)
  }
  
  private var pendingResizeTask: DispatchWorkItem? = nil
  private var pendingSize: (w: Int32, h: Int32)? = nil

  private func handleDrawableSize(_ size: CGSize) {
    guard xid != 0 else { return }

    // MTKView drawable sizes are already in pixels.
    let wPx = Int32(size.width.rounded(.toNearestOrAwayFromZero))
    let hPx = Int32(size.height.rounded(.toNearestOrAwayFromZero))

    guard wPx >= 1, hPx >= 1 else { return }

    // Presentable: once.
    if !didNotifyPresentable {
      didNotifyPresentable = true
      x11_post_window_presentable(xid)
    }

    if wPx == lastDrawablePx.w && hPx == lastDrawablePx.h { return }
    lastDrawablePx = (wPx, hPx)

    // Debounce resize feedback (coalesce bursts)
    pendingResizeTask?.cancel()
    pendingSize = (wPx, hPx)

    let task = DispatchWorkItem { [weak self] in
      guard let self = self, let s = self.pendingSize else { return }
      x11_post_window_resize(self.xid, s.w, s.h)
    }
    pendingResizeTask = task
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05, execute: task) // 50ms
  }
}

extension X11View {
  static func logIfInLayout(_ label: String, view: X11View?) {
    view?.logIfInLayout(label, view: view)
  }
}
