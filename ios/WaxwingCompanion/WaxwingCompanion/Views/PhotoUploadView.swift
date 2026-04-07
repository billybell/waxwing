import SwiftUI
import PhotosUI
import CoreLocation

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
    /// Cached size of `selectedImageData` so the view never re-encodes JPEG
    /// during a render pass. Updated only when the source image or quality
    /// actually changes.
    @State private var estimatedSize: Int = 0
    /// Debounce token for slider-driven recompression. Re-encoding a large
    /// JPEG on every slider tick used to OOM the app.
    @State private var recompressTask: Task<Void, Never>?

    /// Maximum dimension (in pixels) we keep in memory for the preview /
    /// upload. Anything larger is downscaled on load. iPhone photos are
    /// often 4000+px on the long edge; keeping the full thing around and
    /// re-encoding it on every slider tick is what was causing the OOM
    /// crash. 2048px is plenty for BLE-bound uploads to the pico.
    private let maxImageDimension: CGFloat = 2048

    // Camera state
    @State private var showingCamera = false
    @State private var cameraImage: UIImage?

    // Upload form
    @State private var fileName = ""
    @State private var compressionQuality: Double = 0.5
    @State private var isUploading = false
    @State private var uploadProgress: Double = 0
    @State private var errorMessage: String?

    // Geo-tagging and identity opt-in
    @State private var includeLocation = UserProfile.shared.includeLocationByDefault
    @State private var includeIdentity = UserProfile.shared.includeIdentityByDefault
    @ObservedObject private var locationManager = LocationManager.shared
    @ObservedObject private var userProfile = UserProfile.shared

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
                    metadataSection
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
                    let scaled = downscaledImage(img, maxDimension: maxImageDimension)
                    previewImage = scaled
                    let data = scaled.jpegData(compressionQuality: compressionQuality)
                    selectedImageData = data
                    estimatedSize = data?.count ?? 0
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

            if estimatedSize > 0 {
                Text(formatBytes(estimatedSize))
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
                        scheduleRecompression(quality: newQuality)
                    }
                if estimatedSize > 0 {
                    Text("Estimated size: \(formatBytes(estimatedSize))")
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

    // MARK: - Metadata Opt-ins

    private var metadataSection: some View {
        Section {
            Toggle(isOn: $includeLocation) {
                HStack(spacing: 6) {
                    Image(systemName: "location.fill")
                        .foregroundStyle(includeLocation ? .blue : .secondary)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Tag location")
                        if includeLocation, let loc = locationManager.location {
                            Text(String(format: "%.5f, %.5f", loc.coordinate.latitude, loc.coordinate.longitude))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } else if includeLocation && !locationManager.isAuthorized {
                            Text("Location permission required")
                                .font(.caption)
                                .foregroundStyle(.orange)
                        }
                    }
                }
            }
            .onChange(of: includeLocation) { _, on in
                if on { locationManager.requestLocation() }
            }

            Toggle(isOn: $includeIdentity) {
                HStack(spacing: 6) {
                    Image(systemName: "person.fill")
                        .foregroundStyle(includeIdentity ? .blue : .secondary)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Include my name")
                        if includeIdentity {
                            if userProfile.hasName {
                                Text(userProfile.displayName)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            } else {
                                TextField("Display name", text: $userProfile.displayName)
                                    .font(.caption)
                                    .textFieldStyle(.roundedBorder)
                            }
                        }
                    }
                }
            }
        } header: {
            Text("Metadata")
        } footer: {
            Text("Optional — helps others find and identify shared images")
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
            guard let data = try await item.loadTransferable(type: Data.self) else { return }
            // Decode + downscale off the main thread so we don't pin a
            // full-resolution UIImage in memory.
            let scaled = await Task.detached(priority: .userInitiated) { () -> (UIImage, Data)? in
                guard let img = UIImage(data: data) else { return nil }
                let down = downscale(img, maxDimension: 2048)
                guard let jpeg = down.jpegData(compressionQuality: 0.5) else { return nil }
                return (down, jpeg)
            }.value

            guard let (img, jpeg) = scaled else { return }
            await MainActor.run {
                previewImage = img
                selectedImageData = jpeg
                estimatedSize = jpeg.count
                if fileName.isEmpty {
                    fileName = defaultFileName()
                }
            }
        } catch {
            await MainActor.run {
                errorMessage = "Failed to load photo: \(error.localizedDescription)"
            }
        }
    }

    /// Debounces slider-driven recompression and runs the JPEG encode off
    /// the main thread. Without this, dragging the quality slider used to
    /// re-encode a multi-megapixel image many times per second on the main
    /// thread, exhausting memory.
    private func scheduleRecompression(quality: Double) {
        recompressTask?.cancel()
        guard let img = previewImage else { return }
        recompressTask = Task {
            // Small debounce window so a slider drag only encodes once.
            try? await Task.sleep(nanoseconds: 120_000_000)
            if Task.isCancelled { return }
            let data = await Task.detached(priority: .userInitiated) {
                img.jpegData(compressionQuality: quality)
            }.value
            if Task.isCancelled { return }
            await MainActor.run {
                selectedImageData = data
                estimatedSize = data?.count ?? 0
            }
        }
    }

    /// Instance helper that mirrors the file-scope `downscale` function so
    /// camera captures (handled inline above) can share the same logic.
    private func downscaledImage(_ image: UIImage, maxDimension: CGFloat) -> UIImage {
        downscale(image, maxDimension: maxDimension)
    }

    private func uploadPhoto() {
        let trimmedName = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canUpload,
              let data = selectedImageData else { return }

        isUploading = true
        errorMessage = nil
        uploadProgress = 0

        let metadata = buildMetadata()

        bleManager.writeFileChunked(
            name: trimmedName,
            data: data,
            progress: { progress in
                uploadProgress = progress
            },
            completion: { [metadata] success in
                if success, let metadata {
                    bleManager.writeFileMeta(name: trimmedName, metadata: metadata) { _ in
                        isUploading = false
                        onUploaded?()
                        dismiss()
                    }
                } else if success {
                    isUploading = false
                    onUploaded?()
                    dismiss()
                } else {
                    isUploading = false
                    errorMessage = bleManager.fileOperationError ?? "Upload failed"
                }
            }
        )
    }

    private func buildMetadata() -> ImageMetadata? {
        let wantsLocation = includeLocation && locationManager.location != nil
        let wantsIdentity = includeIdentity && userProfile.hasName

        guard wantsLocation || wantsIdentity else { return nil }

        return ImageMetadata(
            uploader: wantsIdentity ? userProfile.displayName : nil,
            latitude: wantsLocation ? locationManager.location?.coordinate.latitude : nil,
            longitude: wantsLocation ? locationManager.location?.coordinate.longitude : nil,
            timestamp: Date()
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

// MARK: - Image downscaling

/// Downscales `image` so its longest edge is at most `maxDimension` pixels,
/// using a UIGraphicsImageRenderer (which uses backing memory efficiently
/// and releases intermediate buffers promptly). If the image is already
/// within the limit it is returned unchanged.
fileprivate func downscale(_ image: UIImage, maxDimension: CGFloat) -> UIImage {
    let size = image.size
    let longest = max(size.width, size.height)
    guard longest > maxDimension else { return image }
    let scale = maxDimension / longest
    let newSize = CGSize(width: floor(size.width * scale),
                         height: floor(size.height * scale))
    let format = UIGraphicsImageRendererFormat.default()
    format.scale = 1 // we want pixel dimensions, not @2x/@3x
    format.opaque = true
    let renderer = UIGraphicsImageRenderer(size: newSize, format: format)
    return renderer.image { _ in
        image.draw(in: CGRect(origin: .zero, size: newSize))
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
