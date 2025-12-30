import SwiftUI

struct RenderingTab: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        Form {
            Toggle("Use Metal Rendering", isOn: $settings.useMetal)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
