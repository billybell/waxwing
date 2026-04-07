import SwiftUI

@main
struct WaxwingCompanionApp: App {
    @StateObject private var bleManager = BLEManager()
    @ObservedObject private var profile  = UserProfile.shared
    @ObservedObject private var identity = ContentIdentity.shared

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(bleManager)
        }
    }
}

/// Gates the main app behind the mandatory onboarding flow. Once the user
/// has completed onboarding AND has a Content Identity provisioned, the
/// regular ScannerView is shown.
private struct RootView: View {
    @ObservedObject private var profile  = UserProfile.shared
    @ObservedObject private var identity = ContentIdentity.shared

    var body: some View {
        if profile.hasCompletedOnboarding && identity.hasIdentity {
            ScannerView()
        } else {
            OnboardingView()
        }
    }
}
