import Foundation
import Combine

final class SettingsStore: ObservableObject {
  @Published var displayNumber: Int {
      didSet { UserDefaults.standard.set(displayNumber, forKey: "displayNumber") }
  }

  @Published var useMetal: Bool {
      didSet { UserDefaults.standard.set(useMetal, forKey: "useMetal") }
  }

  init() {
      self.displayNumber = UserDefaults.standard.object(forKey: "displayNumber") as? Int ?? 0
      self.useMetal = UserDefaults.standard.object(forKey: "useMetal") as? Bool ?? true
  }

  @Published var enableClipboard: Bool = true
  @Published var enableTCP: Bool = false
  
  @Published var pauseLogAppend: Bool = false     // freeze the log history
  @Published var showMotionLogs: Bool = false     // suppress the motion logging 
  @Published var pauseDrain:     Bool = false     // stop draining queue
  @Published var showQueueStats: Bool = false
}
