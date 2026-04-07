import SwiftUI

// MARK: - Onboarding
//
// Mandatory first-launch flow. The user cannot reach the main app until
// they've either generated a brand-new Content Identity or restored one
// from a recovery phrase, AND confirmed they've backed it up.

struct OnboardingView: View {
    @ObservedObject private var profile  = UserProfile.shared
    @ObservedObject private var identity = ContentIdentity.shared

    @State private var step: Step = .welcome
    @State private var generatedWords: [String] = []
    @State private var hasConfirmedBackup = false
    @State private var errorMessage: String?
    @State private var showingRestoreSheet = false

    enum Step {
        case welcome
        case name
        case createOrRestore
        case showPhrase
        case finish
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                content
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding()

                footer
                    .padding()
                    .background(.ultraThinMaterial)
            }
            .navigationTitle(navigationTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if step != .welcome && step != .finish {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Back") { goBack() }
                    }
                }
            }
            .sheet(isPresented: $showingRestoreSheet, onDismiss: {
                if identity.hasIdentity {
                    step = .finish
                }
            }) {
                RestoreIdentityView()
            }
        }
        .interactiveDismissDisabled(true)
    }

    // MARK: - Step content

    @ViewBuilder
    private var content: some View {
        switch step {
        case .welcome:        welcomeStep
        case .name:           nameStep
        case .createOrRestore: createOrRestoreStep
        case .showPhrase:     showPhraseStep
        case .finish:         finishStep
        }
    }

    private var welcomeStep: some View {
        VStack(spacing: 24) {
            Spacer()
            Image(systemName: "antenna.radiowaves.left.and.right.circle.fill")
                .font(.system(size: 80))
                .foregroundStyle(.blue)
            Text("Welcome to Waxwing")
                .font(.largeTitle.bold())
            Text("A pocket companion for the Waxwing mesh. Before you start, we need to set up your identity. It will only take a minute.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .padding(.horizontal)
            Spacer()
        }
    }

    private var nameStep: some View {
        VStack(alignment: .leading, spacing: 16) {
            Image(systemName: "person.crop.circle.badge.questionmark")
                .font(.system(size: 56))
                .foregroundStyle(.blue)
                .frame(maxWidth: .infinity)

            Text("What should others call you?")
                .font(.title2.bold())

            Text("This name is optional. It's only attached to things you share when you opt in. You can change it any time, or leave it blank to stay anonymous.")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            TextField("Display name (optional)", text: $profile.displayName)
                .textInputAutocapitalization(.words)
                .textFieldStyle(.roundedBorder)
                .padding(.top, 4)

            Spacer()
        }
    }

    private var createOrRestoreStep: some View {
        VStack(spacing: 24) {
            Image(systemName: "key.horizontal.fill")
                .font(.system(size: 56))
                .foregroundStyle(.purple)

            Text("Create your identity")
                .font(.title2.bold())

            Text("Your Waxwing identity is a cryptographic keypair. It signs anything you publish so others can verify it really came from you. The private key never leaves this device.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)

            if !BIP39.isWordlistLoaded {
                Label("BIP-39 wordlist missing — see Crypto/Resources/README.md.",
                      systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
                    .font(.footnote)
                    .padding(.horizontal)
            }

            if let err = errorMessage {
                Text(err)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }

            VStack(spacing: 12) {
                Button {
                    generateNewIdentity()
                } label: {
                    Label("Create New Identity", systemImage: "sparkles")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!BIP39.isWordlistLoaded)

                Button {
                    showingRestoreSheet = true
                } label: {
                    Label("I already have a recovery phrase", systemImage: "arrow.down.doc")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(!BIP39.isWordlistLoaded)
            }
            .padding(.horizontal)

            Spacer()
        }
    }

    private var showPhraseStep: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Label("Write this down", systemImage: "pencil.and.list.clipboard")
                    .font(.title2.bold())

                Text("These 12 words are the only way to restore your identity if you lose this device. Write them on paper and store them somewhere safe. Don't take a screenshot. Don't paste them into a cloud notes app.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                wordGrid

                Toggle(isOn: $hasConfirmedBackup) {
                    Text("I've written down all 12 words in order")
                        .font(.footnote)
                }
                .padding(.top, 8)
            }
        }
    }

    private var finishStep: some View {
        VStack(spacing: 20) {
            Spacer()
            Image(systemName: "checkmark.seal.fill")
                .font(.system(size: 72))
                .foregroundStyle(.green)
            Text("You're all set")
                .font(.largeTitle.bold())
            if let fp = identity.fingerprint {
                Text("Fingerprint")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(fp)
                    .font(.body.monospaced())
            }
            Text("You can review your identity, change defaults, or view your recovery phrase any time from the Settings menu.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .padding(.horizontal)
            Spacer()
        }
    }

    // MARK: - Word grid

    private var wordGrid: some View {
        let columns = [
            GridItem(.flexible(), spacing: 8),
            GridItem(.flexible(), spacing: 8)
        ]
        return LazyVGrid(columns: columns, spacing: 8) {
            ForEach(Array(generatedWords.enumerated()), id: \.offset) { i, word in
                HStack(spacing: 8) {
                    Text("\(i + 1).")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 24, alignment: .trailing)
                    Text(word)
                        .font(.body.monospaced())
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.vertical, 8)
                .padding(.horizontal, 10)
                .background(.quaternary.opacity(0.5))
                .clipShape(RoundedRectangle(cornerRadius: 8))
            }
        }
    }

    // MARK: - Footer (next/finish button)

    private var footer: some View {
        HStack {
            Spacer()
            Button {
                advance()
            } label: {
                Text(primaryButtonTitle)
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!canAdvance)
            Spacer()
        }
    }

    private var primaryButtonTitle: String {
        switch step {
        case .welcome:          return "Get Started"
        case .name:             return profile.hasName ? "Continue" : "Skip"
        case .createOrRestore:  return "Continue"
        case .showPhrase:       return "Continue"
        case .finish:           return "Open Waxwing"
        }
    }

    private var canAdvance: Bool {
        switch step {
        case .welcome:          return true
        case .name:             return true     // name is optional
        case .createOrRestore:  return identity.hasIdentity
        case .showPhrase:       return hasConfirmedBackup
        case .finish:           return true
        }
    }

    private var navigationTitle: String {
        switch step {
        case .welcome:          return ""
        case .name:             return "Your Name"
        case .createOrRestore:  return "Identity"
        case .showPhrase:       return "Backup"
        case .finish:           return "Done"
        }
    }

    // MARK: - Navigation

    private func advance() {
        switch step {
        case .welcome:
            step = .name
        case .name:
            step = .createOrRestore
        case .createOrRestore:
            // If they restored, skip showing the phrase (they already have it).
            if generatedWords.isEmpty {
                step = .finish
            } else {
                step = .showPhrase
            }
        case .showPhrase:
            step = .finish
        case .finish:
            profile.hasCompletedOnboarding = true
        }
    }

    private func goBack() {
        switch step {
        case .name:             step = .welcome
        case .createOrRestore:  step = .name
        case .showPhrase:       step = .createOrRestore
        case .finish:           step = generatedWords.isEmpty ? .createOrRestore : .showPhrase
        case .welcome:          break
        }
    }

    // MARK: - Actions

    private func generateNewIdentity() {
        do {
            generatedWords = try identity.generateNew()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
