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
    // Both default ON since v1.20.0.22 (XI2 Phases A–B and XKEYBOARD Phase F
    // verified 2026-09-07 against xterm, Vivado, Vitis and portal-GTK).
    self.xi2Advertised = UserDefaults.standard.object(forKey: "xi2Advertised") as? Bool ?? true
    self.xkbAdvertised = UserDefaults.standard.object(forKey: "xkbAdvertised") as? Bool ?? true
    // Sync initial state to C++ (didSet does NOT fire during init, so apply
    // the persisted values explicitly here).
    x11_set_font_antialiased(self.antialiasedFonts ? 1 : 0)
    x11_set_log_verbosity(Int32(self.logVerbosity))
    x11_set_xi2_advertised(self.xi2Advertised ? 1 : 0)
    x11_set_xkb_advertised(self.xkbAdvertised ? 1 : 0)
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

  // Advertise XInputExtension (XI2). Default ON since v1.20.0.22; OFF is the
  // escape hatch that keeps Electron/GTK clients on the core input path.
  @Published var xi2Advertised: Bool {
    didSet {
      UserDefaults.standard.set(xi2Advertised, forKey: "xi2Advertised")
      x11_set_xi2_advertised(xi2Advertised ? 1 : 0)
    }
  }

  // Advertise XKEYBOARD. Default ON since v1.20.0.22 (verified against libX11,
  // GDK3, Java AWT and Chromium); XI2 needs it ON for GTK3 clients (GDK's
  // non-XKB keymap path crashes on the first XI2 key event, M23).
  @Published var xkbAdvertised: Bool {
    didSet {
      UserDefaults.standard.set(xkbAdvertised, forKey: "xkbAdvertised")
      x11_set_xkb_advertised(xkbAdvertised ? 1 : 0)
    }
  }
}
