//
//  X11FramePresenter.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/1/26.
//

import AppKit

typealias X11PresentFrameCB = @convention(c) (
    UInt32,
    UnsafeRawPointer?,
    Int32,
    Int32,
    Int32
) -> Void

let swiftX11PresentFrame: X11PresentFrameCB = { xid, ptr, w, h, bpr in
    guard let ptr else { return }
    Task { @MainActor in
        WindowRegistry.shared.presentFrame(
            xid: xid,
            bgra: ptr,
            width: Int(w),
            height: Int(h),
            bytesPerRow: Int(bpr)
        )
    }
    print(String(format: "PRESENT xid=0x%X %dx%d", xid, w, h))
}
