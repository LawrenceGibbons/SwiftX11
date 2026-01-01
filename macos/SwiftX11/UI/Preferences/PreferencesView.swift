import SwiftUI

struct PreferencesView: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        TabView {
            GeneralTab().tabItem { Text("General") }
            RenderingTab().tabItem { Text("Rendering") }
            ClipboardTab().tabItem { Text("Clipboard") }
            NetworkTab().tabItem { Text("Network") }
        }
        .padding()
    }
}
