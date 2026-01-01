import SwiftUI
import AppKit
import MetalKit
import CoreGraphics

final class X11View: NSView, MTKViewDelegate {
    // MARK: - Mode
    private var wantMetal: Bool = true
    private var usingMetal: Bool = false

    // MARK: - Software path
    private var imageLayer: CALayer?
    private var lastFrameData: Data?

    // MARK: - Metal path
    private var mtkView: MTKView?
    private var device: MTLDevice?
    private var commandQueue: MTLCommandQueue?
    private var frameTexture: MTLTexture?

    // Latest frame (copied) that we upload to texture
    private var pendingFrame: (data: Data, width: Int, height: Int, bytesPerRow: Int)?

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

        let view = MTKView(frame: bounds, device: device)
        view.autoresizingMask = [.width, .height]
        view.delegate = self
        view.enableSetNeedsDisplay = false
        view.isPaused = true              // we will call draw() manually
        view.framebufferOnly = false      // allow blit/copy to drawable texture
        view.colorPixelFormat = .bgra8Unorm

        addSubview(view, positioned: .below, relativeTo: nil)
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
