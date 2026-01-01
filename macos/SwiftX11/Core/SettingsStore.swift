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
}
