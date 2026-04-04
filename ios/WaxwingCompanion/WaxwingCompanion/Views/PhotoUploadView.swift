import SwiftUI
import PhotosUI

/// Sheet view for uploading a photo from the camera or photo library to the node.
/// The image is compressed to JPEG and sent in chunks via BLE.
struct PhotoUploadView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @Environment(\.dismiss) private var dismiss

    // Photo picker state
    @State private var selectedItem: PhotosPickerItem?
    @State private var selectedImageData: Data?
    @State private var previewImage: UIImage?

    // Camera state
    @State private var showingCamera = false
    @State private var cameraImage: UIImage?

    // Upload form
    @State private var fileName = ""
    @State private var compressionQuality: Double = 0.5
    @State private var isUploading = false
    @State private var uploadProgress: Double = 0
    @State private var errorMessage: String?

    /// Called after a successful upload
    var onUploaded: (() -> Void)?

    var body: some View {
        NavigationStack {
            Form {
                // Image source section
                imageSourceSection

                // Preview section
                if let image = previewImage {
                    previewSection(image)
                }

                // File details section (visible once an image is selected)
                if previewImage != nil {
                    fileDetailsSection
                }

                // Upload progress
                if isUploading {
                    uploadProgressSection
                }

                // Error
                if let error = errorMessage {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("Upload Photo")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                    .disabled(isUploading)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Upload") {
                        uploadPhoto()
                    }
                    .disabled(!canUpload || isUploading)
                }
            }
            .onChange(of: selectedItem) { _, newItem in
                Task {
                    await loadSelectedPhoto(newItem)
                }
            }
            .onChange(of: cameraImage) { _, newImage in
                if let img = newImage {
                    previewImage = img
                    selectedImageData = img.jpegData(compressionQuality: compressionQuality)
                    if fileName.isEmpty {
                        fileName = defaultFileName()
                    }
                }
            }
            .fullScreenCover(isPresented: $showingCamera) {
                CameraView(image: $cameraImage)
                    .ignoresSafeArea()
            }
            .interactiveDismissDisabled(isUploading)
        }
    }

    // MARK: - Image Source

    private var imageSourceSection: some View {
        Section {
            PhotosPicker(selection: $selectedItem, matching: .images) {
                Label("Choose from Library", systemImage: "photo.on.rectangle")
            }

            Button {
                showingCamera = true
            } label: {
                Label("Take Photo", systemImage: "camera")
            }
        } header: {
            Text("Source")
        }
    }

    // MARK: - Preview

    private func previewSection(_ image: UIImage) -> some View {
        Section {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(maxHeight: 200)
                .frame(maxWidth: .infinity)
                .clipShape(RoundedRectangle(cornerRadius: 8))
                .padding(.vertical, 4)

            if let data = compressedData {
                Text(formatBytes(data.count))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Preview")
        }
    }

    // MARK: - File Details

    private var fileDetailsSection: some View {
        Section {
            TextField("Filename", text: $fileName)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)

            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Quality")
                    Spacer()
                    Text("\(Int(compressionQuality * 100))%")
                        .foregroundStyle(.secondary)
                }
                Slider(value: $compressionQuality, in: 0.1...1.0, step: 0.1)
                    .onChange(of: compressionQuality) { _, newQuality in
                        // Recompress when quality changes
                        if let img = previewImage {
                            selectedImageData = img.jpegData(compressionQuality: newQuality)
                        }
                    }
                if let data = compressedData {
                    Text("Estimated size: \(formatBytes(data.count))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("File Details")
        } footer: {
            Text("Lower quality = smaller file = faster upload over BLE")
        }
    }

    // MARK: - Upload Progress

    private var uploadProgressSection: some View {
        Section {
            VStack(spacing: 8) {
                ProgressView(value: uploadProgress)
                Text("Uploading... \(Int(uploadProgress * 100))%")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.vertical, 4)
        }
    }

    // MARK: - Computed

    private var compressedData: Data? {
        guard let img = previewImage else { return nil }
        return img.jpegData(compressionQuality: compressionQuality)
    }

    private var canUpload: Bool {
        let trimmedName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        return previewImage != nil
            && !trimmedName.isEmpty
            && !trimmedName.contains("/")
            && !trimmedName.contains("\\")
    }

    // MARK: - Actions

    private func loadSelectedPhoto(_ item: PhotosPickerItem?) async {
        guard let item else { return }
        do {
            if let data = try await item.loadTransferable(type: Data.self),
               let img = UIImage(data: data) {
                await MainActor.run {
                    previewImage = img
                    selectedImageData = img.jpegData(compressionQuality: compressionQuality)
                    if fileName.isEmpty {
                        fileName = defaultFileName()
                    }
                }
            }
        } catch {
            await MainActor.run {
                errorMessage = "Failed to load photo: \(error.localizedDescription)"
            }
        }
    }

    private func uploadPhoto() {
        let trimmedName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canUpload,
              let data = compressedData else { return }

        isUploading = true
        errorMessage = nil
        uploadProgress = 0

        bleManager.writeFileChunked(
            name: trimmedName,
            data: data,
            progress: { progress in
                uploadProgress = progress
            },
            completion: { success in
                isUploading = false
                if success {
                    onUploaded?()
                    dismiss()
                } else {
                    errorMessage = bleManager.fileOperationError ?? "Upload failed"
                }
            }
        )
    }

    private func defaultFileName() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return "photo_\(formatter.string(from: Date())).jpg"
    }

    private func formatBytes(_ bytes: Int) -> String {
        if bytes < 1024 {
            return "\(bytes) B"
        } else if bytes < 1024 * 1024 {
            return String(format: "%.1f KB", Double(bytes) / 1024.0)
        } else {
            return String(format: "%.1f MB", Double(bytes) / (1024.0 * 1024.0))
        }
    }
}

// MARK: - Camera View (UIKit wrapper)

struct CameraView: UIViewControllerRepresentable {
    @Binding var image: UIImage?
    @Environment(\.dismiss) private var dismiss

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker = UIImagePickerController()
        picker.sourceType = .camera
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIImagePickerController, context: Context) {}

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let parent: CameraView

        init(_ parent: CameraView) {
            self.parent = parent
        }

        func imagePickerController(_ picker: UIImagePickerController,
                                   didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]) {
            if let img = info[.originalImage] as? UIImage {
                parent.image = img
            }
            parent.dismiss()
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) {
            parent.dismiss()
        }
    }
}
