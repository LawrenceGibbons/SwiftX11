//
//  WindowRegistry.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 12/31/25.
//

import AppKit
import X11LowLevel

@MainActor
final class WindowRegistry {
  static let shared = WindowRegistry()
  private init() {}
  
  private var windows: [UInt32: X11WindowController] = [:]
  private var repaintWorkItemByXid: [UInt32: DispatchWorkItem] = [:]
  private var latestPixelSizeByXid: [UInt32: (w: Int32, h: Int32)] = [:]
  private var lastRepaintTimeByXid: [UInt32: CFTimeInterval] = [:]
  
  func createWindow(xid: UInt32, title: String, width: Int, height: Int) {
    // Avoid duplicates
    if let existing = windows[xid] {
      existing.window?.makeKeyAndOrderFront(nil)
      return
    }
    
    let controller = X11WindowController(xid: xid, title: title, width: width, height: height)
    windows[xid] = controller
    controller.showWindow(nil)
    
    // IMPORTANT: request an initial repaint at the *actual* pixel size once the window has laid out.
    DispatchQueue.main.async { [weak controller] in
        guard let win = controller?.window else { return }
        let sizePoints = win.contentLayoutRect.size
        let scale = win.backingScaleFactor
        let wPx = Int32(max(1, Int((sizePoints.width * scale).rounded(.down))))
        let hPx = Int32(max(1, Int((sizePoints.height * scale).rounded(.down))))
        x11_request_repaint(xid, wPx, hPx)
    }
  }
  
  func closeWindow(xid: UInt32) {
    repaintWorkItemByXid[xid]?.cancel()
    repaintWorkItemByXid.removeValue(forKey: xid)
    latestPixelSizeByXid.removeValue(forKey: xid)
    lastRepaintTimeByXid.removeValue(forKey: xid)
    
    guard let controller = windows.removeValue(forKey: xid) else { return }
    controller.close()
  }
  
  func presentFrame(xid: UInt32, bgra: UnsafeRawPointer, width: Int, height: Int, bytesPerRow: Int) {
    guard let controller = windows[xid], let view = controller.x11View else { return }
    view.presentBGRA(framebuffer: bgra, width: width, height: height, bytesPerRow: bytesPerRow)
  }
  
  func windowResized(xid: UInt32, sizePoints: CGSize, sizePixels: CGSize, scale: CGFloat) {
      let w = Int32(max(1, Int(sizePixels.width.rounded(.down))))
      let h = Int32(max(1, Int(sizePixels.height.rounded(.down))))
      latestPixelSizeByXid[xid] = (w: w, h: h)

      // Throttle: allow at most ~30fps during live resize
      let now = CACurrentMediaTime()
      let last = lastRepaintTimeByXid[xid] ?? 0
      if now - last >= (1.0 / 30.0) {
          lastRepaintTimeByXid[xid] = now
          repaintWorkItemByXid[xid]?.cancel()
          repaintWorkItemByXid.removeValue(forKey: xid)
          x11_request_repaint(xid, w, h)
          return
      }

      // Otherwise debounce to the final size shortly
      repaintWorkItemByXid[xid]?.cancel()
      let work = DispatchWorkItem { [weak self] in
          guard let self else { return }
          guard let sz = self.latestPixelSizeByXid[xid] else { return }
          self.lastRepaintTimeByXid[xid] = CACurrentMediaTime()
          x11_request_repaint(xid, sz.w, sz.h)
      }
      repaintWorkItemByXid[xid] = work
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.02, execute: work)
  }
  
  func flushRepaintNow(xid: UInt32) {
      repaintWorkItemByXid[xid]?.cancel()
      repaintWorkItemByXid.removeValue(forKey: xid)

      if let sz = latestPixelSizeByXid[xid] {
          x11_request_repaint(xid, sz.w, sz.h)
      }
  }
  
  func closeAll() {
    for (_, controller) in windows {
      controller.close()
    }
    windows.removeAll()
  }
  
}
