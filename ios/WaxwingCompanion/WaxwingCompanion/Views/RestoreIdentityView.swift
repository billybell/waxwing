import SwiftUI

/// Lets the user restore their Content Identity by typing in their 12- or
/// 24-word BIP-39 recovery phrase.
struct RestoreIdentityView: View {
    @ObservedObject private var identity = ContentIdentity.shared
    @Environment(\.dismiss) private var dismiss

    @State private var phraseText: String = ""
    @State private var errorMessage: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextEditor(text: $phraseText)
                        .frame(minHeight: 120)
                        .font(.body.monospaced())
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                } header: {
                    Text("Recovery Phrase")
                } footer: {
                    Text("Enter your 12 or 24 words separated by spaces, in the original order.")
                }

                if let err = errorMessage {
                    Section {
                        Label(err, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }

                Section {
                    Button {
                        attemptRestore()
                    } label: {
                        Label("Restore Identity", systemImage: "arrow.down.doc")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(words.count < 12)
                }
            }
            .navigationTitle("Restore")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }

    private var words: [String] {
        phraseText
            .lowercased()
            .split(whereSeparator: { $0.isWhitespace })
            .map(String.init)
    }

    private func attemptRestore() {
        do {
            try identity.restore(mnemonic: words)
            dismiss()
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
