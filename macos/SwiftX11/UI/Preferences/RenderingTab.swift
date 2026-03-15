import SwiftUI

struct RenderingTab: View {
    @EnvironmentObject var settings: SettingsStore

    var body: some View {
        Form {
            Toggle("Antialiased Fonts", isOn: $settings.antialiasedFonts)
                .help("Use CoreText subpixel rendering for smoother font edges. Disable for classic bitmap-style fonts.")
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
