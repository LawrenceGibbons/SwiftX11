//
//  X11Callbacks.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 12/31/25.
//

import AppKit

typealias X11CreateCB = @convention(c) (
    UInt32,
    UnsafePointer<CChar>?,
    Int32,
    Int32
) -> Void

typealias X11CloseCB = @convention(c) (UInt32) -> Void

let swiftX11CreateCallback: X11CreateCB = { xwinID, titlePtr, width, height in
    let title = titlePtr.map { String(cString: $0) } ?? "SwiftX11 Window"
    Task { @MainActor in
        WindowRegistry.shared.createWindow(
            xid: xwinID,
            title: title,
            width: Int(width),
            height: Int(height)
        )
    }
}

let swiftX11CloseCallback: X11CloseCB = { xwinID in
    Task { @MainActor in
        WindowRegistry.shared.closeWindow(xid: xwinID)
    }
}
