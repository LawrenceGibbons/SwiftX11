import Foundation
import Metal
import MetalKit

/// Minimal “textured quad” Metal renderer for SwiftX11.
/// Uploads BGRA8 frames into a texture and draws a full-screen quad.
///
/// Assumptions:
/// - Source pixels are BGRA8 little-endian (matches .bgra8Unorm).
/// - The quad uses UV with v=0 at TOP to match your software orientation.
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

  // Quad pipeline resources
  private var pipeline: MTLRenderPipelineState!
  private var sampler: MTLSamplerState!
  private var quadVB: MTLBuffer!

  // MARK: - Init

  init(device: MTLDevice, pixelFormat: MTLPixelFormat) {
    self.device = device
    guard let q = device.makeCommandQueue() else {
      fatalError("X11MetalRenderer: failed to create MTLCommandQueue")
    }
    self.commandQueue = q
    buildQuadResources(device: device, pixelFormat: pixelFormat)
  }

  /// Convenience init that binds to the MTKView’s actual pixel format.
  convenience init(view: MTKView) {
    guard let dev = view.device ?? MTLCreateSystemDefaultDevice() else {
      fatalError("X11MetalRenderer: no Metal device available")
    }
    // Ensure MTKView has a device (caller usually sets this already).
    view.device = dev
    self.init(device: dev, pixelFormat: view.colorPixelFormat)
  }

  // MARK: - Resources

  private func buildQuadResources(device: MTLDevice, pixelFormat: MTLPixelFormat) {
    // Fullscreen quad in clip space, with UVs.
    // IMPORTANT: uv.y = 0 at TOP to match your BGRA buffer orientation.
    let verts: [Float] = [
      //  x,   y,   u,  v
      -1,  +1,  0,  0,   // top-left
      -1,  -1,  0,  1,   // bottom-left
      +1,  +1,  1,  0,   // top-right

      +1,  +1,  1,  0,   // top-right
      -1,  -1,  0,  1,   // bottom-left
      +1,  -1,  1,  1,   // bottom-right
    ]

    quadVB = device.makeBuffer(bytes: verts,
                               length: verts.count * MemoryLayout<Float>.size,
                               options: .storageModeShared)
    if quadVB == nil {
      fatalError("X11MetalRenderer: failed to create quad vertex buffer")
    }

    // Nearest sampling (matches your CALayer nearest filters)
    let sd = MTLSamplerDescriptor()
    sd.minFilter = .nearest
    sd.magFilter = .nearest
    sd.sAddressMode = .clampToEdge
    sd.tAddressMode = .clampToEdge
    sampler = device.makeSamplerState(descriptor: sd)
    if sampler == nil {
      fatalError("X11MetalRenderer: failed to create sampler")
    }

    // Pipeline
    guard let lib = device.makeDefaultLibrary() else {
      fatalError("X11MetalRenderer: makeDefaultLibrary() failed (missing .metal in target?)")
    }
    guard let vfn = lib.makeFunction(name: "texturedQuadVS") else {
      fatalError("X11MetalRenderer: missing vertex function texturedQuadVS")
    }
    guard let ffn = lib.makeFunction(name: "texturedQuadFS") else {
      fatalError("X11MetalRenderer: missing fragment function texturedQuadFS")
    }

    let pd = MTLRenderPipelineDescriptor()
    pd.vertexFunction = vfn
    pd.fragmentFunction = ffn
    pd.colorAttachments[0].pixelFormat = pixelFormat

    // Vertex layout: float2 pos, float2 uv
    let vdesc = MTLVertexDescriptor()
    vdesc.attributes[0].format = .float2
    vdesc.attributes[0].offset = 0
    vdesc.attributes[0].bufferIndex = 0

    vdesc.attributes[1].format = .float2
    vdesc.attributes[1].offset = 2 * MemoryLayout<Float>.size
    vdesc.attributes[1].bufferIndex = 0

    vdesc.layouts[0].stride = 4 * MemoryLayout<Float>.size
    pd.vertexDescriptor = vdesc

    do {
      pipeline = try device.makeRenderPipelineState(descriptor: pd)
    } catch {
      fatalError("X11MetalRenderer: makeRenderPipelineState failed: \(error)")
    }
  }

  // MARK: - Upload

  func updateTexture(with data: Data, width: Int, height: Int, bytesPerRow: Int,
                     damageRect: DamageRect? = nil) {
    data.withUnsafeBytes { rawBuf in
      guard let base = rawBuf.baseAddress else { return }
      self.updateTexture(with: base, width: width, height: height, bytesPerRow: bytesPerRow,
                         damageRect: damageRect)
    }
  }

  func updateTexture(with pixels: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int,
                     damageRect: DamageRect? = nil) {
    guard width > 0, height > 0, bytesPerRow > 0 else { return }

    // BGRA8 sanity
    let minBpr = width * 4
    guard bytesPerRow >= minBpr else {
      print("[Metal] updateTexture: DROP bad bytesPerRow=\(bytesPerRow) < \(minBpr) (w=\(width))")
      return
    }

    // Reuse the texture when possible to avoid churn.
    var textureIsNew = false
    if texture == nil || texture?.width != width || texture?.height != height {
      let desc = MTLTextureDescriptor.texture2DDescriptor(
        pixelFormat: .bgra8Unorm,
        width: width,
        height: height,
        mipmapped: false
      )

      // CPU upload on macOS is fine with shared.
      desc.storageMode = .shared

      // We sample it in the fragment shader
      desc.usage = [.shaderRead]

      texture = device.makeTexture(descriptor: desc)
      if texture == nil {
        print("[Metal] updateTexture: FAILED to allocate texture \(width)x\(height)")
        return
      }
      textureIsNew = true
    }

    guard let tex = texture else { return }

    // If we have a valid damage rect and the texture already existed (not freshly
    // allocated), upload only the dirty sub-region.  The rest of the texture retains
    // the previous frame's pixels.
    if !textureIsNew, let dr = damageRect,
       dr.w > 0, dr.h > 0,
       dr.x >= 0, dr.y >= 0,
       dr.x + dr.w <= width, dr.y + dr.h <= height {
      let region = MTLRegionMake2D(dr.x, dr.y, dr.w, dr.h)
      let offset = dr.y * bytesPerRow + dr.x * 4
      tex.replace(region: region,
                  mipmapLevel: 0,
                  withBytes: pixels.advanced(by: offset),
                  bytesPerRow: bytesPerRow)
      return
    }

    // Full upload: first frame, texture resize, or unknown damage.
    let region = MTLRegionMake2D(0, 0, width, height)
    tex.replace(region: region,
                mipmapLevel: 0,
                withBytes: pixels,
                bytesPerRow: bytesPerRow)
  }

  // MARK: - Draw

  func draw(on view: MTKView) {
    // Coalesce bursts safely.
    if inFlight {
      pendingDraw = true
      return
    }

    // MTKView.drawableSize is in pixels.
    let ds = view.drawableSize
    guard ds.width >= 1, ds.height >= 1 else { return }

    // Avoid rendering before the view is attached to a window.
    guard view.window != nil else { return }

    guard let srcTex = texture else { return }
    guard let rpd = view.currentRenderPassDescriptor else { return }
    guard let drawable = view.currentDrawable else { return }
    guard let cmdBuf = commandQueue.makeCommandBuffer() else { return }

    inFlight = true

    if let ca = rpd.colorAttachments[0] {
      ca.loadAction = .clear
      ca.storeAction = .store
      ca.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    }

    guard let enc = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) else {
      inFlight = false
      return
    }

    enc.setRenderPipelineState(pipeline)
    enc.setVertexBuffer(quadVB, offset: 0, index: 0)
    enc.setFragmentTexture(srcTex, index: 0)
    enc.setFragmentSamplerState(sampler, index: 0)
    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
    enc.endEncoding()

    cmdBuf.addCompletedHandler { [weak self, weak view] _ in
      guard let self else { return }
      self.inFlight = false

      guard self.pendingDraw else { return }
      self.pendingDraw = false

      DispatchQueue.main.async {
        guard let view else { return }
        guard view.window != nil else { return }
        let ds2 = view.drawableSize
        guard ds2.width >= 1, ds2.height >= 1 else { return }
        view.setNeedsDisplay(view.bounds)
      }
    }

    cmdBuf.present(drawable)
    cmdBuf.commit()
  }
}
