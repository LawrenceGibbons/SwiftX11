import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var statusItemController: StatusItemController?

  func applicationDidFinishLaunching(_ notification: Notification) {
    statusItemController = StatusItemController()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      StatusItemController.shared.install()
    }
  }
}
