import Foundation

/// Disk cache for arbitrary file bytes pulled from a node.
///
/// Mirrors `WaxwingImageCache`'s storage pattern but stores raw bytes for
/// non-image files (text, binary). Keyed by `NodeFile.cacheKey` so reads
/// and writes line up across sessions even when the firmware doesn't
/// emit real content hashes.
final class WaxwingFileCache {
    static let shared = WaxwingFileCache()

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
        cacheDir = base.appendingPathComponent("WaxwingFiles", isDirectory: true)
        try? fm.createDirectory(at: cacheDir,
                                withIntermediateDirectories: true,
                                attributes: nil)
    }

    // MARK: - Lookup / store

    /// Return cached bytes for `file` if a blob exists at its cache key.
    func data(for file: NodeFile) -> Data? {
        let url = diskURL(forKey: file.cacheKey)
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        return try? Data(contentsOf: url)
    }

    /// Persist freshly-downloaded bytes under the file's cache key.
    /// Reads and writes use the same key so a later `data(for:)` finds it.
    func store(_ data: Data, for file: NodeFile) {
        let url = diskURL(forKey: file.cacheKey)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            print("[fileCache] failed to write \(url.lastPathComponent): \(error)")
        }
    }

    // MARK: - Clear / stats

    func clearAll() {
        let fm = FileManager.default
        if let entries = try? fm.contentsOfDirectory(at: cacheDir,
                                                     includingPropertiesForKeys: nil) {
            for url in entries {
                try? fm.removeItem(at: url)
            }
        }
    }

    func diskFileCount() -> Int {
        let fm = FileManager.default
        return (try? fm.contentsOfDirectory(at: cacheDir,
                                            includingPropertiesForKeys: nil))?.count ?? 0
    }

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

    // MARK: - Internals

    private func diskURL(forKey key: String) -> URL {
        cacheDir.appendingPathComponent("\(key).bin")
    }
}
