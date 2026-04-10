import SwiftUI

/// App-level settings: user identity, default upload metadata, default node,
/// and Content Identity backup/management. This is distinct from
/// `NodeSettingsView`, which shows per-device technical info.
struct AppSettingsView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject private var profile  = UserProfile.shared
    @ObservedObject private var identity = ContentIdentity.shared
    @Environment(\.dismiss) private var dismiss

    @State private var showingBackupSheet  = false
    @State private var showingWipeConfirm  = false
    @State private var showingRestoreSheet = false
    @State private var showingClearCacheConfirm = false

    /// Local snapshot of the on-disk cache stats so the section refreshes
    /// when the user clears the cache. Recomputed on appear and after
    /// any clear action.
    @State private var cacheFileCount: Int = 0
    @State private var cacheBytes:     Int = 0

    var body: some View {
        NavigationStack {
            Form {
                identitySection
                contentKeySection
                defaultsSection
                defaultNodeSection
                imageCacheSection
                aboutSection
                dangerZoneSection
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .onAppear { refreshCacheStats() }
            .alert("Clear image cache?", isPresented: $showingClearCacheConfirm) {
                Button("Clear", role: .destructive) {
                    WaxwingImageCache.shared.clearAll()
                    refreshCacheStats()
                }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("This deletes the locally cached copies of all Waxwing images. They will be re-downloaded from the node next time you view them.")
            }
            .sheet(isPresented: $showingBackupSheet) {
                MnemonicBackupView()
            }
            .sheet(isPresented: $showingRestoreSheet) {
                RestoreIdentityView()
            }
            .alert("Wipe identity?", isPresented: $showingWipeConfirm) {
                Button("Wipe", role: .destructive) {
                    identity.wipe()
                    profile.resetAll()
                    dismiss()
                }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("This permanently removes your Content Identity from this device. Anything you signed will still verify, but you will not be able to sign new content unless you restore from your backup phrase. This cannot be undone.")
            }
        }
    }

    // MARK: - Identity (display name)

    private var identitySection: some View {
        Section {
            HStack {
                Image(systemName: "person.crop.circle")
                    .foregroundStyle(.blue)
                    .frame(width: 24)
                TextField("Display name (optional)", text: $profile.displayName)
                    .textInputAutocapitalization(.words)
                    .submitLabel(.done)
            }
        } header: {
            Text("You")
        } footer: {
            Text("Your display name is optional and only attached to uploads when you opt in. Other people on the mesh see your public key as a permanent pseudonym.")
        }
    }

    // MARK: - Content Identity (public key + backup)

    private var contentKeySection: some View {
        Section {
            if let fp = identity.fingerprint, let hex = identity.publicKeyHex {
                HStack {
                    Image(systemName: "key.fill")
                        .foregroundStyle(.purple)
                        .frame(width: 24)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Fingerprint")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(fp)
                            .font(.body.monospaced())
                    }
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text("Public Key")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(hex)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                        .lineLimit(2)
                        .truncationMode(.middle)
                }
                .padding(.vertical, 2)

                Button {
                    showingBackupSheet = true
                } label: {
                    Label("Show Recovery Phrase", systemImage: "doc.text.magnifyingglass")
                }
            } else {
                Label("No identity yet", systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)

                Button {
                    do { _ = try identity.generateNew() }
                    catch { /* surfaced via UI elsewhere */ }
                } label: {
                    Label("Generate New Identity", systemImage: "sparkles")
                }
                .disabled(!BIP39.isWordlistLoaded)

                Button {
                    showingRestoreSheet = true
                } label: {
                    Label("Restore from Phrase", systemImage: "arrow.down.doc")
                }
                .disabled(!BIP39.isWordlistLoaded)
            }
        } header: {
            Text("Content Identity")
        } footer: {
            if BIP39.isWordlistLoaded {
                Text("Your Content Identity signs anything you publish to the mesh. The recovery phrase is the only way to restore it on another device.")
            } else {
                Text("⚠︎ BIP-39 wordlist is missing from this build. See Crypto/Resources/README.md.")
                    .foregroundStyle(.orange)
            }
        }
    }

    // MARK: - Default upload toggles

    private var defaultsSection: some View {
        Section {
            Toggle(isOn: $profile.includeLocationByDefault) {
                Label("Tag location by default", systemImage: "location.fill")
            }
            Toggle(isOn: $profile.includeIdentityByDefault) {
                Label("Include name by default", systemImage: "person.fill")
            }
        } header: {
            Text("Upload Defaults")
        } footer: {
            Text("These set the initial state of the corresponding switches on the upload screen. You can still change them per-upload.")
        }
    }

    // MARK: - Default node

    private var defaultNodeSection: some View {
        Section {
            if bleManager.discoveredNodes.isEmpty {
                Text("No nodes discovered yet")
                    .foregroundStyle(.secondary)
            } else {
                Picker(selection: $profile.defaultNodeIdentifier) {
                    Text("None").tag(String?.none)
                    ForEach(bleManager.discoveredNodes) { node in
                        Text(node.displayName).tag(Optional(node.id.uuidString))
                    }
                } label: {
                    Label("Default Node", systemImage: "antenna.radiowaves.left.and.right")
                }
            }
        } header: {
            Text("Default Node")
        } footer: {
            Text("If set, the app will prefer this Waxwing node when more than one is in range.")
        }
    }

    // MARK: - Image Cache

    private var imageCacheSection: some View {
        Section {
            HStack {
                Image(systemName: "photo.stack")
                    .foregroundStyle(.teal)
                    .frame(width: 24)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Cached images")
                        .font(.body)
                    Text("\(cacheFileCount) file\(cacheFileCount == 1 ? "" : "s") · \(formatBytes(cacheBytes))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Button(role: .destructive) {
                showingClearCacheConfirm = true
            } label: {
                Label("Clear image cache", systemImage: "trash")
            }
            .disabled(cacheFileCount == 0)
        } header: {
            Text("Image Cache")
        } footer: {
            Text("Waxwing images are cached on this device by content hash, so reconnecting to a node skips re-downloading anything you already have. Clear the cache to force fresh downloads (useful for testing).")
        }
    }

    private func refreshCacheStats() {
        cacheFileCount = WaxwingImageCache.shared.diskFileCount()
        cacheBytes     = WaxwingImageCache.shared.diskByteCount()
    }

    private func formatBytes(_ bytes: Int) -> String {
        if bytes < 1024 {
            return "\(bytes) B"
        } else if bytes < 1024 * 1024 {
            return String(format: "%.1f KB", Double(bytes) / 1024.0)
        } else {
            return String(format: "%.2f MB", Double(bytes) / (1024.0 * 1024.0))
        }
    }

    // MARK: - About

    private var aboutSection: some View {
        Section {
            HStack {
                Label("App", systemImage: "app.badge")
                Spacer()
                Text(appVersionString)
                    .foregroundStyle(.secondary)
            }
            HStack {
                Label("Protocol", systemImage: "network")
                Spacer()
                Text("Waxwing Mesh v1")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("About")
        }
    }

    // MARK: - Danger zone

    private var dangerZoneSection: some View {
        Section {
            Button(role: .destructive) {
                showingWipeConfirm = true
            } label: {
                Label("Wipe Identity & Reset", systemImage: "trash")
            }
            .disabled(!identity.hasIdentity && !profile.hasCompletedOnboarding)
        } header: {
            Text("Danger Zone")
        }
    }

    private var appVersionString: String {
        let info = Bundle.main.infoDictionary
        let v = info?["CFBundleShortVersionString"] as? String ?? "?"
        let b = info?["CFBundleVersion"] as? String ?? "?"
        return "\(v) (\(b))"
    }
}
