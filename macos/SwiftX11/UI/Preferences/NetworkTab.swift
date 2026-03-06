import SwiftUI

struct NetworkTab: View {
    @EnvironmentObject var settings: SettingsStore

    private var displayNum: Int { max(settings.displayNumber, 1) }

    var body: some View {
        Form {
            Section {
                Toggle("Enable TCP Listener (port \(6000 + displayNum))",
                       isOn: $settings.enableTCP)

                Toggle("Enable Unix Socket (/tmp/.X11-unix/X\(displayNum))",
                       isOn: $settings.enableUnixSocket)
            }

            Section {
                Text("Docker usage:")
                    .font(.headline)

                VStack(alignment: .leading, spacing: 4) {
                    Text("TCP (via host.docker.internal):")
                        .font(.subheadline).foregroundColor(.secondary)
                    Text("docker run -e DISPLAY=host.docker.internal:\(displayNum) ...")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text("Unix socket (volume mount):")
                        .font(.subheadline).foregroundColor(.secondary)
                    Text("docker run -v /tmp/.X11-unix:/tmp/.X11-unix -e DISPLAY=:\(displayNum) ...")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }

                Text("Changes take effect on next server restart.")
                    .font(.caption).foregroundColor(.secondary)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
