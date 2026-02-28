//
//  X11Trace.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/28/26.
//
//  Categorical trace flags for SwiftX11 Swift code.
//
//  Each flag gates a class of diagnostic print() calls.
//  Flip individual flags to true when debugging a specific subsystem.
//  The compiler optimizes away dead code when flags are false.
//

enum X11Trace {
  /// Resize flow: ensureHostSurface, handleDrawableSize, drawableSizeWillChange
  static let resize    = false

  /// Present/damage: snapshot, frame copy, Metal texture upload
  static let present   = false

  /// Window lifecycle: presentable, attach/settle, MTK create, makeKey
  static let lifecycle = false

  /// Input events: cursor, first responder
  static let input     = false
}
