import Foundation
import Combine
import AppKit
import Metal
import X11LowLevel
import QuartzCore

@MainActor
final class XServerController: ObservableObject {
  @Published var isRunning: Bool = false
  @Published var display: Int = 0
  @Published var logText: String = ""
  @Published var didInstallLogControls = false

  private var drainTimer: DispatchSourceTimer?

  init() {
    registerLogBridge()

    NotificationCenter.default.addObserver(
      self,
      selector: #selector(_handleStartRequested(_:)),
      name: .x11StartRequested,
      object: nil
    )

    NotificationCenter.default.addObserver(
      self,
      selector: #selector(_handleStopRequested(_:)),
      name: .x11StopRequested,
      object: nil
    )

    NotificationCenter.default.addObserver(
      self,
      selector: #selector(_handleLogMessage(_:)),
      name: .x11LogMessage,
      object: nil
    )

    checkFontsAndStart()
  }


  @objc private func _handleStartRequested(_ note: Notification) {
    start()
  }

  @objc private func _handleStopRequested(_ note: Notification) {
    stop()
  }
  
  static let buildVersion = String(cString: swiftx11_version())
  static let buildDate = String(cString: swiftx11_build_date())

  /// Pre-flight checks before starting the server.
  private func checkFontsAndStart() {
    // ── Metal check ──────────────────────────────────────────────────
    if MTLCreateSystemDefaultDevice() == nil {
      let alert = NSAlert()
      alert.messageText = "Metal GPU Not Available"
      alert.informativeText = """
        SwiftX11 requires a Metal-capable GPU for rendering. \
        This Mac does not appear to have Metal support.

        Metal is available on all Macs from 2012 or later. \
        If you are running in a virtual machine, ensure GPU \
        passthrough is enabled.
        """
      alert.alertStyle = .critical
      alert.addButton(withTitle: "OK")
      alert.runModal()
      NSApp.terminate(nil)
      return
    }

    // ── Font check ───────────────────────────────────────────────────
    let fontDir = "/opt/X11/share/fonts/misc/fonts.dir"
    if FileManager.default.fileExists(atPath: fontDir) {
      start()
      return
    }

    // Fonts not found — show dialog on main thread
    let alert = NSAlert()
    alert.messageText = "X11 Fonts Not Found"
    alert.informativeText = """
      SwiftX11 could not find X11 bitmap fonts at:
      /opt/X11/share/fonts/

      Without these fonts, legacy X11 applications (xterm, xcalc, xclock) \
      will not render text correctly.

      You can install fonts from the SwiftX11 disk image \
      (see the "X11 Fonts" folder and its INSTALL.txt), \
      or by installing XQuartz (xquartz.org).
      """
    alert.alertStyle = .warning
    alert.addButton(withTitle: "Continue Without Fonts")
    alert.addButton(withTitle: "Quit")

    let response = alert.runModal()
    if response == .alertSecondButtonReturn {
      NSApp.terminate(nil)
      return
    }

    // User chose to continue without fonts
    append("WARNING: X11 fonts not found at /opt/X11/share/fonts/ — text rendering may be broken")
    start()
  }

  func start() {
    guard !isRunning else { return }

    // Ensure X11 clients can find app-defaults (e.g., XCalc, XTerm resources).
    // The compiled-in libXt default (/usr/lib/X11/...) doesn't exist on macOS;
    // app-defaults live in /opt/X11/share/X11/app-defaults/.
    ensureXFilesSearchPath()

    let display = max(self.display, 1) // capture on MainActor; minimum display :1
    append("SwiftX11 v\(Self.buildVersion) — starting on :\(display)…")

    // Read network settings (SettingsStore is injected via environment;
    // fall back to defaults if not available yet).
    let enableTCP = UserDefaults.standard.object(forKey: "enableTCP") as? Bool ?? true
    let enableUnix = UserDefaults.standard.object(forKey: "enableUnixSocket") as? Bool ?? true
    let bindAddr = UserDefaults.standard.object(forKey: "tcpBindAddress") as? String ?? "0.0.0.0"

    let ok = x11_start_server_ex(
      Int32(display),
      enableTCP,
      enableUnix,
      bindAddr
    )
    
    isRunning = ok
    append(ok ? "Server started" : "Failed to start server")
    if ok {
      GlobalPointerTracker.shared.start()
      startDrainTimer()
      registerClipboardBridge()
    }
  }

  /// Set XFILESEARCHPATH so X11 clients find app-defaults on macOS.
  /// Also propagates to new terminal windows via launchctl setenv.
  private func ensureXFilesSearchPath() {
    let key = "XFILESEARCHPATH"
    let path = "/opt/X11/share/X11/%T/%N%C%S:/opt/X11/share/X11/%T/%N%S"

    // Set in our process (inherited by any child processes we spawn).
    // 0 = don't overwrite if already set by the user.
    setenv(key, path, 0)

    // Propagate to new terminal windows opened after SwiftX11 starts.
    // launchctl setenv makes the variable available to all new launchd children.
    if ProcessInfo.processInfo.environment[key] == nil {
      let task = Process()
      task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
      task.arguments = ["setenv", key, path]
      task.standardOutput = FileHandle.nullDevice
      task.standardError = FileHandle.nullDevice
      try? task.run()
    }
  }

  /// Register clipboard callbacks so C++ can read/write macOS pasteboard.
  /// IMPORTANT: These callbacks are invoked from the xproto background thread.
  /// NSPasteboard read operations (getter, changeCount) are called directly —
  /// they are safe enough from background threads and MUST NOT use
  /// DispatchQueue.main.sync because that deadlocks when the main thread is
  /// in NSMenu tracking mode (e.g., Vivado Edit → Copy).
  /// The setter uses DispatchQueue.main.async (fire-and-forget) for writes.
  private func registerClipboardBridge() {
    // Getter: read NSPasteboard text into buffer, return byte count.
    // Called from xproto thread during ConvertSelection when no X11 owner.
    // Direct access — no main-thread dispatch (avoids menu-tracking deadlock).
    let getter: x11_clipboard_get_text_fn = { buf, maxLen in
      guard let buf = buf, maxLen > 0 else { return 0 }
      guard let text = NSPasteboard.general.string(forType: .string) else { return 0 }
      var copyLen: Int = 0
      text.withCString { cstr in
        let len = strlen(cstr)
        copyLen = min(Int(maxLen), len)
        if copyLen > 0 { memcpy(buf, cstr, copyLen) }
      }
      return UInt32(copyLen)
    }

    // Setter: write text to NSPasteboard.
    // Called from xproto thread during clipboard capture (X11→macOS sync).
    // Uses async dispatch — the caller doesn't need to wait for completion.
    let setter: x11_clipboard_set_text_fn = { text, len in
      guard let text = text, len > 0 else { return }
      let data = Data(bytes: text, count: Int(len))
      guard let s = String(data: data, encoding: .utf8) else { return }
      DispatchQueue.main.async {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
      }
    }

    // Change-count: lets C++ detect when macOS clipboard changed externally.
    // Called from xproto thread during SetSelectionOwner and ConvertSelection.
    // Direct access — no main-thread dispatch (avoids menu-tracking deadlock).
    let changeCounter: x11_clipboard_change_count_fn = {
      return Int64(NSPasteboard.general.changeCount)
    }

    x11_clipboard_register(getter, setter)
    x11_clipboard_register_change_count(changeCounter)
    append("Clipboard bridge registered")
  }

  /// Register C++ → Swift log callback so protocol traces appear in the UI log panel.
  private func registerLogBridge() {
    let callback: x11_log_callback_fn = { level, msg in
      guard let msg = msg else { return }
      let str = String(cString: msg)
      DispatchQueue.main.async {
        NotificationCenter.default.post(
          name: .x11LogMessage,
          object: nil,
          userInfo: ["message": str, "level": level as Any]
        )
      }
    }
    x11_register_log_callback(callback)
  }

  @objc private func _handleLogMessage(_ note: Notification) {
    guard let msg = note.userInfo?["message"] as? String else { return }
    // Strip trailing newline if present (append() adds its own)
    let trimmed = msg.hasSuffix("\n") ? String(msg.dropLast()) : msg
    append(trimmed)
  }
  
  func stop() {
    guard isRunning else { return }
    append("Stopping X11 server…")

    // Stop is also quick; keep it on MainActor to avoid Swift 6 Sendable capture warnings.
    x11_stop_server()  // enqueues EV_WINDOW_DESTROY (and others)

    // Final drain so destroy events show up in the log UI
    drainEventsForce(max: 4096)

    isRunning = false
    stopDrainTimer()
    GlobalPointerTracker.shared.stop()
    append("Server stopped")
  }
  
  private func tstamp() -> String {
    String(format: "%.6f", CACurrentMediaTime())
  }
  
  private var pendingLogLines: [String] = []
  private var logFlushTimer: Timer?

  @MainActor
  func append(_ line: String) {
    let s = "[\(tstamp())] \(line)"
    pendingLogLines.append(s)
    // Coalesce rapid log updates to avoid overwhelming SwiftUI's
    // AttributeGraph.  DispatchQueue.main.async can fire during SwiftUI's
    // layout pass, crashing in propagate_dirty.  A timer with a minimum
    // 200ms interval ensures the @Published update happens in a clean
    // runloop cycle, well after any in-flight SwiftUI transactions.
    if logFlushTimer == nil {
      logFlushTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: false) { [weak self] _ in
        Task { @MainActor [weak self] in
          guard let self else { return }
          self.logFlushTimer = nil
          guard !self.pendingLogLines.isEmpty else { return }
          self.logText += self.pendingLogLines.joined(separator: "\n") + "\n"
          self.pendingLogLines.removeAll(keepingCapacity: true)
        }
      }
    }
  }
  
  @MainActor
  func newWindow(title: String = "SwiftX11 Window", w: Int32 = 800, h: Int32 = 600) -> UInt32 {
    append("newWindow disabled: X windows must be created by real X11 clients (xeyes/xterm).")
    return 0
  }
  
  @MainActor
  func showWindow(xid: UInt32) {
    x11_post_window_map(xid)

    // Force processing immediately (removes the timer as a variable)
    drainEventsForce(max: 256)
  }
  
  private func startDrainTimer() {
    stopDrainTimer()
    
    let t = DispatchSource.makeTimerSource(queue: DispatchQueue.main)
    t.schedule(deadline: .now(), repeating: .milliseconds(33)) // ~30 Hz
    t.setEventHandler { [weak self] in
      self?.drainEvents(max: 200)
    }
    t.resume()
    drainTimer = t
  }
  
  private func stopDrainTimer() {
    drainTimer?.cancel()
    drainTimer = nil
  }
  

  private func drainEvents(max: Int) {
    // 1) Pop off the C queue here
    var batch: [x11_ui_cmd_t] = []
    batch.reserveCapacity(max)

    assert(Thread.isMainThread)
    var n = 0
    while n < max {
      var cmd = x11_ui_cmd_t()
      guard x11_ui_pop_command(&cmd) else { break }
      batch.append(cmd)
      n += 1
    }
    if batch.isEmpty { return }

    // 2) Apply effects + logging in-order on MainActor
    Task { @MainActor in
      for cmd in batch {
        handleUICommand(cmd)
        // (optional) log formatting for UI commands
      }
      // for ev in batch {
      //   handleEventSideEffects(ev)
      //   if (!isLogPausedNow()),
      //      let line = format(ev, showMotion: (showMotion?() ?? false)) {
      //     append(line)
      //   }
      // }
    }
  }

  
  private func drainEventsForce(max: Int) {
    // 1) Pop off the C queue here (NOT MainActor)
    var batch: [x11_ui_cmd_t] = []
    batch.reserveCapacity(max)

    var n = 0
    while n < max {
      var cmd = x11_ui_cmd_t()
      guard x11_ui_pop_command(&cmd) else { break }
      batch.append(cmd)
      n += 1
    }
    if batch.isEmpty { return }

    // 2) Apply effects + logging in-order on MainActor
    Task { @MainActor in
      for cmd in batch {
        handleUICommand(cmd)
        // (optional) log formatting for UI commands
      }
    }
  }
  
  @MainActor
  private func handleUICommand(_ cmd: x11_ui_cmd_t) {
    if X11Trace.lifecycle { print("[UI_CMD] type=\(cmd.type) xid=0x\(String(cmd.xid, radix:16))") }
    switch cmd.type {

    case X11_UI_TITLE:
      let xid = cmd.xid
      let len = Int(cmd.title_len)
      let cappedLen = max(0, min(len, 128))

      let title = withUnsafeBytes(of: cmd.title_utf8) { raw -> String in
        let bytes = Array(raw.prefix(cappedLen))
        return String(bytes: bytes, encoding: .utf8) ?? "SwiftX11 Window"
      }

      WindowRegistry.shared.setTitle(xid: xid, title: title)

    case X11_UI_SET_CURSOR:
      let host = cmd.cursor.host_xid
      let shapeRaw = cmd.cursor.shape

      if X11Trace.input { print("[UI_CMD] SET_CURSOR host=0x\(String(host, radix:16)) shapeRaw=\(shapeRaw) cursorXid=0x\(String(cmd.cursor.cursor_xid, radix:16))") }

      if let v = WindowRegistry.shared.viewForHostXid(host) {
        v.applyCursorShapeRaw(shapeRaw)
      } else {
        if X11Trace.input { print("[UI_CMD] SET_CURSOR: no viewForHost(0x\(String(host, radix:16)))") }
      }
      
    case X11_UI_RAISE:
      WindowRegistry.shared.raiseWindow(xid: cmd.xid)

    case X11_UI_MAP:
      WindowRegistry.shared.mapWindow(xid: cmd.xid)

    case X11_UI_UNMAP:
      WindowRegistry.shared.unmapWindow(xid: cmd.xid)

    case X11_UI_RESIZE:
      if X11Trace.resize { print("[SIZE][UI_CMD] applyX11Resize xid=0x\(String(format:"%X", cmd.xid)) wX11=\(cmd.w_u) hX11=\(cmd.h_u)") }
      WindowRegistry.shared.applyX11Resize(xid: cmd.xid, wX11: cmd.w_u, hX11: cmd.h_u)

    case X11_UI_CREATE:
      let isOverrideRedirect = (cmd.flags & UInt32(X11_UI_FLAG_OVERRIDE_REDIRECT)) != 0
      WindowRegistry.shared.noteX11WindowCreated(
        xid: cmd.xid,
        parentXid: cmd.parent_xid,
        title: "SwiftX11 Window",
        x: Int(cmd.x_u),
        y: Int(cmd.y_u),
        width: Int(cmd.w_u),
        height: Int(cmd.h_u),
        overrideRedirect: isOverrideRedirect
      )

    case X11_UI_DESTROY:
      WindowRegistry.shared.noteX11WindowDestroyed(xid: cmd.xid)

    case X11_UI_MOVE:
      // Override-redirect windows (popup menus) use X11 root coords.
      // Convert to macOS screen coords and move the NSWindow.
      WindowRegistry.shared.moveWindow(xid: cmd.xid, x11X: Int(cmd.x_u), x11Y: Int(cmd.y_u))

    case X11_UI_DAMAGE:
      // The shared C++ damage accumulator carries the actual rect data.
      // This UI command just signals Swift to schedule a present.
      WindowRegistry.shared.noteDamage(xid: cmd.xid, x: cmd.x_u, y: cmd.y_u, w: cmd.w_u, h: cmd.h_u)

    case X11_UI_WARP_POINTER:
      let hostXid = cmd.xid
      let x = CGFloat(cmd.x_u)
      let y = CGFloat(cmd.y_u)

      if hostXid == 0 {
        // Relative warp: delta from current pointer position
        let current = NSEvent.mouseLocation
        // NSEvent.mouseLocation is bottom-left origin; CGWarpMouseCursorPosition is top-left.
        // Use virtual desktop max-Y for multi-monitor correctness.
        let vmaxY = WindowRegistry.virtualDesktopMaxY
        let cgPt = CGPoint(x: current.x + x, y: vmaxY - current.y + y)
        CGWarpMouseCursorPosition(cgPt)
      } else if let view = WindowRegistry.shared.viewForHostXid(hostXid),
                let window = view.window {
        // Window-relative: convert host-local coords to screen coords
        // X11 coords: origin at top-left; Cocoa: bottom-left
        let contentRect = window.contentView?.frame ?? window.frame
        let cocoaLocal = NSPoint(x: x, y: contentRect.height - y)
        let screenPt = window.convertPoint(toScreen: cocoaLocal)
        // Use virtual desktop max-Y for multi-monitor correctness.
        let vmaxY = WindowRegistry.virtualDesktopMaxY
        let cgPt = CGPoint(x: screenPt.x, y: vmaxY - screenPt.y)
        CGWarpMouseCursorPosition(cgPt)
      }

    case X11_UI_SHAPE_CHANGED:
      let hostXid = cmd.xid
      let shaped = x11_shape_is_shaped(hostXid)
      if let view = WindowRegistry.shared.viewForHostXid(hostXid) {
        view.setWindowTransparency(shaped)
      }

    case X11_UI_SIZE_HINTS:
      // WM_NORMAL_HINTS: apply min/max/increment to NSWindow
      WindowRegistry.shared.applySizeHints(
        xid: cmd.xid,
        minW: Int(cmd.min_w), minH: Int(cmd.min_h),
        maxW: Int(cmd.max_w), maxH: Int(cmd.max_h),
        incW: Int(cmd.inc_w), incH: Int(cmd.inc_h)
      )

    case X11_UI_WINDOW_TYPE:
      // _NET_WM_WINDOW_TYPE or _NET_WM_STATE encoded as type atom
      WindowRegistry.shared.applyWindowType(xid: cmd.xid, typeAtom: cmd.flags)

    case X11_UI_INITIAL_STATE:
      // WM_HINTS initial_state: 3 = IconicState (start minimized)
      WindowRegistry.shared.applyInitialState(xid: cmd.xid, state: cmd.flags)

    case X11_UI_TRANSIENT_FOR:
      // WM_TRANSIENT_FOR: dialog parent window relationship
      WindowRegistry.shared.applyTransientFor(xid: cmd.xid, transientForXid: cmd.parent_xid)

    default:
      break
    }
  }
  
  

  
  private func format(_ ev: x11_ui_cmd_t, showMotion: Bool) -> String? {
      let xid = String(format: "0x%X", ev.xid)
      let parent_xid = String(format: "0x%X", ev.parent_xid)
      

      switch ev.type {
      case X11_UI_CREATE:
        return "X11_UI_CREATE xid=\(xid) parent=\(parent_xid) \(ev.w_u)x\(ev.h_u)"
          
      case X11_UI_DESTROY:
          return "EV_WINDOW_DESTROY xid=\(xid)"
          
      case X11_UI_TITLE:
          // Note: side-effects are handled in handleEventSideEffects(_:).
          let xidStr = xid
          let len = Int(ev.title_len)
          let cappedLen = max(0, min(len, Int(32)))
          let bytes: [UInt8] = withUnsafeBytes(of: ev.title_utf8) { raw in
            Array(raw.prefix(cappedLen))
          }
          let title = String(bytes: bytes, encoding: .utf8) ?? "(invalid utf8)"
          return "X11_UI_TITLE xid=\(xidStr) title=\(title)"

      case X11_UI_RAISE:
          return "X11_UI_RAISE xid=\(xid)"
        
      case X11_UI_MAP:
          return "X11_UI_MAP xid=\(xid)"

      case X11_UI_UNMAP:
          return "X11_UI_UNMAP xid=\(xid)"
        
      case X11_UI_RESIZE:
        return "X11_UI_RESIZE xid=\(xid) \(ev.w_u)x\(ev.h_u)"
        
      case X11_UI_DAMAGE:
        return "X11_UI_DAMAGE xid=\(xid) rect=(\(ev.x_u),\(ev.y_u)) \(ev.w_u)x\(ev.h_u)"
        
      default:
          return "unknown UI type=\(ev.type.rawValue) xid=\(xid)"
      }
  }

  deinit {
    NotificationCenter.default.removeObserver(self)
  }
}
