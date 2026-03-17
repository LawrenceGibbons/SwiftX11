import SwiftUI

struct HelpView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                // Header
                VStack(alignment: .leading, spacing: 4) {
                    Text("SwiftX11 User Guide")
                        .font(.largeTitle)
                        .fontWeight(.bold)
                    Text("Version \(XServerController.buildVersion)")
                        .font(.subheadline)
                        .foregroundColor(.secondary)
                }

                Divider()

                helpSection("Display Setup") {
                    Text("SwiftX11 listens on display :1 (TCP port 6001) to avoid conflict with XQuartz on :0.")
                    codeBlock("# TCP (default — works with Docker and remote clients)\nexport DISPLAY=127.0.0.1:1\n\n# Unix socket (local clients only)\nexport DISPLAY=:1")
                    Text("Add the export to ~/.profile or ~/.zshrc so it persists across terminal sessions.")
                }

                helpSection("Docker / Container Workflow") {
                    Text("Docker Desktop for Mac runs containers in a Linux VM. Unix socket mounts don't work — use TCP.")
                    codeBlock("# Inside a Docker container, use:\nexport DISPLAY=host.docker.internal:1")
                    Text("Enable TCP in Settings \u{2192} Network. The server binds to 0.0.0.0 by default so containers can connect.")
                    Text("Unix sockets work for native macOS X11 clients but not Docker containers on macOS.")
                }

                helpSection("Fonts") {
                    Text("SwiftX11 loads fonts from three sources:")
                    bulletList([
                        "PCF bitmap fonts from /opt/X11/share/fonts/{misc,75dpi,100dpi}/",
                        "CoreText system fonts: fixed\u{2192}Menlo, courier\u{2192}Courier, helvetica\u{2192}Helvetica, times\u{2192}Times New Roman, lucida\u{2192}Lucida Grande",
                        "Built-in \"fixed\" fallback font for unresolved requests"
                    ])
                    Text("Toggle antialiased (CoreText) fonts in Settings \u{2192} Rendering \u{2192} \"Antialiased Fonts\".")
                    Text("Install XQuartz or the X11 font packages from Homebrew to get PCF fonts.")
                }

                helpSection("Settings") {
                    Text("Open Settings via \u{2318}, from the menu bar or the SwiftX11 app menu.")
                    bulletList([
                        "Rendering: Antialiased font toggle",
                        "Network: TCP and Unix socket toggles, bind address",
                        "Clipboard: Clipboard sync options",
                        "General: Display number, log verbosity"
                    ])
                }

                helpSection("Log Window") {
                    Text("View \u{2192} Show Log Window (\u{2318}0) toggles the log window.")
                    Text("The log shows X11 protocol activity, connection events, and diagnostics.")
                    Text("Increase verbosity in Settings \u{2192} General for more detail.")
                }

                helpSection("Keyboard & Mouse") {
                    bulletList([
                        "Option + Click = Middle mouse button (button 2) \u{2014} use for scrollbar thumb drag",
                        "Ctrl + Click = Right-click (button 3)",
                        "\u{2318}W = Close window / kill X11 client (sends WM_DELETE_WINDOW or disconnects)",
                        "\u{2318}Q = Quit SwiftX11 (gracefully stops server)"
                    ])
                    Text("X11 modifier mapping: Option \u{2192} Mod1 (Alt), Command \u{2192} Mod4 (Super), Control \u{2192} Control.")
                }

                helpSection("Copy & Paste") {
                    Text("X11 has two clipboard mechanisms, both bridged to the macOS clipboard:")
                    Text("PRIMARY selection (traditional X11)").fontWeight(.semibold)
                    bulletList([
                        "Select text with the mouse \u{2014} it is automatically copied (no \u{2318}C needed)",
                        "Middle-click (Option + Click) to paste at the cursor position",
                        "This is the classic Unix/X11 workflow and works in xterm, vim, emacs, etc."
                    ])
                    Text("CLIPBOARD selection (modern apps)").fontWeight(.semibold)
                    bulletList([
                        "Ctrl+Shift+C to copy in xterm (or use the Edit menu)",
                        "Ctrl+Shift+V to paste in xterm",
                        "Java/GTK apps typically use Ctrl+C / Ctrl+V as on other platforms"
                    ])
                    Text("SwiftX11 syncs both directions: text copied in X11 apps appears in \u{2318}V on macOS, and text copied with \u{2318}C on macOS can be pasted into X11 apps.")
                }

                helpSection("Supported Extensions") {
                    bulletList([
                        "BIG-REQUESTS \u{2014} Large request support (up to 4 MB)",
                        "RENDER \u{2014} Anti-aliased text, compositing, gradients",
                        "XFIXES \u{2014} Cursor and selection fixes",
                        "RANDR \u{2014} Multi-monitor layout (dynamic, real display data)",
                        "XINERAMA \u{2014} Multi-monitor screen queries",
                        "SHAPE \u{2014} Non-rectangular windows (e.g., xeyes)",
                        "GE \u{2014} Generic Events",
                        "XC-MISC \u{2014} XID range recycling for long-running sessions",
                        "XInput2 \u{2014} Input device enumeration for GTK3/4",
                        "XTEST \u{2014} Synthetic input events for accessibility and automation"
                    ])
                }

                helpSection("Known Limitations") {
                    bulletList([
                        "No GLX \u{2014} OpenGL rendering over X11 is not supported",
                        "No XKB compose sequences \u{2014} dead keys and multi-key compose not supported",
                        "Little-endian only \u{2014} big-endian X11 clients are rejected at connection time",
                        "Requires Metal GPU \u{2014} software rendering is not supported",
                        "US keyboard layout assumed \u{2014} non-US layouts may have incorrect keysym mapping"
                    ])
                }

                helpSection("Testing") {
                    codeBlock("# Basic test\nDISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc\n\n# Eye-tracking demo\nDISPLAY=127.0.0.1:1 xeyes\n\n# Calculator\nDISPLAY=127.0.0.1:1 xcalc")
                }

                Spacer(minLength: 20)
            }
            .padding(30)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(minWidth: 560, idealWidth: 640, minHeight: 400, idealHeight: 700)
    }

    // MARK: - Helpers

    @ViewBuilder
    private func helpSection(_ title: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.title2)
                .fontWeight(.semibold)
            content()
        }
    }

    private func codeBlock(_ code: String) -> some View {
        Text(code)
            .font(.system(.body, design: .monospaced))
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(6)
            .textSelection(.enabled)
    }

    private func bulletList(_ items: [String]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(items, id: \.self) { item in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text("\u{2022}")
                    Text(item)
                }
            }
        }
    }
}
