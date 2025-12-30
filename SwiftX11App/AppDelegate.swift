import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItemController: StatusItemController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItemController = StatusItemController()
        statusItemController?.install()
    }
}
