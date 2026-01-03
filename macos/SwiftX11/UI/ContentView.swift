import SwiftUI

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
      
      Button("Dump Queue") {
          server.dumpEventQueue(maxItems: 64)
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
      Task { @MainActor in
        WindowRegistry.shared.useMetalForNewWindows = settings.useMetal
      }
    }
    .onChange(of: settings.useMetal) { _, newValue in
      Task { @MainActor in
        WindowRegistry.shared.setUseMetalForAllWindows(newValue)
      }
    }
  }
}
