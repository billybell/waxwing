import SwiftUI

/// Sheet view for creating a new text file and pushing it to the node.
struct CreateFileView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @Environment(\.dismiss) private var dismiss

    @State private var fileName = ""
    @State private var fileContent = ""
    @State private var isSaving = false
    @State private var errorMessage: String?

    /// Called after a successful save
    var onSaved: (() -> Void)?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Filename", text: $fileName)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                } header: {
                    Text("File Name")
                } footer: {
                    Text("Include the extension, e.g. \"notes.txt\"")
                }

                Section {
                    TextEditor(text: $fileContent)
                        .frame(minHeight: 200)
                        .font(.body.monospaced())
                } header: {
                    Text("Content")
                } footer: {
                    let byteCount = fileContent.utf8.count
                    Text("\(byteCount) / 2048 bytes")
                        .foregroundStyle(byteCount > 2048 ? .red : .secondary)
                }

                if let error = errorMessage {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("New File")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        saveFile()
                    }
                    .disabled(!canSave || isSaving)
                }
            }
            .overlay {
                if isSaving {
                    ProgressView("Saving to node...")
                        .padding()
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
            }
            .interactiveDismissDisabled(isSaving)
        }
    }

    // MARK: - Validation

    private var canSave: Bool {
        let trimmedName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        return !trimmedName.isEmpty
            && !trimmedName.contains("/")
            && !trimmedName.contains("\\")
            && fileContent.utf8.count <= 2048
    }

    // MARK: - Save

    private func saveFile() {
        let trimmedName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canSave else { return }

        isSaving = true
        errorMessage = nil

        bleManager.writeFile(name: trimmedName, content: fileContent) { success in
            isSaving = false
            if success {
                onSaved?()
                dismiss()
            } else {
                errorMessage = bleManager.fileOperationError ?? "Failed to save file"
            }
        }
    }
}
