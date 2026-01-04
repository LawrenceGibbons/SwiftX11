import SwiftUI
import X11LowLevel

struct ContentView: View {
  @EnvironmentObject var server: XServerController
  @EnvironmentObject var settings: SettingsStore
  
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
      Toggle("Repaint storm (stress test)", isOn: $settings.repaintStorm)
      
      Button("Dump Queue") {
          server.dumpEventQueue(maxItems: 64)
      }
      
      let XID_A: UInt32 = 0x10001
      Button("Test destroy-waits (A)") {
        // Ensure we’re running on the main actor (matches how you do start/stop).
        Task { @MainActor in
          // 1) Keep repaints happening so we’ll definitely be in-flight soon.
          x11_debug_set_repaint_storm(1, XID_A)

          // 2) One-shot: during the next repaint of A, call destroy from inside repaint.
          x11_debug_destroy_during_next_repaint(1, XID_A)
        }
      }
      
      Button("Open Inspector") {
          X11DebugInspectorWindowController.shared.show()
      }
      
      Button("Snapshot routing") {
        // Dump a routing + window-table snapshot to stderr (debug builds)
        x11_debug_dump_routing_snapshot("manual")
      }
      
      Divider()
      
      Text("Logs").font(.headline)
      ScrollViewReader { proxy in
          ScrollView {
              LazyVStack(alignment: .leading) {
                  ForEach(Array(server.logLines.enumerated()), id: \.offset) { idx, line in
                      Text(line)
                          .font(.system(.caption, design: .monospaced))
                          .frame(maxWidth: .infinity, alignment: .leading)
                          .id(idx)
                  }
              }
          }
          .onAppear {
              if let last = server.logLines.indices.last {
                  proxy.scrollTo(last, anchor: .bottom)
              }
          }
          .onChange(of: server.logLines.count) { _, _ in
              if let last = server.logLines.indices.last {
                  proxy.scrollTo(last, anchor: .bottom)
              }
          }
      }
      Toggle("Use Metal rendering", isOn: $settings.useMetal)
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
      }

      
    }
    .onChange(of: settings.useMetal) { _, newValue in
      Task { @MainActor in
        WindowRegistry.shared.setUseMetalForAllWindows(newValue)
      }
    }
    .onChange(of: settings.repaintStorm) { _, enabled in
      // 0 means “all windows” for this debug path
      x11_debug_set_repaint_storm(enabled ? 1 : 0, 0)
    }
  }
}
