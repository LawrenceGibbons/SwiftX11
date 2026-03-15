import SwiftUI

struct ClipboardTab: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        Form {
            Toggle("Enable Clipboard Bridge", isOn: $settings.enableClipboard)
                .help("Sync the macOS pasteboard with X11 selections. Copy/paste between X11 and macOS apps.")
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
