import Foundation
import Combine
import X11LowLevel
import QuartzCore

@MainActor
final class XServerController: ObservableObject {
  @Published var isRunning: Bool = false
  @Published var display: Int = 0
  @Published var logLines: [String] = []
  @Published var didInstallLogControls = false

  private var drainTimer: DispatchSourceTimer?
  private var isPaused:    (() -> Bool)?
  private var showMotion:  (() -> Bool)?
  private var showStats:   (() -> Bool)?
  private var drainPaused: (() -> Bool)?

  
  init() {
    // Register Swift callbacks with the C shim
    x11_register_callbacks(
      swiftX11CreateCallback,
      swiftX11CloseCallback
    )
    append("Registered X11 callbacks")
    
    x11_register_frame_presenter(swiftX11PresentFrame)
    append("Registered frame presenter")
    
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
    if ok { startDrainTimer() }
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
    append("Server stopped")
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
    logLines.append("[\(Date())] \(line)")
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
      append("drain paused; not popping events")
      return 
    }  // optional: don’t drain at all
    
    // sample before
    let qBefore = Int(x11_events_count())
    if qBefore > 0 {
      append("drain tick: qBefore=\(qBefore)")
    }

    var n = 0
    while n < max {
      var ev = x11_event_t()
      guard x11_debug_pop_event(&ev) else { break }
      
      handleEventSideEffects(ev)
      
      if (!isLogPausedNow()), let line = format(ev, showMotion: (showMotion?() ?? false)) {
        append(line)
      }
      n += 1
    }
    
    maybeAppendQueueStats(qBefore: qBefore, drained: n)
  }
  
  private func drainEventsForce(max: Int) {
    let qBefore = Int(x11_events_count())
    var n = 0
    while n < max {
      var ev = x11_event_t()
      guard x11_debug_pop_event(&ev) else { break }

      handleEventSideEffects(ev)

      // Force append, but still respect showMotion toggle
      if let line = format(ev, showMotion: (showMotion?() ?? false)) {
        append(line)
      }
      n += 1
    }

    // Optional: still show stats if enabled (or always show once on stop)
    maybeAppendQueueStats(qBefore: qBefore, drained: n)
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

      let co = x11_debug_motion_overwrites()
      let drops = x11_debug_push_drops()

      append("EVQ qBefore=\(qBefore) drained=\(drained) coalesce=\(co) drops=\(drops)")
  }
  
  func dumpEventQueue(maxItems: UInt32 = 32) {
      var buf = [CChar](repeating: 0, count: 8192)

      let ok = buf.withUnsafeMutableBufferPointer { ptr -> Bool in
          guard let base = ptr.baseAddress else { return false }
          return x11_debug_dump_queue(base, ptr.count, maxItems)
      }

      if ok {
          append(String(cString: buf))
      } else {
          append("x11_debug_dump_queue: failed")
      }
  }
  
  private func handleEventSideEffects(_ ev: x11_event_t) {
    switch ev.type {

    case X11_EV_WINDOW_TITLE: do {
      let xid = ev.xid

      let len = Int(ev.u.win_title.title_len)
      let cappedLen = max(0, min(len, Int(X11_TEXT_MAX)))

      let bytes: [UInt8] = withUnsafeBytes(of: ev.u.win_title.title_utf8) { raw in
        Array(raw.prefix(cappedLen))
      }
      let title = String(bytes: bytes, encoding: .utf8) ?? "SwiftX11 Window"

      Task { @MainActor in
        WindowRegistry.shared.setTitle(xid: xid, title: title)
      }
    }

    case X11_EV_WINDOW_RAISE: do {
      let xid = ev.xid
      Task { @MainActor in
        WindowRegistry.shared.raiseWindow(xid: xid)
      }
    }

    case X11_EV_WINDOW_MAP: do {
      let xid = ev.xid
      Task { @MainActor in
        WindowRegistry.shared.mapWindow(xid: xid)
      }
    }

    case X11_EV_WINDOW_UNMAP: do {
      let xid = ev.xid
      Task { @MainActor in
        WindowRegistry.shared.unmapWindow(xid: xid)
      }
    }

    case X11_EV_WINDOW_RESIZE:
      let wPx = ev.u.win_resize.width_px
      let hPx = ev.u.win_resize.height_px
      Task { @MainActor in
        WindowRegistry.shared.applyX11Resize(xid: ev.xid, wPx: wPx, hPx: hPx)
      }
      
    default:
      break
    }
  }
  
  
  private func format(_ ev: x11_event_t, showMotion: Bool) -> String? {
      let xid = String(format: "0x%X", ev.xid)

      switch ev.type {
      case X11_EV_WINDOW_CREATE:
          return "EV_WINDOW_CREATE xid=\(xid) \(ev.u.win_create.width_px)x\(ev.u.win_create.height_px)"
          
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
        
      default:
          return "EV type=\(ev.type.rawValue) xid=\(xid) size=\(ev.size)"
      }
  }

  deinit {
    NotificationCenter.default.removeObserver(self)
  }
}
