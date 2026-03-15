import SwiftUI

struct NetworkTab: View {
    @EnvironmentObject var settings: SettingsStore

    private var displayNum: Int { max(settings.displayNumber, 1) }

    var body: some View {
        Form {
            Section {
                Toggle("Enable TCP Listener (port \(6000 + displayNum))",
                       isOn: $settings.enableTCP)
                    .help("Listen for X11 connections on TCP port \(6000 + displayNum). Required for Docker containers.")

                Toggle("Enable Unix Socket (/tmp/.X11-unix/X\(displayNum))",
                       isOn: $settings.enableUnixSocket)
                    .help("Listen on /tmp/.X11-unix/X\(displayNum). Works for native macOS X11 clients only (not Docker).")
            }

            Section {
                Text("Docker on macOS (recommended):")
                    .font(.headline)

                VStack(alignment: .leading, spacing: 4) {
                    Text("Use TCP — Docker Desktop runs in a VM, so Unix socket volume mounts do not work.")
                        .font(.caption).foregroundColor(.secondary)
                    Text("docker run -e DISPLAY=host.docker.internal:\(displayNum) ...")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }
            }

            Section {
                Text("Native X11 clients:")
                    .font(.headline)

                VStack(alignment: .leading, spacing: 4) {
                    Text("TCP:")
                        .font(.subheadline).foregroundColor(.secondary)
                    Text("DISPLAY=127.0.0.1:\(displayNum) xterm")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text("Unix socket:")
                        .font(.subheadline).foregroundColor(.secondary)
                    Text("DISPLAY=:\(displayNum) xterm")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }
            }

            Text("Changes take effect on next server restart.")
                .font(.caption).foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
