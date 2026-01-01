import SwiftUI
import AppKit
import MetalKit
import CoreGraphics
import X11LowLevel

final class X11MTKView: MTKView {
    weak var owner: X11View?

    override var acceptsFirstResponder: Bool { true }

    override func mouseDown(with event: NSEvent) { owner?.mouseDown(with: event)  }
    override func mouseUp(with event: NSEvent) { owner?.mouseUp(with: event) }
    override func rightMouseDown(with event: NSEvent) { owner?.rightMouseDown(with: event) }
    override func rightMouseUp(with event: NSEvent) { owner?.rightMouseUp(with: event) }
    override func otherMouseDown(with event: NSEvent) { owner?.otherMouseDown(with: event) }
    override func otherMouseUp(with event: NSEvent) { owner?.otherMouseUp(with: event) }

    override func mouseMoved(with event: NSEvent) { owner?.mouseMoved(with: event) }
    override func mouseDragged(with event: NSEvent) { owner?.mouseDragged(with: event) }
    override func rightMouseDragged(with event: NSEvent) { owner?.rightMouseDragged(with: event) }
    override func otherMouseDragged(with event: NSEvent) { owner?.otherMouseDragged(with: event) }

    override func scrollWheel(with event: NSEvent) { owner?.scrollWheel(with: event) }

    override func keyDown(with event: NSEvent) { owner?.keyDown(with: event) }
    override func keyUp(with event: NSEvent) { owner?.keyUp(with: event) }
    override func flagsChanged(with event: NSEvent) { owner?.flagsChanged(with: event) }
}

final class X11View: NSView, MTKViewDelegate {
    // MARK: - Mode
    private var wantMetal: Bool = true
    private var usingMetal: Bool = false

    // MARK: - Software path
    private var imageLayer: CALayer?
    private var lastFrameData: Data?
    private var trackingArea: NSTrackingArea?

    // MARK: - Metal path
    private var mtkView: MTKView?
    private var device: MTLDevice?
    private var commandQueue: MTLCommandQueue?
    private var frameTexture: MTLTexture?

    // MARK: -- interface to X11
    private var buttonMask: UInt32 = 0
    private var scrollAccumX: CGFloat = 0
    private var scrollAccumY: CGFloat = 0
  
    // Latest frame (copied) that we upload to texture
    private var pendingFrame: (data: Data, width: Int, height: Int, bytesPerRow: Int)?

    // know which window to use
    var xid: UInt32 = 0
  
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        setupSoftwareLayer()
        // Don’t setup metal yet until setUseMetal() is called.
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()
        imageLayer?.frame = bounds
        mtkView?.frame = bounds
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        // Ensure the containing window delivers mouse moved events.
        self.window?.acceptsMouseMovedEvents = true
      
      DispatchQueue.main.async { [weak self] in
          self?.updateTrackingAreas()
      }
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()

        if let trackingArea {
            removeTrackingArea(trackingArea)
        }

        // Track mouse movement within our visible rect while the window is key.
        let options: NSTrackingArea.Options = [
            .mouseMoved,
            .activeInKeyWindow,
            .inVisibleRect
        ]

        let area = NSTrackingArea(rect: .zero, options: options, owner: self, userInfo: nil)
        addTrackingArea(area)
        trackingArea = area
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
        wantMetal = enabled

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
    }

    /// Present BGRA8888 little-endian framebuffer
    func presentBGRA(framebuffer: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
        guard width > 0, height > 0 else { return }
        let byteCount = bytesPerRow * height

        // Always copy bytes so the memory remains valid after C frees it
        let data = Data(bytes: framebuffer, count: byteCount)

        if usingMetal {
            pendingFrame = (data: data, width: width, height: height, bytesPerRow: bytesPerRow)
            DispatchQueue.main.async { [weak self] in
                self?.mtkView?.draw()
            }
        } else {
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
        self.commandQueue = device.makeCommandQueue()

        let view = X11MTKView(frame: bounds, device: device)
        view.owner = self
        view.autoresizingMask = [.width, .height]
        view.delegate = self
        view.enableSetNeedsDisplay = false
        view.isPaused = true              // we will call draw() manually
        view.framebufferOnly = false      // allow blit/copy to drawable texture
        view.colorPixelFormat = .bgra8Unorm

        addSubview(view, positioned: .above, relativeTo: nil)
        self.mtkView = view
    }

    private func ensureTexture(width: Int, height: Int) {
        if let tex = frameTexture, tex.width == width, tex.height == height { return }
        guard let device else { return }

        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: width,
            height: height,
            mipmapped: false
        )
        // No explicit usage needed for CPU upload + blit copy
        desc.storageMode = .shared
        frameTexture = device.makeTexture(descriptor: desc)
    }
  
    // MARK: X11 interface
    private func bitForButton(_ button: Int) -> UInt32 {
        // button: 1..8
        guard button >= 1 && button <= 31 else { return 0 }
        return 1 << UInt32(button - 1)
    }

    // MARK: - MTKViewDelegate
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // No-op: we size our source texture to the incoming frames.
    }

  
    func draw(in view: MTKView) {
        guard let queue = commandQueue,
              let drawable = view.currentDrawable,
              let frame = pendingFrame else { return }

        pendingFrame = nil

        ensureTexture(width: frame.width, height: frame.height)
        guard let src = frameTexture else { return }

        // Upload pixel bytes into src texture
        frame.data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.baseAddress else { return }
            let region = MTLRegionMake2D(0, 0, frame.width, frame.height)
            src.replace(region: region, mipmapLevel: 0, withBytes: base, bytesPerRow: frame.bytesPerRow)
        }

        // Copy src texture to drawable texture
        guard let cmd = queue.makeCommandBuffer(),
              let blit = cmd.makeBlitCommandEncoder() else { return }

        let w = min(src.width, drawable.texture.width)
        let h = min(src.height, drawable.texture.height)

        blit.copy(
            from: src,
            sourceSlice: 0,
            sourceLevel: 0,
            sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: w, height: h, depth: 1),
            to: drawable.texture,
            destinationSlice: 0,
            destinationLevel: 0,
            destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0)
        )
        blit.endEncoding()

        cmd.present(drawable)
        cmd.commit()
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

extension X11View {
  override var acceptsFirstResponder: Bool { true }
  
  private func mods(_ flags: NSEvent.ModifierFlags) -> UInt32 {
    var m: UInt32 = 0
    if flags.contains(.shift) { m |= 1 << 0 }
    if flags.contains(.control) { m |= 1 << 1 }
    if flags.contains(.option) { m |= 1 << 2 }
    if flags.contains(.command) { m |= 1 << 3 }
    return m
  }
  
  private func pointInPixels(_ event: NSEvent) -> (Int32, Int32) {
    // Convert to view coords (origin bottom-left in view coords)
    let pInWindow = event.locationInWindow
    let p = convert(pInWindow, from: nil)
    
    let scale = window?.backingScaleFactor ?? 1.0
    let x = Int32(max(0, Int((p.x * scale).rounded(.down))))
    let y = Int32(max(0, Int((p.y * scale).rounded(.down))))
    return (x, y)
  }
  
  override func flagsChanged(with event: NSEvent) {
      // You can later forward this into the backend as “modifier state changed”.
      // For now we’ll just ensure logs show it if you want.
      // x11_post_modifiers_changed(xid, mods(event.modifierFlags))  // future API
  }
  
  // MARK: Mouse
  private func sendPointer(_ type: x11_ptr_event_type, _ event: NSEvent) {
    let (x, y) = pointInPixels(event)
    x11_post_pointer_event(xid, type, x, y, buttonMask, mods(event.modifierFlags))
  }
  
  override func mouseDown(with event: NSEvent) {
    self.window?.makeFirstResponder(self)
    buttonMask |= bitForButton(1)
    sendPointer(X11_PTR_DOWN, event)
  }
  
  override func mouseUp(with event: NSEvent) {
    // send UP while the bit is still set (matches common X11 reporting)
    sendPointer(X11_PTR_UP, event)
    buttonMask &= ~bitForButton(1)
  }
  
  override func rightMouseDown(with event: NSEvent) {
    self.window?.makeFirstResponder(self)
    buttonMask |= bitForButton(3)
    sendPointer(X11_PTR_DOWN, event)
  }
  
  override func rightMouseUp(with event: NSEvent) {
    sendPointer(X11_PTR_UP, event)
    buttonMask &= ~bitForButton(3)
  }
  
  override func otherMouseDown(with event: NSEvent) {
    self.window?.makeFirstResponder(self)
    // common mapping: “other” -> button2 (middle)
    buttonMask |= bitForButton(2)
    sendPointer(X11_PTR_DOWN, event)
  }
  
  override func otherMouseUp(with event: NSEvent) {
    sendPointer(X11_PTR_UP, event)
    buttonMask &= ~bitForButton(2)
  }
  
  override func mouseMoved(with event: NSEvent) { sendPointer(X11_PTR_MOVE, event) }
  override func mouseDragged(with event: NSEvent) { sendPointer(X11_PTR_MOVE, event) }
  override func rightMouseDragged(with event: NSEvent) { sendPointer(X11_PTR_MOVE, event) }
  override func otherMouseDragged(with event: NSEvent) { sendPointer(X11_PTR_MOVE, event) }
  
  override func scrollWheel(with event: NSEvent) {
    let dy = event.scrollingDeltaY
    let dx = event.scrollingDeltaX
    
    // Trackpads are often “precise”; accumulate until we cross a tick threshold.
    // Tune: 8–20 feels reasonable. Start at 12.
    let tick: CGFloat = 12
    
    scrollAccumY += dy
    scrollAccumX += dx
    
    func emitTick(button: Int, using event: NSEvent) {
      // Press+release for the wheel tick without changing persistent buttonMask
      let saved = buttonMask
      buttonMask |= bitForButton(button)
      sendPointer(X11_PTR_DOWN, event)
      sendPointer(X11_PTR_UP, event)
      buttonMask = saved
    }
    
    while scrollAccumY >= tick { scrollAccumY -= tick; emitTick(button: 4, using: event) } // up
    while scrollAccumY <= -tick { scrollAccumY += tick; emitTick(button: 5, using: event) } // down
    while scrollAccumX >= tick { scrollAccumX -= tick; emitTick(button: 7, using: event) } // right
    while scrollAccumX <= -tick { scrollAccumX += tick; emitTick(button: 6, using: event) } // left
  }
  
  // MARK: Keyboard
  override func keyDown(with event: NSEvent) {
    let text = event.characters ?? ""
    text.withCString { cstr in
      x11_post_key_event(xid, true, UInt32(event.keyCode), mods(event.modifierFlags), cstr)
    }
  }
  
  override func keyUp(with event: NSEvent) {
    x11_post_key_event(xid, false, UInt32(event.keyCode), mods(event.modifierFlags), nil)
  }
}
