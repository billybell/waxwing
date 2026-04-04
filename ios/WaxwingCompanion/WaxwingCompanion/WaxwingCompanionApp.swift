import SwiftUI

@main
struct WaxwingCompanionApp: App {
    @StateObject private var bleManager = BLEManager()

    var body: some Scene {
        WindowGroup {
            ScannerView()
                .environmentObject(bleManager)
        }
    }
}
