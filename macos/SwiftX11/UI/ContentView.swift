import SwiftUI
import AppKit
import X11LowLevel

struct LogTextView: NSViewRepresentable {
  @Binding var text: String

  func makeNSView(context: Context) -> NSScrollView {
    let tv = NSTextView()
    tv.isEditable = false
    tv.isSelectable = true
    tv.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
    tv.textContainerInset = NSSize(width: 6, height: 6)

    let sv = NSScrollView()
    sv.hasVerticalScroller = true
    sv.documentView = tv
    return sv
  }

  func updateNSView(_ nsView: NSScrollView, context: Context) {
    guard let tv = nsView.documentView as? NSTextView else { return }
    if tv.string != text {
      tv.string = text
      tv.scrollToEndOfDocument(nil) // optional auto-scroll
    }
  }
}

struct ContentView: View {
  @EnvironmentObject var server: XServerController
  @EnvironmentObject var settings: SettingsStore
  @State private var lastXid: UInt32?
  
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Toggle(isOn: Binding(
          get: { server.isRunning },
          set: { $0 ? server.start() : server.stop() }
        )) {
          Text(server.isRunning ? "Server Running" : "Server Stopped")
        }
        Spacer()
        Stepper("Display :\(server.display)", value: $server.display, in: 0...63)
      }
      .padding(.bottom, 8)
      
      
      Toggle("Freeze log output", isOn: $settings.pauseLogAppend)

      Toggle("Show queue stats (1/sec)", isOn: $settings.showQueueStats)
      
      Toggle("Pause draining (danger: queue can fill)", isOn: $settings.pauseDrain)
          .toggleStyle(.switch)
      
      Toggle("Show Motion events", isOn: $settings.showMotionLogs)
      
      
      Toggle("Use Metal rendering", isOn: $settings.useMetal)
      
      Toggle("Show Damage events", isOn: $settings.showDamageLogs)

      Divider()

      Text("Logs").font(.headline)

      HStack {
        Button("Copy All") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(server.logText, forType: .string)
        }
        Spacer()
      }

      LogTextView(text: $server.logText)
        .frame(minHeight: 200)
    }
    .padding(16)
    .frame(minWidth: 560, minHeight: 360)
    
    // preferences hooks
    .onAppear {
      guard !server.didInstallLogControls else { return }
      server.didInstallLogControls = true

      server.setLogControls(
        isPaused: { settings.pauseLogAppend },
        showMotion: { settings.showMotionLogs },
        showStats: { settings.showQueueStats },
        drainPaused: { settings.pauseDrain }
      )

      WindowRegistry.shared.attachLogHooks(
          logAppend: { line in server.append(line) },
          isLogPaused: { settings.pauseLogAppend },
          showQueueStats: { settings.showQueueStats }
      )

      Task { @MainActor in
        WindowRegistry.shared.useMetalForNewWindows = settings.useMetal
        WindowRegistry.shared.setUseMetalForAllWindows(settings.useMetal)
      }

      
    }
    .onChange(of: settings.useMetal) { _, newValue in
      Task { @MainActor in
        WindowRegistry.shared.setUseMetalForAllWindows(newValue)
      }
    }
  }
}
