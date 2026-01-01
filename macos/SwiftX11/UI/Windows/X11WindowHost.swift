import SwiftUI
import AppKit
import CoreGraphics

final class X11View: NSView {
    private var imageLayer: CALayer?
    private var lastFrameData: Data?
  
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true

        let layer = CALayer()
        layer.frame = bounds
        layer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        layer.contentsGravity = .resize
        layer.magnificationFilter = .nearest
        layer.minificationFilter = .nearest
        self.layer?.addSublayer(layer)
        imageLayer = layer
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()
        imageLayer?.frame = bounds
    }
  
    /// Present a BGRA8888 (little-endian) framebuffer.
    func presentBGRA(framebuffer: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
        guard width > 0, height > 0 else { return }

        // Copy the pixels so the memory stays valid after C frees its buffer.
        let byteCount = bytesPerRow * height
        let frameData = Data(bytes: framebuffer, count: byteCount)

        let colorSpace = CGColorSpaceCreateDeviceRGB()

        // BGRA, 8bpc, little-endian, premultiplied first (i.e., BGRA in memory).
        let bitmapInfo: CGBitmapInfo = [
            CGBitmapInfo.byteOrder32Little,
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
        ]

        guard let provider = CGDataProvider(data: frameData as CFData) else { return }

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
        ) else {
            return
        }

        // Must update layers on main thread.
      DispatchQueue.main.async { [weak self] in
          // Keep the data alive for as long as the layer might reference it.
          // (Storing it prevents ARC from releasing it immediately.)
          self?.lastFrameData = frameData
          self?.imageLayer?.contents = cgImage
      }
    }
}

struct X11WindowHost: NSViewRepresentable {
    let onViewReady: (X11View) -> Void

    func makeNSView(context: Context) -> X11View {
        let v = X11View(frame: .zero)
        onViewReady(v)
        return v
    }

    func updateNSView(_ nsView: X11View, context: Context) { }
}
