import SwiftUI
import PhotosUI
import CoreLocation

/// Full-screen sheet for composing a Waxwing micro-image.
/// Flow: pick a photo → choose palette → adjust contrast/brightness → add caption → preview → upload.
struct ComposeImageView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @Environment(\.dismiss) private var dismiss

    // Source image — held only briefly during prepare(); we drop the
    // full-resolution UIImage as soon as the 128×128 PreparedSource is
    // available, so slider tweaks don't pin the original photo in memory.
    @State private var selectedItem: PhotosPickerItem?
    @State private var showingPhotoPicker = false
    @State private var showingCamera = false
    @State private var cameraImage: UIImage?
    @State private var preparedSource: PreparedSource?
    @State private var isPreparing = false

    // Processing parameters
    @State private var selectedPaletteId = "cedar"
    @State private var contrast: Float = 1.15
    @State private var brightness: Float = 0.0
    @State private var caption: String = ""
    @State private var rotationSteps: Int = 0  // 0, 1, 2, 3 → 0°, 90°, 180°, 270° CW

    // Processed output
    @State private var previewImage: UIImage?
    @State private var pngData: Data?

    // The render task we cancel when slider/palette state moves faster
    // than we can render — keeps work from piling up across slider drags.
    @State private var renderTask: Task<Void, Never>?

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

                    if preparedSource != nil {
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
                        .disabled(preparedSource == nil || isUploading || pngData == nil)
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
                    Task { await prepareSource(from: img) }
                }
            }
            .fullScreenCover(isPresented: $showingCamera) {
                CameraView(image: $cameraImage).ignoresSafeArea()
            }
            .photosPicker(isPresented: $showingPhotoPicker,
                          selection: $selectedItem,
                          matching: .images)
            .interactiveDismissDisabled(isUploading)
            .alert("Upload failed", isPresented: errorAlertPresented) {
                Button("OK") { errorMessage = nil }
            } message: {
                Text(errorMessage ?? "")
            }
        }
    }

    /// Two-way binding so dismissing the alert clears `errorMessage`
    /// (otherwise a non-nil message would re-present the alert each
    /// time SwiftUI re-renders the view).
    private var errorAlertPresented: Binding<Bool> {
        Binding(
            get: { errorMessage != nil },
            set: { if !$0 { errorMessage = nil } }
        )
    }

    // MARK: - Source Section

    private var sourceSection: some View {
        VStack(spacing: 12) {
            if preparedSource == nil {
                // No image yet — prominent picker
                VStack(spacing: 16) {
                    Image(systemName: "camera.viewfinder")
                        .font(.system(size: 44))
                        .foregroundStyle(.secondary)

                    Text("Choose a photo to transform")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)

                    HStack(spacing: 12) {
                        Button {
                            showingPhotoPicker = true
                        } label: {
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
                    Button {
                        showingPhotoPicker = true
                    } label: {
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
                rerender()
            } label: {
                Image(systemName: "rotate.left")
                    .font(.title3)
            }
            .buttonStyle(.bordered)

            Button {
                rotationSteps = (rotationSteps + 1) % 4  // 90° clockwise
                rerender()
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
                        rerender()
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
                } else if isPreparing {
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
                    set: { contrast = $0; rerender() }
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
                    set: { brightness = $0; rerender() }
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
                            locationPermissionPrompt
                        }
                    }
                }
            }
            .onChange(of: includeLocation) { _, on in
                if on { locationManager.requestLocation() }
            }
            .onChange(of: locationManager.authorizationStatus) { _, _ in
                // Once permission is granted (or re-granted), kick a
                // location fix so the toggle's address text fills in
                // without the user needing to flip the switch again.
                if includeLocation && locationManager.isAuthorized {
                    locationManager.requestLocation()
                }
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
                await prepareSource(from: img)
            }
        } catch {
            await MainActor.run {
                errorMessage = "Failed to load photo: \(error.localizedDescription)"
            }
        }
    }

    /// Reduce the full-resolution photo down to a 128×128 grayscale buffer
    /// off the main thread. The full-res `UIImage` is only alive for the
    /// duration of this call — once the `PreparedSource` is in @State,
    /// the original is dropped and slider changes operate on the small
    /// buffer instead. We use a `DispatchQueue` continuation rather than
    /// `Task.detached` because `UIImage` isn't `Sendable` and we don't
    /// want to fight strict concurrency over a one-shot background hop.
    @MainActor
    private func prepareSource(from image: UIImage) async {
        isPreparing = true
        renderTask?.cancel()
        renderTask = nil

        let prepared: PreparedSource? = await withCheckedContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                cont.resume(returning: WaxwingImageProcessor.prepare(source: image))
            }
        }

        isPreparing = false
        preparedSource = prepared
        rerender()
    }

    /// Re-run the cheap render stage (contrast + brightness + rotation +
    /// dither + palette) on the cached `PreparedSource`. Cancels any
    /// in-flight render so a fast slider drag doesn't queue stale work.
    private func rerender() {
        guard let prepared = preparedSource else { return }
        let pal = selectedPalette
        let c = contrast
        let b = brightness
        let steps = rotationSteps

        renderTask?.cancel()
        renderTask = Task.detached(priority: .userInitiated) {
            let result = WaxwingImageProcessor.render(
                prepared: prepared, palette: pal,
                rotationSteps: steps, contrast: c, brightness: b
            )
            if Task.isCancelled { return }
            await MainActor.run {
                previewImage = result?.image
                pngData = result?.pngData
            }
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

        // Cache the preview image and caption for the grid view, and
        // persist the PNG bytes to the content-addressed disk cache so
        // we won't re-download our own upload on the next reconnect.
        if let img = previewImage {
            imageCache.storeLocal(
                name: name,
                data: data,
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

    /// Inline view that asks for location permission, or nudges the user
    /// to Settings if they previously denied it. Replaces the inert
    /// "Location permission required" caption that used to sit there.
    @ViewBuilder
    private var locationPermissionPrompt: some View {
        switch locationManager.authorizationStatus {
        case .denied, .restricted:
            Button {
                if let url = URL(string: UIApplication.openSettingsURLString) {
                    UIApplication.shared.open(url)
                }
            } label: {
                Text("Location denied — open Settings")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .underline()
            }
            .buttonStyle(.plain)
        default:
            Button {
                locationManager.requestPermission()
            } label: {
                Text("Tap to grant location permission")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .underline()
            }
            .buttonStyle(.plain)
        }
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
