import Foundation
import Metal
import MetalKit

final class X11MetalRenderer {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue

    // Debug-only: whether we currently have an uploaded source texture.
    var hasTexture: Bool { texture != nil }

    // Keep readable for logging without allowing external mutation.
    private(set) var texture: MTLTexture?

    // Prevent overlapping presents / command buffers piling up.
    // If a draw is requested while one is in-flight, remember it and schedule
    // exactly one more draw on completion.
    private var inFlight: Bool = false
    private var pendingDraw: Bool = false

    init(device: MTLDevice) {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
    }

    convenience init(view: MTKView) {
        self.init(device: view.device!)
    }

    func updateTexture(
        with data: Data,
        width: Int,
        height: Int,
        bytesPerRow: Int
    ) {
        data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.baseAddress else { return }
            self.updateTexture(with: base, width: width, height: height, bytesPerRow: bytesPerRow)
        }
    }

    func updateTexture(
        with pixels: UnsafeRawPointer,
        width: Int,
        height: Int,
        bytesPerRow: Int
    ) {
        guard width > 0, height > 0, bytesPerRow > 0 else { return }

        // Reuse the texture when possible to avoid churn.
        if texture == nil || texture?.width != width || texture?.height != height {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm,
                width: width,
                height: height,
                mipmapped: false
            )

            // We upload from CPU; shared is fine on macOS.
            desc.storageMode = .shared

            // We only need to copy/blit from it.
            desc.usage = [.shaderRead] // minimal safe usage
            texture = device.makeTexture(descriptor: desc)
        }

        guard let tex = texture else { return }

        let region = MTLRegionMake2D(0, 0, width, height)
        tex.replace(
            region: region,
            mipmapLevel: 0,
            withBytes: pixels,
            bytesPerRow: bytesPerRow
        )
    }

    func draw(on view: MTKView) {
        // Coalesce bursts safely.
        if inFlight {
            pendingDraw = true
            return
        }

        guard let srcTex = texture else { return }
        guard let rpd = view.currentRenderPassDescriptor else { return }
        guard let drawable = view.currentDrawable else { return }

        // IMPORTANT: clearing only happens if we actually encode a render pass.
        if let ca = rpd.colorAttachments[0] {
            ca.loadAction = .clear
            ca.storeAction = .store
            ca.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        }

        guard let cmdBuf = commandQueue.makeCommandBuffer() else { return }
        inFlight = true

        // 1) Execute the clear by running a trivial render pass.
        if let render = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) {
            render.endEncoding()
        } else {
            inFlight = false
            return
        }

        // 2) Copy pixels into the drawable using a blit encoder.
        guard let blit = cmdBuf.makeBlitCommandEncoder() else {
            inFlight = false
            return
        }

        let copyW = min(srcTex.width, drawable.texture.width)
        let copyH = min(srcTex.height, drawable.texture.height)
        if copyW > 0, copyH > 0 {
            blit.copy(
                from: srcTex,
                sourceSlice: 0,
                sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: copyW, height: copyH, depth: 1),
                to: drawable.texture,
                destinationSlice: 0,
                destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0)
            )
        }
        blit.endEncoding()

        cmdBuf.addCompletedHandler { [weak self, weak view] _ in
            guard let self else { return }
            self.inFlight = false

            if self.pendingDraw {
                self.pendingDraw = false
                DispatchQueue.main.async {
                    guard let view else { return }
                    view.setNeedsDisplay(view.bounds)
                }
            }
        }

        cmdBuf.present(drawable)
        cmdBuf.commit()
    }
}
