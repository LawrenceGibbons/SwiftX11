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
  
  func start() {
    guard !isRunning else { return }

    let display = self.display // capture on MainActor
    append("Starting X11 server on :\(display)…")

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
  func newWindow(title: String = "SwiftX11 Window", w: Int32 = 800, h: Int32 = 600)  -> UInt32  {
    guard isRunning else {
      append("Server not running; cannot create window.")
      return 0
    }
    let xid = x11_client_create_window(title, w, h)
    x11_client_set_window_title(xid, String(format: "SwiftX11 Window 0x%X", xid) )
    append(String(format: "Requested new window xid=0x%X", xid))
    return xid
  }
  
  @MainActor
  func showWindow(xid: UInt32) {
    x11_client_map_window(xid)

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
    //var batch: [x11_event_t] = []
    batch.reserveCapacity(max)

    assert(Thread.isMainThread)
    var n = 0
    while n < max {
      var cmd = x11_ui_cmd_t()
      guard x11_ui_pop_command(&cmd) else { break }
      batch.append(cmd)
      //var ev = x11_event_t()
      //guard x11_debug_pop_event(&ev) else { break }
      //batch.append(ev)
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
    //var batch: [x11_event_t] = []
    batch.reserveCapacity(max)

    var n = 0
    while n < max {
      var cmd = x11_ui_cmd_t()
      guard x11_ui_pop_command(&cmd) else { break }
      batch.append(cmd)
      //var ev = x11_event_t()
      //guard x11_debug_pop_event(&ev) else { break }
      //batch.append(ev)
      n += 1
    }
    if batch.isEmpty { return }

    // 2) Apply effects + logging in-order on MainActor
    Task { @MainActor in
      for cmd in batch {
        handleUICommand(cmd)
        // (optional) log formatting for UI commands
      }
      //for ev in batch {
      //  handleEventSideEffects(ev)
      //  if (!isLogPausedNow()),
      //     let line = format(ev, showMotion: (showMotion?() ?? false)) {
      //    append(line)
      //  }
      //}
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

    case X11_UI_RAISE:
      WindowRegistry.shared.raiseWindow(xid: cmd.xid)

    case X11_UI_MAP:
      WindowRegistry.shared.mapWindow(xid: cmd.xid)

    case X11_UI_UNMAP:
      WindowRegistry.shared.unmapWindow(xid: cmd.xid)

    case X11_UI_RESIZE:
      WindowRegistry.shared.applyX11Resize(xid: cmd.xid, wPx: cmd.w_px, hPx: cmd.h_px)

    case X11_UI_CREATE:
      WindowRegistry.shared.noteX11WindowCreated(
        xid: cmd.xid,
        parentXid: cmd.parent_xid,
        title: "SwiftX11 Window",
        width: Int(cmd.w_px),
        height: Int(cmd.h_px)
      )

    case X11_UI_DESTROY:
      WindowRegistry.shared.noteX11WindowDestroyed(xid: cmd.xid)

    case X11_UI_DAMAGE:
      // Convert to your existing damage handler signature.
      // If you want, you can change WindowRegistry.handleDamageEvent to take raw fields instead.
      WindowRegistry.shared.noteDamageRect(xid: cmd.xid, x: cmd.x_px, y: cmd.y_px, w: cmd.w_px, h: cmd.h_px)

    default:
      break
    }
  }
  
  
  @MainActor
  private func handleEventSideEffects(_ ev: x11_event_t) {
    assert(Thread.isMainThread)
    switch ev.type {

    case X11_EV_WINDOW_TITLE: do {
      let xid = ev.xid

      let len = Int(ev.u.win_title.title_len)
      let cappedLen = max(0, min(len, Int(X11_TEXT_MAX)))

      let bytes: [UInt8] = withUnsafeBytes(of: ev.u.win_title.title_utf8) { raw in
        Array(raw.prefix(cappedLen))
      }
      let title = String(bytes: bytes, encoding: .utf8) ?? "SwiftX11 Window"

      //Task { @MainActor in
      WindowRegistry.shared.setTitle(xid: xid, title: title)
      //}
    }

    case X11_EV_WINDOW_RAISE: do {
      let xid = ev.xid
      //Task { @MainActor in
      WindowRegistry.shared.raiseWindow(xid: xid)
      //}
    }

    case X11_EV_WINDOW_MAP: do {
      let xid = ev.xid
      //Task { @MainActor in
      WindowRegistry.shared.mapWindow(xid: xid)
      //}
    }

    case X11_EV_WINDOW_UNMAP: do {
      let xid = ev.xid
      //Task { @MainActor in
      WindowRegistry.shared.unmapWindow(xid: xid)
      //}
    }

    case X11_EV_WINDOW_RESIZE: do {
      let xid = ev.xid
      let wPx = ev.u.win_resize.width_px
      let hPx = ev.u.win_resize.height_px
      //Task { @MainActor in
      WindowRegistry.shared.applyX11Resize(xid: xid, wPx: wPx, hPx: hPx)
      //}
    }

    case X11_EV_WINDOW_CREATE: do {
      let xid = ev.xid
      let parent = ev.u.win_create.parent_xid
      let w = Int(ev.u.win_create.width_px)
      let h = Int(ev.u.win_create.height_px)
      
      //Task { @MainActor in
      WindowRegistry.shared.noteX11WindowCreated(
        xid: xid,
        parentXid: parent,
        title: "SwiftX11 Window",   // title may arrive later via TITLE event
        width: w,
        height: h
      )
      //}
    }

    case X11_EV_WINDOW_DESTROY: do {
      let xid = ev.xid
      //Task { @MainActor in
      WindowRegistry.shared.noteX11WindowDestroyed(xid: xid)
      //}
    }

    case X11_EV_WINDOW_DAMAGE: do {
      // Keep damage handling centralized in WindowRegistry.
      let evCopy = ev
      //Task { @MainActor in
      WindowRegistry.shared.handleDamageEvent(evCopy)
      //}
    }


    default:
      break
    }
  }
  
  
  private func format(_ ev: x11_event_t, showMotion: Bool) -> String? {
      let xid = String(format: "0x%X", ev.xid)
      let parent_xid = String(format: "0x%X", ev.u.win_create.parent_xid)
      

      switch ev.type {
      case X11_EV_WINDOW_CREATE:
        return "EV_WINDOW_CREATE xid=\(xid) parent=\(parent_xid) \(ev.u.win_create.width_px)x\(ev.u.win_create.height_px)"
          
      case X11_EV_WINDOW_DESTROY:
          return "EV_WINDOW_DESTROY xid=\(xid)"
          
      case X11_EV_WINDOW_TITLE:
          // Note: side-effects are handled in handleEventSideEffects(_:).
          let xidStr = xid
          let len = Int(ev.u.win_title.title_len)
          let cappedLen = max(0, min(len, Int(X11_TEXT_MAX)))
          let bytes: [UInt8] = withUnsafeBytes(of: ev.u.win_title.title_utf8) { raw in
            Array(raw.prefix(cappedLen))
          }
          let title = String(bytes: bytes, encoding: .utf8) ?? "(invalid utf8)"
          return "EV_WINDOW_TITLE xid=\(xidStr) title=\(title)"

      case X11_EV_POINTER_ENTER:
          return "EV_ENTER xid=\(xid) (\(ev.u.crossing.x_px),\(ev.u.crossing.y_px))"

      case X11_EV_POINTER_LEAVE:
          return "EV_LEAVE xid=\(xid) (\(ev.u.crossing.x_px),\(ev.u.crossing.y_px))"

      case X11_EV_POINTER_MOTION:
         return showMotion ? "EV_MOTION xid=\(xid) (\(ev.u.motion.x_px),\(ev.u.motion.y_px)) buttons=\(ev.u.motion.buttons)" : nil

      case X11_EV_POINTER_BUTTON:
          return "EV_BUTTON xid=\(xid) btn=\(ev.u.button.button) press=\(ev.u.button.is_press) buttons=\(ev.u.button.buttons)"

      case X11_EV_SCROLL:
          return "EV_SCROLL xid=\(xid) axis=\(ev.u.scroll.axis) ticks=\(ev.u.scroll.ticks)"

      case X11_EV_KEY:
          let modsHex = String(format: "0x%X", ev.u.key.modifiers)
          let isPress = ev.u.key.is_press != 0

          let len = Int(ev.u.key.text_len)
          let cappedLen = max(0, min(len, Int(X11_TEXT_MAX)))

          let text: String
          if cappedLen > 0 {
              let bytes: [UInt8] = withUnsafeBytes(of: ev.u.key.text_utf8) { raw in
                  Array(raw.prefix(cappedLen))
              }
              text = String(bytes: bytes, encoding: .utf8) ?? "(invalid utf8)"
          } else {
              text = ""
          }

          return """
          EV_KEY xid=\(xid) code=\(ev.u.key.keycode) press=\(isPress) mods=\(modsHex)\(text.isEmpty ? "" : " text=\"\(text)\"")
          """
      
      case X11_EV_FOCUS:
          return "EV_FOCUS xid=\(xid) focused=\(ev.u.focus.focused)"

      case X11_EV_WINDOW_RAISE:
          return "EV_WINDOW_RAISE xid=\(xid)"
        
      case X11_EV_WINDOW_MAP:
          return "EV_WINDOW_MAP xid=\(xid)"

      case X11_EV_WINDOW_UNMAP:
          return "EV_WINDOW_UNMAP xid=\(xid)"
        
      case X11_EV_WINDOW_RESIZE:
        return "EV_WINDOW_RESIZE xid=\(xid) \(ev.u.win_resize.width_px)x\(ev.u.win_resize.height_px)"
        
      case X11_EV_WINDOW_DAMAGE:
        return "EV_WINDOW_DAMAGE xid=\(xid) rect=(\(ev.u.win_damage.x_px),\(ev.u.win_damage.y_px)) \(ev.u.win_damage.w_px)x\(ev.u.win_damage.h_px)"
        
      default:
          return "EV type=\(ev.type.rawValue) xid=\(xid) size=\(ev.size)"
      }
  }

  deinit {
    NotificationCenter.default.removeObserver(self)
  }
}
