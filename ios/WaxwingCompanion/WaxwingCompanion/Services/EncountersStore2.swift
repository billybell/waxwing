import Foundation
import Combine

/// Local archive of verified v2 encounter records pulled from any node.
///
/// v2 records are content-addressed by `encounter_id` (sha256 of
/// pub-min || pub-max || nonce_a || nonce_b, truncated to 16 bytes), so
/// the same encounter pulled from either side dedups naturally. The
/// store is a flat list, persisted to `Documents/encounters_v2.json`.
///
/// Sits alongside the v1 EncountersStore during the M4 transition. The
/// MapView reads from both. Once firmware-side v1 is fully retired and
/// existing v1 records age out of devices, the v1 store will be removed.
final class EncountersStore2: ObservableObject {
    static let shared = EncountersStore2()

    @Published private(set) var records: [EncounterRecord2] = []

    private let url: URL

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        let dir  = docs.appendingPathComponent("encounters_v2", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir,
                                                 withIntermediateDirectories: true)
        self.url = dir.appendingPathComponent("records.json")
        if let data = try? Data(contentsOf: url),
           let decoded = try? JSONDecoder().decode([EncounterRecord2].self, from: data) {
            self.records = decoded
        }
    }

    /// Set of encounter ids currently in the store. Used by the BLE
    /// pull path to figure out which records still need fetching.
    var idSet: Set<Data> {
        Set(records.map(\.encounterId))
    }

    /// Insert if novel and signature-verified. Returns true on insert.
    /// Records that fail verification are silently dropped — the firmware
    /// side won't ship invalid records, so a failure here is either a
    /// bug or tampering.
    @discardableResult
    func add(_ record: EncounterRecord2) -> Bool {
        if records.contains(where: { $0.encounterId == record.encounterId }) {
            return false
        }
        guard record.verifyBothSignatures() else {
            print("[EncountersStore2] sig verify failed for \(record.id), dropping")
            return false
        }
        records.append(record)
        save()
        return true
    }

    /// Merge an array of decoded records (dedup + verify). Returns the
    /// number actually added. One save per batch keeps disk I/O flat.
    @discardableResult
    func merge(_ incoming: [EncounterRecord2]) -> Int {
        var existingIds = idSet
        var added = 0
        for rec in incoming where !existingIds.contains(rec.encounterId) {
            guard rec.verifyBothSignatures() else { continue }
            records.append(rec)
            existingIds.insert(rec.encounterId)
            added += 1
        }
        if added > 0 { save() }
        return added
    }

    private func save() {
        do {
            let data = try JSONEncoder().encode(records)
            try data.write(to: url, options: .atomic)
        } catch {
            print("[EncountersStore2] save failed: \(error)")
        }
    }
}
