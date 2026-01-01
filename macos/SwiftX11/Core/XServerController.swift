import Foundation
import Combine
import X11LowLevel

final class XServerController: ObservableObject {
  @Published var isRunning: Bool = false
  @Published var display: Int = 0
  @Published var logLines: [String] = []
  
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
      }
    }
  }
  
  func stop() {
    guard isRunning else { return }
    append("Stopping X11 server…")
    
    queue.async {
      x11_stop_server()
      DispatchQueue.main.async { [weak self] in
        self?.isRunning = false
        self?.append("Server stopped")
      }
    }
  }
  
  private func append(_ line: String) {
    logLines.append("[\(Date())] \(line)")
  }
}
