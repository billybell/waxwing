import SwiftUI
import PhotosUI
import CoreLocation

/// Full-screen sheet for composing a Waxwing micro-image.
/// Flow: pick a photo → choose palette → adjust contrast/brightness → add caption → preview → upload.
struct ComposeImageView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @Environment(\.dismiss) private var dismiss

    // Source image
    @State private var selectedItem: PhotosPickerItem?
    @State private var sourceImage: UIImage?
    @State private var showingCamera = false
    @State private var cameraImage: UIImage?

    // Processing parameters
    @State private var selectedPaletteId = "cedar"
    @State private var contrast: Float = 1.15
    @State private var brightness: Float = 0.0
    @State private var caption: String = ""
    @State private var rotationSteps: Int = 0  // 0, 1, 2, 3 → 0°, 90°, 180°, 270° CW

    // Processed output
    @State private var previewImage: UIImage?
    @State private var pngData: Data?
    @State private var isProcessing = false

    // Upload state
    @State private var isUploading = false
    @State private var uploadProgress: Double = 0
    @State private var errorMessage: String?

    // Geo-tagging and identity opt-in
    @State private var includeLocation = UserProfile.shared.includeLocationByDefault
    @State private var includeIdentity = UserProfile.shared.includeIdentityByDefault
    @ObservedObject private var locationManager = LocationManager.shared
    @ObservedObject private var userProfile = UserProfile.shared

    /// Shared image cache for grid display.
    @ObservedObject var imageCache: WaxwingImageCache

    var onUploaded: (() -> Void)?

    private var selectedPalette: WaxwingPalette {
        WaxwingPalettes.palette(for: selectedPaletteId)
    }

    private var accentColor: Color {
        Color(hex: selectedPalette.hexColors[2])
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 24) {
                    // Source picker
                    sourceSection

                    if sourceImage != nil {
                        // Rotation controls
                        rotationSection

                        // Palette picker
                        paletteSection

                        // Preview
                        previewSection

                        // Controls
                        controlsSection

                        // Caption
                        captionSection

                        // Metadata opt-ins
                        metadataSection

                        // Size info
                        sizeSection

                        // Upload
                        if isUploading {
                            uploadSection
                        }
                    }
                }
                .padding()
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Compose Image")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                        .disabled(isUploading)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { uploadImage() }
                        .disabled(sourceImage == nil || isUploading || pngData == nil)
                        .fontWeight(.semibold)
                }
            }
            .onChange(of: selectedItem) { _, newItem in
                rotationSteps = 0
                Task { await loadPhoto(newItem) }
            }
            .onChange(of: cameraImage) { _, img in
                if let img {
                    rotationSteps = 0
                    sourceImage = img
                    reprocess()
                }
            }
            .fullScreenCover(isPresented: $showingCamera) {
                CameraView(image: $cameraImage).ignoresSafeArea()
            }
            .interactiveDismissDisabled(isUploading)
        }
    }

    // MARK: - Source Section

    private var sourceSection: some View {
        VStack(spacing: 12) {
            if sourceImage == nil {
                // No image yet — prominent picker
                VStack(spacing: 16) {
                    Image(systemName: "camera.viewfinder")
                        .font(.system(size: 44))
                        .foregroundStyle(.secondary)

                    Text("Choose a photo to transform")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)

                    HStack(spacing: 12) {
                        PhotosPicker(selection: $selectedItem, matching: .images) {
                            Label("Library", systemImage: "photo.on.rectangle")
                                .font(.subheadline.weight(.medium))
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)

                        Button {
                            showingCamera = true
                        } label: {
                            Label("Camera", systemImage: "camera")
                                .font(.subheadline.weight(.medium))
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                    }
                }
                .padding(.vertical, 32)
            } else {
                // Image loaded — small change-photo row
                HStack {
                    PhotosPicker(selection: $selectedItem, matching: .images) {
                        Label("Change Photo", systemImage: "arrow.triangle.2.circlepath")
                            .font(.caption)
                    }
                    Spacer()
                    Button {
                        showingCamera = true
                    } label: {
                        Label("Camera", systemImage: "camera")
                            .font(.caption)
                    }
                }
                .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - Rotation Section

    private var rotationSection: some View {
        HStack(spacing: 16) {
            Text("ROTATE")
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.secondary)
                .tracking(1)

            Spacer()

            Button {
                rotationSteps = (rotationSteps + 3) % 4  // 90° counter-clockwise
                reprocess()
            } label: {
                Image(systemName: "rotate.left")
                    .font(.title3)
            }
            .buttonStyle(.bordered)

            Button {
                rotationSteps = (rotationSteps + 1) % 4  // 90° clockwise
                reprocess()
            } label: {
                Image(systemName: "rotate.right")
                    .font(.title3)
            }
            .buttonStyle(.bordered)

            Text(rotationLabel)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: 32, alignment: .trailing)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    private var rotationLabel: String {
        switch rotationSteps {
        case 1: return "90°"
        case 2: return "180°"
        case 3: return "270°"
        default: return "0°"
        }
    }

    // MARK: - Palette Section

    private var paletteSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("COLOR PALETTE")
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.secondary)
                .tracking(1)

            HStack(spacing: 10) {
                ForEach(WaxwingPalettes.all) { pal in
                    Button {
                        selectedPaletteId = pal.id
                        reprocess()
                    } label: {
                        VStack(spacing: 6) {
                            // Swatch row
                            HStack(spacing: 3) {
                                ForEach(0..<4, id: \.self) { i in
                                    RoundedRectangle(cornerRadius: 3)
                                        .fill(Color(hex: pal.hexColors[i]))
                                        .frame(width: 18, height: 18)
                                }
                            }

                            Text(pal.name)
                                .font(.caption.weight(.medium))
                                .foregroundStyle(selectedPaletteId == pal.id ? .primary : .secondary)

                            Text(pal.subtitle)
                                .font(.caption2)
                                .foregroundStyle(.tertiary)
                        }
                        .padding(.vertical, 10)
                        .padding(.horizontal, 8)
                        .frame(maxWidth: .infinity)
                        .background(
                            RoundedRectangle(cornerRadius: 10)
                                .fill(selectedPaletteId == pal.id ? Color(.systemBackground) : Color.clear)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 10)
                                .stroke(selectedPaletteId == pal.id ? accentColor : Color.clear, lineWidth: 2)
                        )
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Preview

    private var previewSection: some View {
        VStack(spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 12)
                    .fill(Color(selectedPalette.backgroundColor))

                if let img = previewImage {
                    Image(uiImage: img)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFit()
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .padding(4)
                } else if isProcessing {
                    ProgressView()
                        .tint(.white)
                }
            }
            .aspectRatio(1, contentMode: .fit)

            HStack {
                Text("128 × 128 px")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("Bayer 4×4")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - Controls (Contrast / Brightness)

    private var controlsSection: some View {
        VStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Contrast")
                        .font(.caption.weight(.medium))
                    Spacer()
                    Text(String(format: "%.2f", contrast))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Slider(value: Binding(
                    get: { contrast },
                    set: { contrast = $0; reprocess() }
                ), in: 0.5...2.5, step: 0.05)
                .tint(accentColor)
            }

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Brightness")
                        .font(.caption.weight(.medium))
                    Spacer()
                    Text(String(format: "%+.2f", brightness))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Slider(value: Binding(
                    get: { brightness },
                    set: { brightness = $0; reprocess() }
                ), in: -0.4...0.4, step: 0.02)
                .tint(accentColor)
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Caption

    private var captionSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            TextField("Add a caption...", text: $caption)
                .font(.subheadline)
                .textFieldStyle(.roundedBorder)

            Text("Embedded as PNG metadata · \(caption.count)/140")
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Metadata Opt-ins

    private var metadataSection: some View {
        VStack(spacing: 12) {
            Toggle(isOn: $includeLocation) {
                HStack(spacing: 6) {
                    Image(systemName: "location.fill")
                        .foregroundStyle(includeLocation ? .blue : .secondary)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Tag location")
                            .font(.subheadline)
                        if includeLocation, let loc = locationManager.location {
                            Text(String(format: "%.5f, %.5f", loc.coordinate.latitude, loc.coordinate.longitude))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        } else if includeLocation && !locationManager.isAuthorized {
                            Text("Location permission required")
                                .font(.caption2)
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
                            .font(.subheadline)
                        if includeIdentity {
                            if userProfile.hasName {
                                Text(userProfile.displayName)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            } else {
                                TextField("Display name", text: $userProfile.displayName)
                                    .font(.caption2)
                                    .textFieldStyle(.roundedBorder)
                            }
                        }
                    }
                }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Size Info

    private var sizeSection: some View {
        VStack(spacing: 6) {
            if let data = pngData {
                HStack {
                    Text("PNG size")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(formatBytes(data.count))
                        .font(.caption.weight(.medium))
                        .foregroundStyle(data.count <= 4096 ? .green : .orange)
                }
                // Estimated indexed size (≈32% of RGBA PNG)
                let estIndexed = Int(Double(data.count) * 0.32)
                HStack {
                    Text("Est. indexed")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text("~\(formatBytes(estIndexed))")
                        .font(.caption.weight(.medium))
                        .foregroundStyle(estIndexed <= 1024 ? .green : .orange)
                }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Upload Progress

    private var uploadSection: some View {
        VStack(spacing: 8) {
            ProgressView(value: uploadProgress)
                .tint(accentColor)
            Text("Uploading… \(Int(uploadProgress * 100))%")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemGroupedBackground)))
    }

    // MARK: - Actions

    private func loadPhoto(_ item: PhotosPickerItem?) async {
        guard let item else { return }
        do {
            if let data = try await item.loadTransferable(type: Data.self),
               let img = UIImage(data: data) {
                await MainActor.run {
                    sourceImage = img
                    reprocess()
                }
            }
        } catch {
            await MainActor.run {
                errorMessage = "Failed to load photo: \(error.localizedDescription)"
            }
        }
    }

    /// Run rotation + dithering on a background queue.
    private func reprocess() {
        guard let src = sourceImage else { return }
        isProcessing = true
        let pal = selectedPalette
        let c = contrast
        let b = brightness
        let steps = rotationSteps

        DispatchQueue.global(qos: .userInitiated).async {
            // Normalize EXIF orientation first so the raw pixel buffer
            // matches what the user sees.  Without this, camera photos
            // (which carry orientation metadata like .right) would be
            // double-rotated: once by image.draw() respecting EXIF and
            // once by our manual rotation transform.
            let normalized = Self.normalizeOrientation(src)

            // Apply manual rotation (if any) on the orientation-fixed image
            let rotated = steps > 0
                ? Self.rotateImage(normalized, steps: steps)
                : normalized

            let result = WaxwingImageProcessor.process(
                source: rotated, palette: pal, contrast: c, brightness: b
            )
            DispatchQueue.main.async {
                isProcessing = false
                previewImage = result?.image
                pngData = result?.pngData
            }
        }
    }

    /// Bake EXIF orientation into the actual pixels so that CGImage-level
    /// operations (crop, rotate, pixel extraction) see the correct layout.
    private static func normalizeOrientation(_ image: UIImage) -> UIImage {
        guard image.imageOrientation != .up else { return image }
        let format = UIGraphicsImageRendererFormat()
        format.scale = image.scale
        let renderer = UIGraphicsImageRenderer(size: image.size, format: format)
        return renderer.image { _ in
            image.draw(at: .zero)
        }
    }

    /// Rotate a UIImage by the given number of 90° clockwise steps.
    /// The image MUST already be orientation-normalized (`.up`).
    private static func rotateImage(_ image: UIImage, steps: Int) -> UIImage {
        let normalizedSteps = steps % 4
        guard normalizedSteps > 0 else { return image }

        let radians = CGFloat(normalizedSteps) * (.pi / 2.0)
        let size = image.size

        // For 90° and 270° rotations, width and height are swapped
        let newSize: CGSize
        if normalizedSteps == 1 || normalizedSteps == 3 {
            newSize = CGSize(width: size.height, height: size.width)
        } else {
            newSize = size
        }

        let format = UIGraphicsImageRendererFormat()
        format.scale = image.scale
        let renderer = UIGraphicsImageRenderer(size: newSize, format: format)

        return renderer.image { context in
            let ctx = context.cgContext
            ctx.translateBy(x: newSize.width / 2, y: newSize.height / 2)
            ctx.rotate(by: radians)
            image.draw(in: CGRect(
                x: -size.width / 2,
                y: -size.height / 2,
                width: size.width,
                height: size.height
            ))
        }
    }

    private func uploadImage() {
        guard let rawPng = pngData else { return }

        // Strip alpha channel now (deferred from preview to save memory)
        let rgbPng = WaxwingImageProcessor.stripAlphaForUpload(rawPng)

        // Embed caption as a PNG tEXt chunk if one was entered
        let trimmedCaption = caption.trimmingCharacters(in: .whitespacesAndNewlines)
        let data = trimmedCaption.isEmpty
            ? rgbPng
            : PNGMetadata.embedCaption(in: rgbPng, caption: trimmedCaption)

        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        let name = "waxwing_\(formatter.string(from: Date())).png"

        // Cache the preview image and caption for the grid view
        if let img = previewImage {
            imageCache.store(
                name: name,
                image: img,
                caption: trimmedCaption.isEmpty ? nil : trimmedCaption
            )
        }

        isUploading = true
        errorMessage = nil
        uploadProgress = 0

        // Build metadata if the user opted in to anything
        let metadata = buildMetadata()

        bleManager.writeFileChunked(
            name: name,
            data: data,
            progress: { p in uploadProgress = p },
            completion: { [metadata] success in
                if success, let metadata {
                    // Write metadata sidecar after successful file upload
                    bleManager.writeFileMeta(name: name, metadata: metadata) { _ in
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

    private func formatBytes(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        else if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024.0) }
        else { return String(format: "%.1f MB", Double(bytes) / (1024.0 * 1024.0)) }
    }
}

// MARK: - Color from hex string

extension Color {
    init(hex: String) {
        let h = hex.trimmingCharacters(in: CharacterSet(charactersIn: "#"))
        var rgb: UInt64 = 0
        Scanner(string: h).scanHexInt64(&rgb)
        self.init(
            red: Double((rgb >> 16) & 0xFF) / 255,
            green: Double((rgb >> 8) & 0xFF) / 255,
            blue: Double(rgb & 0xFF) / 255
        )
    }
}
