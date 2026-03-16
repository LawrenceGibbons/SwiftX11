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
            CommandGroup(replacing: .appInfo) {
                Button("About SwiftX11") {
                    let version = XServerController.buildVersion
                    let buildDate = XServerController.buildDate
                    let credits = NSAttributedString(
                        string: "An X11 display server for macOS.\n\nDeveloped by Lawrence Gibbons and Claude.",
                        attributes: [
                            .font: NSFont.systemFont(ofSize: 11),
                            .foregroundColor: NSColor.labelColor
                        ]
                    )
                    NSApp.orderFrontStandardAboutPanel(options: [
                        .applicationName: "SwiftX11",
                        .applicationVersion: version,
                        .version: "Built \(buildDate)",
                        .credits: credits
                    ])
                }
            }
            CommandGroup(after: .sidebar) {
                Divider()
                Button("Toggle Log Window") {
                    if let win = NSApp.windows.first(where: { $0.title == "SwiftX11 Log" }),
                       win.isVisible {
                        win.orderOut(nil)
                    } else {
                        NSApp.activate(ignoringOtherApps: true)
                        NSApp.windows.first(where: { $0.title == "SwiftX11 Log" })?
                            .makeKeyAndOrderFront(nil)
                    }
                }
                .keyboardShortcut("0", modifiers: .command)
            }
            CommandGroup(replacing: .help) { }
        }
    }
}
