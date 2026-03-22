import Foundation
import Combine
import X11LowLevel

final class SettingsStore: ObservableObject {
  @Published var displayNumber: Int {
      didSet { UserDefaults.standard.set(displayNumber, forKey: "displayNumber") }
  }

  @Published var antialiasedFonts: Bool {
      didSet {
          UserDefaults.standard.set(antialiasedFonts, forKey: "antialiasedFonts")
          x11_set_font_antialiased(antialiasedFonts ? 1 : 0)
      }
  }

  init() {
    self.displayNumber = UserDefaults.standard.object(forKey: "displayNumber") as? Int ?? 0
    self.antialiasedFonts = UserDefaults.standard.object(forKey: "antialiasedFonts") as? Bool ?? true
    self.enableTCP = UserDefaults.standard.object(forKey: "enableTCP") as? Bool ?? true
    self.enableUnixSocket = UserDefaults.standard.object(forKey: "enableUnixSocket") as? Bool ?? true
    self.tcpBindAddress = UserDefaults.standard.object(forKey: "tcpBindAddress") as? String ?? "0.0.0.0"
    self.logVerbosity = UserDefaults.standard.object(forKey: "logVerbosity") as? Int ?? 0
    // Sync initial state to C++
    x11_set_font_antialiased(self.antialiasedFonts ? 1 : 0)
    x11_set_log_verbosity(Int32(self.logVerbosity))
  }

  @Published var enableClipboard: Bool = true
  @Published var enableTCP: Bool {
    didSet { UserDefaults.standard.set(enableTCP, forKey: "enableTCP") }
  }
  @Published var enableUnixSocket: Bool {
    didSet { UserDefaults.standard.set(enableUnixSocket, forKey: "enableUnixSocket") }
  }
  @Published var tcpBindAddress: String {
    didSet { UserDefaults.standard.set(tcpBindAddress, forKey: "tcpBindAddress") }
  }
  
  @Published var logVerbosity: Int {
    didSet {
      UserDefaults.standard.set(logVerbosity, forKey: "logVerbosity")
      x11_set_log_verbosity(Int32(logVerbosity))
    }
  }

  @Published var wireTrace: Bool = false {
    didSet {
      x11_set_wire_trace(wireTrace ? 1 : 0)
    }
  }
}
