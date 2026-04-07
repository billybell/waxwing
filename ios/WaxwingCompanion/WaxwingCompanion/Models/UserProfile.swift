import Foundation
import Combine

// MARK: - User Profile
//
// A simple user identity that's distinct from the Waxwing device identity.
// Stored in UserDefaults. The user can opt in to attaching their display
// name when uploading images so recipients know who shared what.
//
// This object also tracks app-level preferences (default geo-tagging,
// default node, onboarding completion).

class UserProfile: ObservableObject {
    static let shared = UserProfile()

    // MARK: Identity

    /// Display name shown to other nodes when the user opts in. Optional —
    /// users can stay anonymous and the public key alone serves as a pseudonym.
    @Published var displayName: String {
        didSet { UserDefaults.standard.set(displayName, forKey: Self.nameKey) }
    }

    // MARK: Default toggles for upload metadata

    /// Whether the "include my identity" toggle defaults to on.
    @Published var includeIdentityByDefault: Bool {
        didSet { UserDefaults.standard.set(includeIdentityByDefault, forKey: Self.identityKey) }
    }

    /// Whether the "include my location" toggle defaults to on.
    @Published var includeLocationByDefault: Bool {
        didSet { UserDefaults.standard.set(includeLocationByDefault, forKey: Self.locationKey) }
    }

    // MARK: Default node

    /// Persistent identifier (CBPeripheral.identifier.uuidString) of the
    /// user's preferred node. The scanner can auto-connect to this node when
    /// it's seen, and the upload UI can default to it.
    @Published var defaultNodeIdentifier: String? {
        didSet { UserDefaults.standard.set(defaultNodeIdentifier, forKey: Self.defaultNodeKey) }
    }

    // MARK: Onboarding state

    /// Set to true once the user has either generated or restored a Content
    /// Identity AND completed the welcome flow. The app gates its main UI
    /// behind this so first-launch users always see onboarding.
    @Published var hasCompletedOnboarding: Bool {
        didSet { UserDefaults.standard.set(hasCompletedOnboarding, forKey: Self.onboardingKey) }
    }

    // MARK: Storage keys

    private static let nameKey         = "waxwing_user_displayName"
    private static let identityKey     = "waxwing_user_includeIdentity"
    private static let locationKey     = "waxwing_user_includeLocation"
    private static let defaultNodeKey  = "waxwing_user_defaultNode"
    private static let onboardingKey   = "waxwing_user_completedOnboarding"

    private init() {
        let d = UserDefaults.standard
        self.displayName              = d.string(forKey: Self.nameKey) ?? ""
        self.includeIdentityByDefault = d.bool(forKey: Self.identityKey)
        self.includeLocationByDefault = d.bool(forKey: Self.locationKey)
        self.defaultNodeIdentifier    = d.string(forKey: Self.defaultNodeKey)
        self.hasCompletedOnboarding   = d.bool(forKey: Self.onboardingKey)
    }

    /// True if the user has set a non-empty display name.
    var hasName: Bool {
        !displayName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    /// Reset all app-level state. Used by the Settings → Reset action.
    func resetAll() {
        displayName              = ""
        includeIdentityByDefault = false
        includeLocationByDefault = false
        defaultNodeIdentifier    = nil
        hasCompletedOnboarding   = false
    }
}
