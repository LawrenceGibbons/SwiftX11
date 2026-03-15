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
  
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("Log verbosity:")
        Picker("", selection: $settings.logVerbosity) {
          Text("Errors only").tag(0)
          Text("Info (stubs & unhandled)").tag(1)
          Text("Verbose").tag(2)
        }
        .pickerStyle(.segmented)
        .frame(maxWidth: 320)
      }

      Divider()

      Text("Logs").font(.headline)

      HStack {
        Button("Copy All") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(server.logText, forType: .string)
        }
        Button("Clear") {
          server.logText = ""
        }
        Spacer()
      }

      LogTextView(text: $server.logText)
        .frame(minHeight: 200)
    }
    .padding(16)
    .frame(minWidth: 560, minHeight: 360)
    
    .onAppear {
      guard !server.didInstallLogControls else { return }
      server.didInstallLogControls = true

      WindowRegistry.shared.attachLogHooks(
          logAppend: { line in server.append(line) },
          isLogPaused: { false }
      )
    }
  }
}
