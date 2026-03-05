import Foundation
import Combine
import X11LowLevel

final class SettingsStore: ObservableObject {
  @Published var displayNumber: Int {
      didSet { UserDefaults.standard.set(displayNumber, forKey: "displayNumber") }
  }

  @Published var useMetal: Bool {
      didSet { UserDefaults.standard.set(useMetal, forKey: "useMetal") }
  }

  @Published var antialiasedFonts: Bool {
      didSet {
          UserDefaults.standard.set(antialiasedFonts, forKey: "antialiasedFonts")
          x11_set_font_antialiased(antialiasedFonts ? 1 : 0)
      }
  }

  init() {
    self.displayNumber = UserDefaults.standard.object(forKey: "displayNumber") as? Int ?? 0
    //self.useMetal = UserDefaults.standard.object(forKey: "useMetal") as? Bool ?? false
    self.useMetal = UserDefaults.standard.object(forKey: "useMetal") as? Bool ?? true
    self.antialiasedFonts = UserDefaults.standard.object(forKey: "antialiasedFonts") as? Bool ?? true
    // Sync initial AA state to C++
    x11_set_font_antialiased(self.antialiasedFonts ? 1 : 0)
  }

  @Published var enableClipboard: Bool = true
  @Published var enableTCP:       Bool = false
  
  @Published var pauseLogAppend: Bool = false     // freeze the log history
  @Published var showMotionLogs: Bool = false     // suppress the motion logging 
  @Published var showDamageLogs: Bool = true     // show/suppress the damage logging
  @Published var pauseDrain:     Bool = false     // stop draining queue
  @Published var showQueueStats: Bool = false
}
