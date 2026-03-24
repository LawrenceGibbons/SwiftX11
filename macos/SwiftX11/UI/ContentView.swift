import SwiftUI
import AppKit
import Combine
import X11LowLevel

struct LogTextView: NSViewRepresentable {
  @Binding var text: String

  func makeNSView(context: Context) -> NSScrollView {
    let tv = NSTextView()
    tv.isEditable = false
    tv.isSelectable = true
    tv.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
    tv.textContainerInset = NSSize(width: 6, height: 6)
    tv.usesFindBar = true
    tv.isIncrementalSearchingEnabled = true

    let sv = NSScrollView()
    sv.hasVerticalScroller = true
    sv.documentView = tv
    sv.isFindBarVisible = false
    return sv
  }

  func updateNSView(_ nsView: NSScrollView, context: Context) {
    guard let tv = nsView.documentView as? NSTextView else { return }
    if tv.string != text {
      tv.string = text
      tv.scrollToEndOfDocument(nil) // optional auto-scroll
    }
  }

  /// Show the find bar by making the text view first responder and invoking Find.
  static func showFindBar(in scrollView: NSScrollView) {
    guard let tv = scrollView.documentView as? NSTextView else { return }
    tv.window?.makeFirstResponder(tv)
    // NSTextView.performFindPanelAction needs a sender with tag == NSFindPanelAction rawValue.
    // Tag 1 = showFindPanel.  Using an NSMenuItem as the sender.
    let item = NSMenuItem()
    item.tag = 1  // NSTextFinder.Action.showFindInterface.rawValue
    tv.performFindPanelAction(item)
  }
}

/// Helper to find the LogTextView's NSScrollView from anywhere in the view hierarchy.
class LogScrollViewHolder: ObservableObject {
  @Published var scrollView: NSScrollView?
}

/// NSViewRepresentable that captures its parent NSScrollView reference.
struct LogTextViewWithFind: NSViewRepresentable {
  @Binding var text: String
  var holder: LogScrollViewHolder

  func makeNSView(context: Context) -> NSScrollView {
    let tv = NSTextView()
    tv.isEditable = false
    tv.isSelectable = true
    tv.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
    tv.textContainerInset = NSSize(width: 6, height: 6)
    tv.usesFindBar = true
    tv.isIncrementalSearchingEnabled = true

    let sv = NSScrollView()
    sv.hasVerticalScroller = true
    sv.documentView = tv
    sv.isFindBarVisible = false
    holder.scrollView = sv
    return sv
  }

  func updateNSView(_ nsView: NSScrollView, context: Context) {
    guard let tv = nsView.documentView as? NSTextView else { return }
    if tv.string != text {
      tv.string = text
      tv.scrollToEndOfDocument(nil)
    }
  }
}

struct ContentView: View {
  @EnvironmentObject var server: XServerController
  @EnvironmentObject var settings: SettingsStore
  @StateObject private var logHolder = LogScrollViewHolder()

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
        Button("Find…") {
          if let sv = logHolder.scrollView {
            LogTextView.showFindBar(in: sv)
          }
        }
        .keyboardShortcut("f", modifiers: .command)
        Spacer()
        Toggle("Wire Trace (stderr)", isOn: $settings.wireTrace)
          .toggleStyle(.switch)
          .help("Log every incoming request and outgoing packet to the Xcode console")
      }

      LogTextViewWithFind(text: $server.logText, holder: logHolder)
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
