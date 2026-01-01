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
      
      Divider()
      
      Text("Logs").font(.headline)
      ScrollView {
        LazyVStack(alignment: .leading) {
          ForEach(Array(server.logLines.enumerated()), id: \.offset) { item in
            Text(item.element)
              .font(.system(.caption, design: .monospaced))
              .frame(maxWidth: .infinity, alignment: .leading)
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
    .onChange(of: settings.useMetal) { newValue in
      Task { @MainActor in
        WindowRegistry.shared.setUseMetalForAllWindows(newValue)
      }
    }
  }
}
