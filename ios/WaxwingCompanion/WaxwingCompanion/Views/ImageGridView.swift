import SwiftUI
import Combine
import MapKit
import CryptoKit

// String+Identifiable for fullScreenCover(item:) binding
extension String: @retroactive Identifiable {
    public var id: String { self }
}

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

    @State private var selectedFileName: String?

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
                                .onAppear { fetchImageIfNeeded(file) }
                                .contentShape(Rectangle())
                                .onTapGesture {
                                    if imageCache.images[file.name] != nil {
                                        selectedFileName = file.name
                                    } else if imageCache.hasFailed(file.name) {
                                        imageCache.clearFailure(file.name)
                                        fetchImageIfNeeded(file)
                                    }
                                }
                        }
                    }
                    .padding(.horizontal, 6)
                    .padding(.top, 6)
                }
            }
        }
        // Hydrate the in-memory cache from disk for the initial render
        // (handles the case where the manifest is already populated by
        // the time this view appears).
        .onAppear {
            imageCache.applyManifest(imageFiles)
            for file in imageFiles {
                fetchImageIfNeeded(file)
            }
        }
        // When the file list refreshes (e.g. after reconnect), clear any
        // stale fetching/failed states left behind by a dropped BLE
        // connection, hydrate from disk for any newly-known files, and
        // re-trigger fetches for anything still missing.
        .onChange(of: bleManager.fileList) { _, _ in
            imageCache.resetFetchStates()
            imageCache.applyManifest(imageFiles)
            for file in imageFiles {
                fetchImageIfNeeded(file)
            }
        }
        .fullScreenCover(item: $selectedFileName) { name in
            if let img = imageCache.images[name] {
                NavigationStack {
                    ImageDetailView(
                        fileName: name,
                        image: img,
                        caption: imageCache.caption(for: name),
                        palette: palette
                    )
                    .environmentObject(bleManager)
                }
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
                    // Download failed — tap to retry (handled by cell-level onTapGesture)
                    VStack(spacing: 4) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.title3)
                            .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.6))
                        Text("Tap to retry")
                            .font(.caption2)
                            .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.4))
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
    /// Cache lookup uses the manifest hash, so an image whose bytes we already
    /// have on disk (from a previous session, even under a different filename)
    /// is loaded instantly without touching BLE.
    private func fetchImageIfNeeded(_ file: NodeFile) {
        let name = file.name

        // Already in memory? nothing to do.
        if imageCache.images[name] != nil { return }

        // Hydrate from disk if the manifest hash matches a blob we have.
        if imageCache.hasCached(file) {
            imageCache.applyManifest([file])
            if imageCache.images[name] != nil { return }
        }

        guard !imageCache.isFetching(name),
              !imageCache.hasFailed(name) else { return }

        imageCache.markFetching(name)

        bleManager.readFileChunked(name: name) { data in
            guard let data else {
                imageCache.markFailed(name)
                return
            }
            // storeFromBLE persists to the content-addressed disk cache
            // and updates the in-memory dict + caption in one shot.
            imageCache.storeFromBLE(name: name, data: data)
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
///
/// Storage model:
///   * In memory: a `[filename: UIImage]` dict for the currently visible
///     manifest, plus extracted captions.
///   * On disk: each unique image is stored exactly once at
///     `Application Support/WaxwingImages/<sha256-hex>.png`.
///
/// Because the on-disk filename is a content hash, an image survives
/// across disconnects, app restarts, and even rename on the node — as
/// long as the bytes are identical, we already have it.
///
/// Filenames are mapped to their content hash via the manifest the
/// node sends with `list_files`. When `applyManifest` is called we
/// hydrate the in-memory dict from anything we already have on disk
/// and only the truly missing files need to be re-downloaded over BLE.
class WaxwingImageCache: ObservableObject {
    /// Process-wide shared cache.
    static let shared = WaxwingImageCache()

    @Published var images: [String: UIImage] = [:]

    /// Captions extracted from PNG tEXt metadata, keyed by filename.
    @Published private var captions: [String: String] = [:]

    /// filename → content hash (lowercase hex), for files we've seen.
    private var nameToHashHex: [String: String] = [:]

    /// Filenames currently being downloaded from the node.
    private var fetching: Set<String> = []

    /// Filenames whose download has failed (cleared on retry).
    private var failed: Set<String> = []

    /// On-disk directory for content-addressed PNG storage.
    private let cacheDir: URL

    private init() {
        let fm = FileManager.default
        let base: URL
        if let support = try? fm.url(for: .applicationSupportDirectory,
                                     in: .userDomainMask,
                                     appropriateFor: nil,
                                     create: true) {
            base = support
        } else {
            base = fm.temporaryDirectory
        }
        cacheDir = base.appendingPathComponent("WaxwingImages", isDirectory: true)
        try? fm.createDirectory(at: cacheDir,
                                withIntermediateDirectories: true,
                                attributes: nil)
    }

    // MARK: - Disk paths

    /// Number of leading SHA-256 bytes used as the cache key.
    /// MUST match the Pico's `HASH_PREFIX_BYTES` so the hash sent in the
    /// manifest and the hash we compute locally produce the same hex
    /// filename. 8 bytes (64 bits) is plenty for a per-device cache and
    /// keeps the manifest small enough to fit in a single BLE notification.
    private static let hashPrefixBytes = 8

    private func diskURL(forHashHex hex: String) -> URL {
        cacheDir.appendingPathComponent("\(hex).png")
    }

    private func hashHex(of data: Data) -> String {
        let digest = SHA256.hash(data: data)
        return digest.prefix(Self.hashPrefixBytes)
            .map { String(format: "%02x", $0) }
            .joined()
    }

    // MARK: - Manifest hydration

    /// Called whenever the node's file list changes. For every file
    /// whose content hash we already have on disk, populate the
    /// in-memory dict so the grid renders instantly without re-fetching.
    /// Files whose hash we don't recognise will be fetched on demand
    /// by the grid via `fetchImageIfNeeded`.
    func applyManifest(_ files: [NodeFile]) {
        var didLoadAny = false
        for file in files {
            guard let hex = file.hashHex else { continue }
            nameToHashHex[file.name] = hex
            if images[file.name] != nil { continue }
            let url = diskURL(forHashHex: hex)
            guard FileManager.default.fileExists(atPath: url.path),
                  let data = try? Data(contentsOf: url),
                  let img  = UIImage(data: data) else { continue }
            images[file.name] = img
            if let cap = PNGMetadata.extractCaption(from: data),
               !cap.isEmpty {
                captions[file.name] = cap
            }
            didLoadAny = true
        }
        if didLoadAny {
            objectWillChange.send()
        }
    }

    /// Fast check used by the grid before triggering a BLE fetch.
    func hasCached(_ file: NodeFile) -> Bool {
        if images[file.name] != nil { return true }
        if let hex = file.hashHex {
            return FileManager.default.fileExists(atPath: diskURL(forHashHex: hex).path)
        }
        return false
    }

    // MARK: - Stores

    /// Store image bytes received from the node. Persists to the
    /// content-addressed cache so a future reconnect can skip the
    /// download entirely.
    func storeFromBLE(name: String, data: Data) {
        let hex = hashHex(of: data)
        let url = diskURL(forHashHex: hex)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            print("[cache] failed to write \(url.lastPathComponent): \(error)")
        }
        nameToHashHex[name] = hex
        if let img = UIImage(data: data) {
            let cap = PNGMetadata.extractCaption(from: data)
            store(name: name, image: img, caption: cap)
        } else {
            markFailed(name)
        }
    }

    /// Store an image that was just composed locally and uploaded.
    /// We have both the raw PNG bytes and the decoded UIImage.
    func storeLocal(name: String,
                    data: Data,
                    image: UIImage,
                    caption: String? = nil) {
        let hex = hashHex(of: data)
        let url = diskURL(forHashHex: hex)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            print("[cache] failed to write \(url.lastPathComponent): \(error)")
        }
        nameToHashHex[name] = hex
        store(name: name, image: image, caption: caption)
    }

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

    // MARK: - Fetch state

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

    // MARK: - Clear

    /// Clear in-memory state only. Disk cache untouched.
    func clearMemory() {
        images.removeAll()
        captions.removeAll()
        fetching.removeAll()
        failed.removeAll()
        nameToHashHex.removeAll()
    }

    /// Wipe both memory and disk. Used by the Settings "Clear cache"
    /// button for testing.
    func clearAll() {
        clearMemory()
        let fm = FileManager.default
        if let entries = try? fm.contentsOfDirectory(at: cacheDir,
                                                     includingPropertiesForKeys: nil) {
            for url in entries {
                try? fm.removeItem(at: url)
            }
        }
        objectWillChange.send()
    }

    // MARK: - Disk stats (used by Settings)

    /// Number of unique image blobs currently on disk.
    func diskFileCount() -> Int {
        let fm = FileManager.default
        return (try? fm.contentsOfDirectory(at: cacheDir,
                                            includingPropertiesForKeys: nil))?.count ?? 0
    }

    /// Total bytes used on disk by the cache.
    func diskByteCount() -> Int {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: cacheDir,
            includingPropertiesForKeys: [.fileSizeKey]
        ) else { return 0 }
        var total = 0
        for url in entries {
            if let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize {
                total += size
            }
        }
        return total
    }
}
