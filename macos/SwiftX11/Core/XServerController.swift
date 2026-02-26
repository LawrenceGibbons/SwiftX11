import Foundation
import Combine
import X11LowLevel
import QuartzCore

@MainActor
final class XServerController: ObservableObject {
  @Published var isRunning: Bool = false
  @Published var display: Int = 0
  @Published var logText: String = ""
  @Published var didInstallLogControls = false

  private var drainTimer: DispatchSourceTimer?
  private var isPaused:    (() -> Bool)?
  private var showMotion:  (() -> Bool)?
  private var showStats:   (() -> Bool)?
  private var drainPaused: (() -> Bool)?
  private var didLogDrainPaused = false

  
  init() {
    // // Register Swift callbacks with the C shim
    // x11_register_callbacks(
    //   swiftX11CreateCallback,
    //   swiftX11CloseCallback
    // )
    // append("Registered X11 callbacks")
    
//    x11_register_frame_presenter(swiftX11PresentFrame)
//    append("Registered frame presenter")
    
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
    
    start()
  }
  
  
  @objc private func _handleStartRequested(_ note: Notification) {
    start()
  }

  @objc private func _handleStopRequested(_ note: Notification) {
    stop()
  }
  
  static let buildVersion = String(cString: swiftx11_version())

  func start() {
    guard !isRunning else { return }

    let display = self.display // capture on MainActor
    append("SwiftX11 v\(Self.buildVersion) — starting on :\(display)…")

    // x11_start_server spins its own runloop thread; keep the call on MainActor to
    // avoid Swift 6 Sendable capture warnings from DispatchQueue.async.
    //let ok = x11_start_server(Int32(display))
    let ok = x11_start_server(Int32(1))
    
    isRunning = ok
    append(ok ? "Server started" : "Failed to start server")
    if ok {
      GlobalPointerTracker.shared.start()
      startDrainTimer() 
    }
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
  
  func setLogControls(
    isPaused:    @escaping () -> Bool,
    showMotion:  @escaping () -> Bool,
    showStats:   @escaping () -> Bool,
    drainPaused: @escaping () -> Bool
  ) {
    self.isPaused = isPaused
    self.showMotion = showMotion
    self.showStats = showStats
    self.drainPaused = drainPaused
  }

  @MainActor
  func append(_ line: String) {
    let s = "[\(tstamp())] \(line)"
    logText += s + "\n"
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
  
  private var lastStatsPrintTime: CFTimeInterval = 0

  private func drainEvents(max: Int) {
    if isDrainPausedNow() {
      if !didLogDrainPaused {
        didLogDrainPaused = true
        Task { @MainActor in self.append("drain paused; not popping events") }
      }
      return
    }
    didLogDrainPaused = false
    
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
  
  private func shouldShowStats() -> Bool {
    showStats?() ?? false
  }

  private func isLogPausedNow() -> Bool {
    isPaused?() ?? false
  }

  private func isDrainPausedNow() -> Bool {
    drainPaused?() ?? false
  }

  private func maybeAppendQueueStats(qBefore: Int, drained: Int) {
      guard shouldShowStats() else { return }          // your toggle
      guard !isLogPausedNow() else { return }          // your “Freeze log output” toggle

      let now = CACurrentMediaTime()
      guard now - lastStatsPrintTime >= 1.0 else { return }
      lastStatsPrintTime = now

      append("EVQ qBefore=\(qBefore) drained=\(drained)")
  }
  
  
  @MainActor
  private func handleUICommand(_ cmd: x11_ui_cmd_t) {
    print("[UI_CMD] type=\(cmd.type) xid=0x\(String(cmd.xid, radix:16))")
    switch cmd.type {

    case X11_UI_TITLE:
      let xid = cmd.xid
      let len = Int(cmd.title_len)
      let cappedLen = max(0, min(len, 32))

      let title = withUnsafeBytes(of: cmd.title_utf8) { raw -> String in
        let bytes = Array(raw.prefix(cappedLen))
        return String(bytes: bytes, encoding: .utf8) ?? "SwiftX11 Window"
      }

      WindowRegistry.shared.setTitle(xid: xid, title: title)

    case X11_UI_SET_CURSOR:
      print ("[UI_CMD] SET_CURSOR cmd.host_xid=0x\(String(cmd.cursor.host_xid, radix:16))")
      // Prefer the explicit payload.
      //let host: UInt32 = (cmd.cursor.host_xid != 0) ? cmd.cursor.host_xid : cmd.xid
      //let shapeRaw: Int32 = cmd.cursor.shape
      let host = cmd.cursor.host_xid
      let shapeRaw = cmd.cursor.shape


      print("[UI_CMD] SET_CURSOR host=0x\(String(host, radix:16)) shapeRaw=\(shapeRaw) cursorXid=0x\(String(cmd.cursor.cursor_xid, radix:16))")

      if let v = WindowRegistry.shared.viewForHostXid(host) {
        v.applyCursorShapeRaw(shapeRaw)
      } else {
        print("[UI_CMD] SET_CURSOR: no viewForHost(0x\(String(host, radix:16)))")
      }
      
    case X11_UI_RAISE:
      WindowRegistry.shared.raiseWindow(xid: cmd.xid)

    case X11_UI_MAP:
      WindowRegistry.shared.mapWindow(xid: cmd.xid)

    case X11_UI_UNMAP:
      WindowRegistry.shared.unmapWindow(xid: cmd.xid)

    case X11_UI_RESIZE:
      // xxx temp
      print("[SIZE][UI_CMD] applyX11Resize xid=0x\(String(format:"%X", cmd.xid)) wX11=\(cmd.w_u) hX11=\(cmd.h_u)")
      WindowRegistry.shared.applyX11Resize(xid: cmd.xid, wX11: cmd.w_u, hX11: cmd.h_u)

    case X11_UI_CREATE:
      WindowRegistry.shared.noteX11WindowCreated(
        xid: cmd.xid,
        parentXid: cmd.parent_xid,
        title: "SwiftX11 Window",
        width: Int(cmd.w_u),
        height: Int(cmd.h_u)
      )

    case X11_UI_DESTROY:
      WindowRegistry.shared.noteX11WindowDestroyed(xid: cmd.xid)

    case X11_UI_DAMAGE:
      print("[UI_CMD] DAMAGE xid=0x\(String(cmd.xid, radix:16)) rect=\(cmd.x_u),\(cmd.y_u) \(cmd.w_u)x\(cmd.h_u)")
      // Convert to your existing damage handler signature.
      // If you want, you can change WindowRegistry.handleDamageEvent to take raw fields instead.
      WindowRegistry.shared.noteDamageRect(xid: cmd.xid, x: cmd.x_u, y: cmd.y_u, w: cmd.w_u, h: cmd.h_u)

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
