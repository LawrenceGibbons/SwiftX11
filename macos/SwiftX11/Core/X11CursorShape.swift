//
//  X11CursorShape.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/20/26.
//

import AppKit

enum X11CursorShape: Int32 {
  case arrow = 0
  case iBeam = 1
  case crosshair = 2
  case pointingHand = 3
  case openHand = 4
  case closedHand = 5
  case resizeLeftRight = 6
  case resizeUpDown = 7
  case resizeDiagonal = 8
  case operationNotAllowed = 9
}

func nsCursor(for shape: X11CursorShape) -> NSCursor {
  switch shape {
  case .arrow: return .arrow
  case .iBeam: return .iBeam
  case .crosshair: return .crosshair
  case .pointingHand: return .pointingHand
  case .openHand: return .openHand
  case .closedHand: return .closedHand
  case .resizeLeftRight: return .resizeLeftRight
  case .resizeUpDown: return .resizeUpDown
  case .resizeDiagonal:
    // If your SDK has NSCursor.resizeDiagonal, use it.
    // Otherwise fall back to crosshair or resizeLeftRight.
    if #available(macOS 11.0, *), let diag = NSCursor.perform(NSSelectorFromString("resizeDiagonal"))?.takeUnretainedValue() as? NSCursor {
      return diag
    }
    return .crosshair
  case .operationNotAllowed: return .operationNotAllowed
  }
}
