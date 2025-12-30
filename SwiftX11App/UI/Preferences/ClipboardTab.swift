import SwiftUI

struct ClipboardTab: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        Form {
            Toggle("Enable Clipboard Bridge", isOn: $settings.enableClipboard)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
