import SwiftUI

struct RenderingTab: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        Form {
            Toggle("Use Metal Rendering", isOn: $settings.useMetal)
            Toggle("Antialiased Fonts", isOn: $settings.antialiasedFonts)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
