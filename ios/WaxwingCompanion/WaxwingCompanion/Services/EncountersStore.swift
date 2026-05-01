import Foundation
import Combine

/// Persistent local archive of encounter records pulled from nodes.
/// Records are keyed by node tpk hex and stored as JSON files in the
/// app's Documents directory, one file per node. Survives app launches
/// and reconnects.
///
/// All access happens on the main queue at runtime (BLEManager's CB
/// delegate runs there, and SwiftUI views read from the same queue).
final class EncountersStore: ObservableObject {
    static let shared = EncountersStore()

    @Published private(set) var byNode: [String: [EncounterRecord]] = [:]

    private let storeDirectory: URL

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        self.storeDirectory = docs.appendingPathComponent("encounters", isDirectory: true)
        try? FileManager.default.createDirectory(at: storeDirectory, withIntermediateDirectories: true)
    }

    private func fileURL(for nodeHex: String) -> URL {
        storeDirectory.appendingPathComponent("\(nodeHex).json")
    }

    /// Load records for a node. Cached after first call.
    @discardableResult
    func load(node nodeHex: String) -> [EncounterRecord] {
        if let cached = byNode[nodeHex] { return cached }
        let url = fileURL(for: nodeHex)
        guard let data = try? Data(contentsOf: url),
              let records = try? JSONDecoder().decode([EncounterRecord].self, from: data) else {
            byNode[nodeHex] = []
            return []
        }
        byNode[nodeHex] = records
        return records
    }

    /// Newest captured_at_ms already stored for the node, or 0 if none.
    /// Used as `since_ms` filter so the firmware skips records we already
    /// have.
    func latestCapturedAtMs(node nodeHex: String) -> UInt64 {
        return load(node: nodeHex).map(\.capturedAtMs).max() ?? 0
    }

    /// Merge new records into the store (dedup by signature). Persists
    /// to disk and publishes the change.
    func merge(node nodeHex: String, records new: [EncounterRecord]) {
        var existing = load(node: nodeHex)
        let existingSigs = Set(existing.map(\.signature))
        var added = 0
        for record in new where !existingSigs.contains(record.signature) {
            existing.append(record)
            added += 1
        }
        guard added > 0 else { return }
        existing.sort { $0.capturedAtMs < $1.capturedAtMs }
        byNode[nodeHex] = existing
        do {
            let data = try JSONEncoder().encode(existing)
            try data.write(to: fileURL(for: nodeHex), options: .atomic)
        } catch {
            print("[EncountersStore] save failed for \(nodeHex): \(error)")
        }
    }
}
