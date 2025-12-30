import Foundation

final class SettingsStore: ObservableObject {
    @Published var useMetal: Bool = true
    @Published var enableClipboard: Bool = true
    @Published var enableTCP: Bool = false
}
