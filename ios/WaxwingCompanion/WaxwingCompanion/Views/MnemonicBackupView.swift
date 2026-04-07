import SwiftUI

/// Displays the user's BIP-39 recovery phrase in a numbered grid so they can
/// write it down. The phrase is loaded into memory only while the sheet is
/// open and cleared on dismiss.
struct MnemonicBackupView: View {
    @ObservedObject private var identity = ContentIdentity.shared
    @Environment(\.dismiss) private var dismiss

    @State private var loadError: String?
    @State private var hasConfirmedWritten = false

    var body: some View {
        NavigationStack {
            Group {
                if let words = identity.revealedMnemonic {
                    ScrollView {
                        VStack(spacing: 16) {
                            warningBanner

                            wordGrid(words: words)
                                .padding(.horizontal)

                            Toggle(isOn: $hasConfirmedWritten) {
                                Text("I have written this phrase down somewhere safe")
                                    .font(.footnote)
                            }
                            .padding(.horizontal)
                            .padding(.top, 8)
                        }
                        .padding(.vertical)
                    }
                } else if let err = loadError {
                    VStack(spacing: 12) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.largeTitle)
                            .foregroundStyle(.orange)
                        Text(err)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal)
                    }
                } else {
                    ProgressView()
                }
            }
            .navigationTitle("Recovery Phrase")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                        .disabled(identity.revealedMnemonic != nil && !hasConfirmedWritten)
                }
            }
            .onAppear {
                do { try identity.revealMnemonic() }
                catch { loadError = error.localizedDescription }
            }
            .onDisappear {
                identity.hideMnemonic()
            }
            .interactiveDismissDisabled(identity.revealedMnemonic != nil && !hasConfirmedWritten)
        }
    }

    private var warningBanner: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Keep this private", systemImage: "lock.shield.fill")
                .font(.headline)
            Text("Anyone with these words can sign content as you. Never share them, never type them into a website, never store them in cloud notes.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.12))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .padding(.horizontal)
    }

    private func wordGrid(words: [String]) -> some View {
        let columns = [
            GridItem(.flexible(), spacing: 8),
            GridItem(.flexible(), spacing: 8)
        ]
        return LazyVGrid(columns: columns, spacing: 8) {
            ForEach(Array(words.enumerated()), id: \.offset) { i, word in
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
}
