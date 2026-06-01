import AppKit
import ObjectiveC

// ---------------------------------------------------------------------------
// Layout Recursion Guard
// ---------------------------------------------------------------------------
//
// Works around a macOS AppKit/SwiftUI interop bug where FBSScene updates
// (Stage Manager, display wake, etc.) trigger:
//
//   NSWindow._setFrameCommon:display:fromServer:
//     → displayIfNeeded → layoutSubtreeIfNeeded → [NSView layout]
//       → Auto Layout sets a subview's frame
//         → NSViewFrameDidChangeNotification posted
//           → observer calls NSWindow._setFrameCommon:display:fromServer:
//             → layoutSubtreeIfNeeded (RECURSIVE)
//               → _NSDetectedLayoutRecursion (CRASH)
//
// Fix: swizzle -[NSView layoutSubtreeIfNeeded] to prevent per-window
// re-entrancy.  If a window is already inside a layout pass, skip the
// recursive call.
// ---------------------------------------------------------------------------

private var inLayoutKey: UInt8 = 0

private extension NSWindow {
  var _swiftx11InLayout: Bool {
    get { objc_getAssociatedObject(self, &inLayoutKey) as? Bool ?? false }
    set { objc_setAssociatedObject(self, &inLayoutKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
  }
}

extension NSView {
  /// Call once at app startup to install the layout recursion guard.
  static func installLayoutRecursionGuard() {
    guard let original = class_getInstanceMethod(NSView.self, #selector(layoutSubtreeIfNeeded)),
          let replacement = class_getInstanceMethod(NSView.self, #selector(_swiftx11_layoutSubtreeIfNeeded))
    else { return }
    method_exchangeImplementations(original, replacement)
  }

  /// Swizzled replacement for -[NSView layoutSubtreeIfNeeded].
  /// Adds a per-window re-entrancy guard to prevent _NSDetectedLayoutRecursion.
  /// Also bails out if the view's window is being closed by SwiftX11 — AppKit's
  /// display-cycle observer can fire one more layout pass on a half-torn-down
  /// view tree, which is a known source of wild-PC crashes during CA commit.
  @objc private func _swiftx11_layoutSubtreeIfNeeded() {
    guard let w = self.window else {
      // No window — call original (safe, can't recurse on a window).
      _swiftx11_layoutSubtreeIfNeeded()
      return
    }
    if w._swiftx11InLayout {
      // Already inside this window's layout pass — skip to break the cycle.
      return
    }
    // Bail if the window is being torn down.  WindowRegistry sets
    // closingXids before calling controller.tearDown(); during that
    // window AppKit may queue one more layout pass on stale subviews.
    if let v = self as? X11View, v.xid != 0,
       MainActor.assumeIsolated({ WindowRegistry.shared.isClosing(xid: v.xid) }) {
      return
    }
    w._swiftx11InLayout = true
    defer { w._swiftx11InLayout = false }
    _swiftx11_layoutSubtreeIfNeeded()  // calls original (methods are swapped)
  }
}
