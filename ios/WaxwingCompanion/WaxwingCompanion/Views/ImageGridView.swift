import SwiftUI
import Combine

/// Grid view showing Waxwing micro-images (.png files) on the connected node.
/// Images are loaded from the in-memory cache first; any files not yet cached
/// are automatically fetched from the node via chunked BLE read.
struct ImageGridView: View {
    @EnvironmentObject var bleManager: BLEManager

    /// In-memory cache of recently composed images, keyed by filename.
    /// ComposeImageView inserts here on successful upload; remote images
    /// are fetched and inserted automatically.
    @ObservedObject var imageCache: WaxwingImageCache

    /// Currently active palette (for tinting placeholders).
    var palette: WaxwingPalette

    private var imageFiles: [NodeFile] {
        bleManager.fileList.filter { isWaxwingImage($0.name) }
    }

    private let columns = [
        GridItem(.flexible(), spacing: 6),
        GridItem(.flexible(), spacing: 6),
        GridItem(.flexible(), spacing: 6),
    ]

    var body: some View {
        Group {
            if imageFiles.isEmpty {
                emptyState
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 6) {
                        ForEach(imageFiles) { file in
                            gridCell(file)
                                .onAppear {
                                    fetchImageIfNeeded(file)
                                }
                        }
                    }
                    .padding(.horizontal, 6)
                    .padding(.top, 6)
                }
            }
        }
        // When the file list refreshes (e.g. after reconnect), clear any
        // stale fetching/failed states left behind by a dropped BLE
        // connection and re-trigger fetches for uncached images.
        .onChange(of: bleManager.fileList) { _, _ in
            imageCache.resetFetchStates()
            for file in imageFiles {
                fetchImageIfNeeded(file)
            }
        }
    }

    // MARK: - Grid Cell

    private func gridCell(_ file: NodeFile) -> some View {
        VStack(spacing: 0) {
            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color(hex: palette.hexColors[0]))

                if let cached = imageCache.images[file.name] {
                    Image(uiImage: cached)
                        .resizable()
                        .interpolation(.none)
                        .scaledToFit()
                } else if imageCache.isFetching(file.name) {
                    // Currently downloading from node
                    ProgressView()
                        .tint(Color(hex: palette.hexColors[2]))
                } else if imageCache.hasFailed(file.name) {
                    // Download failed — tap to retry
                    VStack(spacing: 4) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.title3)
                            .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.6))
                        Text("Tap to retry")
                            .font(.caption2)
                            .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.4))
                    }
                    .onTapGesture {
                        imageCache.clearFailure(file.name)
                        fetchImageIfNeeded(file)
                    }
                } else {
                    // Placeholder — fetch will start via onAppear
                    Image(systemName: "photo")
                        .font(.title2)
                        .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.5))
                }
            }
            .aspectRatio(1, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 6))

            // Caption from PNG metadata, or formatted filename
            Text(imageCache.caption(for: file.name) ?? displayName(file.name))
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .padding(.top, 3)
                .padding(.horizontal, 2)
        }
    }

    // MARK: - Auto-Fetch

    /// Fetches the image from the node if it's not already cached or in-flight.
    private func fetchImageIfNeeded(_ file: NodeFile) {
        let name = file.name
        guard imageCache.images[name] == nil,
              !imageCache.isFetching(name),
              !imageCache.hasFailed(name) else { return }

        imageCache.markFetching(name)

        bleManager.readFileChunked(name: name) { data in
            guard let data else {
                imageCache.markFailed(name)
                return
            }
            // Try to create a UIImage from the downloaded PNG data
            if let image = UIImage(data: data) {
                // Also extract caption from PNG tEXt metadata
                let caption = PNGMetadata.extractCaption(from: data)
                imageCache.store(name: name, image: image, caption: caption)
            } else {
                imageCache.markFailed(name)
            }
        }
    }

    // MARK: - Empty State

    private var emptyState: some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: "square.grid.3x3")
                .font(.system(size: 36))
                .foregroundStyle(.secondary)
            Text("No Waxwing images yet")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Text("Compose your first micro-image with the + button")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
            Spacer()
        }
        .padding()
    }

    // MARK: - Helpers

    private func isWaxwingImage(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.hasSuffix(".png")
    }

    private func displayName(_ name: String) -> String {
        // Strip "waxwing_" prefix and extension for cleaner display
        var clean = name
        if clean.lowercased().hasPrefix("waxwing_") {
            clean = String(clean.dropFirst(8))
        }
        if let dotIndex = clean.lastIndex(of: ".") {
            clean = String(clean[..<dotIndex])
        }
        // Format timestamp-style names more readably
        if clean.count == 15, clean.contains("_") {
            // "20260404_143022" → "Apr 4, 14:30"
            let parts = clean.split(separator: "_")
            if parts.count == 2, let date = parts.first, let time = parts.last,
               date.count == 8, time.count == 6 {
                let mo = String(date.dropFirst(4).prefix(2))
                let dy = String(date.suffix(2))
                let hr = String(time.prefix(2))
                let mn = String(time.dropFirst(2).prefix(2))
                let months = ["","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"]
                if let moInt = Int(mo), moInt >= 1, moInt <= 12, let dyInt = Int(dy) {
                    return "\(months[moInt]) \(dyInt), \(hr):\(mn)"
                }
            }
        }
        return clean
    }
}

// MARK: - Image Cache

/// Observable cache for Waxwing images, shared between views.
/// Tracks fetch state for each image so the grid can show spinners
/// and retry controls while images are being downloaded from the node.
class WaxwingImageCache: ObservableObject {
    @Published var images: [String: UIImage] = [:]

    /// Captions extracted from PNG tEXt metadata, keyed by filename.
    @Published private var captions: [String: String] = [:]

    /// Filenames currently being downloaded from the node.
    private var fetching: Set<String> = []

    /// Filenames whose download has failed (cleared on retry).
    private var failed: Set<String> = []

    func store(name: String, image: UIImage, caption: String? = nil) {
        fetching.remove(name)
        failed.remove(name)
        images[name] = image
        if let caption, !caption.isEmpty {
            captions[name] = caption
        }
    }

    func caption(for name: String) -> String? {
        captions[name]
    }

    func isFetching(_ name: String) -> Bool {
        fetching.contains(name)
    }

    func hasFailed(_ name: String) -> Bool {
        failed.contains(name)
    }

    func markFetching(_ name: String) {
        fetching.insert(name)
        failed.remove(name)
        objectWillChange.send()
    }

    func markFailed(_ name: String) {
        fetching.remove(name)
        failed.insert(name)
        objectWillChange.send()
    }

    func clearFailure(_ name: String) {
        failed.remove(name)
    }

    /// Clear transient fetch states (fetching + failed) without
    /// discarding already-loaded images or captions.  Call this
    /// after a BLE reconnect so that images whose fetch was
    /// interrupted by a disconnect can be retried.
    func resetFetchStates() {
        let hadStale = !fetching.isEmpty || !failed.isEmpty
        fetching.removeAll()
        failed.removeAll()
        if hadStale {
            objectWillChange.send()
        }
    }

    func clear() {
        images.removeAll()
        captions.removeAll()
        fetching.removeAll()
        failed.removeAll()
    }
}
