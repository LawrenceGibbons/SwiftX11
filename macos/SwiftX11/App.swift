import SwiftUI

@main
struct SwiftX11: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var server = XServerController()
    @StateObject private var settings = SettingsStore()
    @Environment(\.openWindow) private var openWindow

    var body: some Scene {
        Window("SwiftX11 Log", id: "log-window") {
            ContentView()
                .environmentObject(server)
                .environmentObject(settings)
        }
        Settings {
            PreferencesView()
                .environmentObject(server)
                .environmentObject(settings)
        }
        .commands {
            CommandGroup(replacing: .appInfo) { }
            CommandMenu("View") {
                Button("Show Log Window") {
                    NSApp.activate(ignoringOtherApps: true)
                    openWindow(id: "log-window")
                }
                .keyboardShortcut("0", modifiers: .command)
            }
        }
    }
}
