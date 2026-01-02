import Foundation
import Combine
import X11LowLevel

final class XServerController: ObservableObject {
  @Published var isRunning: Bool = false
  @Published var display: Int = 0
  @Published var logLines: [String] = []
  
  private var drainTimer: DispatchSourceTimer?
  private let queue = DispatchQueue(
    label: "swiftx11.server",
    qos: .userInitiated
  )
  
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
      forName: .x11StartRequested,
      object: nil,
      queue: .main
    ) { [weak self] _ in
      self?.start()
    }
    
    NotificationCenter.default.addObserver(
      forName: .x11StopRequested,
      object: nil,
      queue: .main
    ) { [weak self] _ in
      self?.stop()
    }
  }
  
  func start() {
    guard !isRunning else { return }
    append("Starting X11 server on :\(display)…")
    
    queue.async { [weak self] in
      guard let self else { return }
      let ok = x11_start_server(Int32(self.display))
      DispatchQueue.main.async {
        self.isRunning = ok
        self.append(ok ? "Server started" : "Failed to start server")
        if ok { self.startDrainTimer() }
      }
    }
  }
  
  func stop() {
    guard isRunning else { return }
    append("Stopping X11 server…")
    
    queue.async { [weak self] in
        guard let self else { return }

        x11_stop_server()  // enqueues EV_WINDOW_DESTROY (and others)

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }

            // Final drain so destroy events show up in the log UI
            self.drainEvents(max: 4096)

            self.isRunning = false
            self.append("Server stopped")
        }
    }
  }
  
  private func append(_ line: String) {
    logLines.append("[\(Date())] \(line)")
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
      var n = 0
      while n < max {
          var ev = x11_event_t()
          guard x11_debug_pop_event(&ev) else { break }

          if let line = format(ev) {
              append(line)
          }
          n += 1
      }
  }
  
  private func format(_ ev: x11_event_t) -> String? {
      let xid = String(format: "0x%X", ev.xid)

      switch ev.type {
      case X11_EV_WINDOW_CREATE:
          return "EV_WINDOW_CREATE xid=\(xid) \(ev.u.win_create.width_px)x\(ev.u.win_create.height_px)"

      case X11_EV_WINDOW_DESTROY:
          return "EV_WINDOW_DESTROY xid=\(xid)"

      case X11_EV_POINTER_ENTER:
          return "EV_ENTER xid=\(xid) (\(ev.u.crossing.x_px),\(ev.u.crossing.y_px))"

      case X11_EV_POINTER_LEAVE:
          return "EV_LEAVE xid=\(xid) (\(ev.u.crossing.x_px),\(ev.u.crossing.y_px))"

      case X11_EV_POINTER_MOTION:
          #if MOTION_LOGS
          let b = ev.u.motion.buttons
          return String(
              format: "EV_MOTION xid=%@ (%d,%d) buttons=0x%X",
              xid,
              ev.u.motion.x_px,
              ev.u.motion.y_px,
              b
          )
          #else
          return nil
          #endif

      case X11_EV_POINTER_BUTTON:
          return "EV_BUTTON xid=\(xid) btn=\(ev.u.button.button) press=\(ev.u.button.is_press) buttons=\(ev.u.button.buttons)"

      case X11_EV_SCROLL:
          return "EV_SCROLL xid=\(xid) axis=\(ev.u.scroll.axis) ticks=\(ev.u.scroll.ticks)"

      case X11_EV_KEY:
          return "EV_KEY xid=\(xid) code=\(ev.u.key.keycode) press=\(ev.u.key.is_press)"

      case X11_EV_FOCUS:
          return "EV_FOCUS xid=\(xid) focused=\(ev.u.focus.focused)"

      case X11_EV_WINDOW_RAISE:
          return "EV_WINDOW_RAISE xid=\(xid)"
        
      default:
          return "EV type=\(ev.type.rawValue) xid=\(xid) size=\(ev.size)"
      }
  }
}
