import SwiftUI
import AppKit
import MetalKit

final class X11View: NSView {
    private var metalView: MTKView?
    private var cgLayer: CALayer?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true

        if let device = MTLCreateSystemDefaultDevice() {
            let mtk = MTKView(frame: bounds, device: device)
            mtk.autoresizingMask = [.width, .height]
            addSubview(mtk)
            metalView = mtk
        } else {
            let layer = CALayer()
            layer.frame = bounds
            self.layer?.addSublayer(layer)
            cgLayer = layer
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func present(framebuffer: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
        // TODO: implement Metal texture blit or CGImage creation.
    }
}

struct X11WindowHost: NSViewRepresentable {
    func makeNSView(context: Context) -> X11View { X11View(frame: .zero) }
    func updateNSView(_ nsView: X11View, context: Context) {}
}
