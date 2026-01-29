//
//  X11DebugInspector.swift
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/2/26.
//

import SwiftUI
import Combine
import AppKit
import X11LowLevel

@MainActor
final class X11DebugInspectorModel: ObservableObject {
  @Published var text: String = "No snapshot yet."

  func refresh() {
    var snap = x11_debug_snapshot_t()
    guard x11_debug_get_snapshot(&snap) != 0 else {
      text = "Snapshot failed."
      return
    }

    func hex(_ x: UInt32) -> String { String(format: "0x%X", x) }

    var lines: [String] = []
    lines.append("=== SwiftX11 Inspector Snapshot ===")
    lines.append("Queue: count=\(snap.q_count) drops=\(snap.q_push_drops) coalesce=\(snap.q_motion_overwrites)")
    lines.append("destroyWaits=\(snap.destroy_waits)")
    lines.append("Routing: pointer=\(hex(snap.pointer_xid)) focus=\(hex(snap.focus_xid)) drag=\(hex(snap.drag_xid)) buttons=\(hex(snap.buttons))")
    lines.append("Windows: \(snap.window_count)")
    lines.append("  xid        size(px)     damaged")

    let n = Int(snap.window_count)
    if n == 0 {
      lines.append("  (none)")
    } else {
      withUnsafeBytes(of: snap.windows) { rawBuf in
        let rows = rawBuf.bindMemory(to: x11_debug_window_row_t.self)
        if rows.isEmpty {
          lines.append("  (none)")
        } else {
          for i in 0..<min(n, rows.count) {
            let w = rows[i]
            lines.append(String(format: "  %-10@ %4d x %-4d   %d",
                                NSString(string: hex(w.xid)),
                                w.w_px,
                                w.h_px,
                                w.damaged))
          }
        }
      }
    }

    text = lines.joined(separator: "\n")
  }
}

struct X11DebugInspectorView: View {
  @StateObject private var model = X11DebugInspectorModel()

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("Inspector").font(.headline)
        Spacer()
        Button("Snapshot") { model.refresh() }
      }

      ScrollView {
        Text(model.text)
          .font(.system(.caption, design: .monospaced))
          .frame(maxWidth: .infinity, alignment: .leading)
          .textSelection(.enabled)
          .padding(8)
      }
      .background(.background)
      .overlay(
        RoundedRectangle(cornerRadius: 8)
          .stroke(.quaternary, lineWidth: 1)
      )
    }
    .padding()
  }
}

@MainActor
final class X11DebugInspectorWindowController {
  static let shared = X11DebugInspectorWindowController()

  private var window: NSWindow?

  func show() {
    if let w = window {
      w.makeKeyAndOrderFront(nil)
      NSApp.activate(ignoringOtherApps: true)
      return
    }

    let root = X11DebugInspectorView()
    let hosting = NSHostingController(rootView: root)

    let w = NSWindow(contentViewController: hosting)
    w.title = "SwiftX11 Inspector"
    print("[WIN] setContentSize about to run in X11DebugInspectorWindowController")

    w.setContentSize(NSSize(width: 540, height: 420))
    w.styleMask = [.titled, .closable, .resizable, .miniaturizable]
    w.isReleasedWhenClosed = false

    NotificationCenter.default.addObserver(
      forName: NSWindow.willCloseNotification,
      object: w,
      queue: .main
    ) { [weak wc = self] _ in
      guard let wc else { return }
      Task { @MainActor in
        wc.window = nil
      }
    }
    
    window = w
    w.center()
    w.makeKeyAndOrderFront(nil)
    NSApp.activate(ignoringOtherApps: true)
  }
}
