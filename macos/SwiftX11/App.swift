import SwiftUI

@main
struct SwiftX11: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var server = XServerController()
    @StateObject private var settings = SettingsStore()
    @Environment(\.openWindow) private var openWindow

    private var logWindowVisible: Bool {
        NSApp.windows.first(where: { $0.title == "SwiftX11 Log" })?.isVisible ?? false
    }

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
                Button(logWindowVisible ? "Hide Log Window" : "Show Log Window") {
                    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" }),
                       win.isVisible {
                        win.orderOut(nil)
                    } else {
                        NSApp.activate(ignoringOtherApps: true)
                        openWindow(id: "log-window")
                    }
                }
                .keyboardShortcut("0", modifiers: .command)
            }
        }
    }
}
