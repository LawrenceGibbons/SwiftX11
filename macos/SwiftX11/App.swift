import SwiftUI

@main
struct SwiftX11: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var server = XServerController()
    @StateObject private var settings = SettingsStore()
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
        }
    }
}
