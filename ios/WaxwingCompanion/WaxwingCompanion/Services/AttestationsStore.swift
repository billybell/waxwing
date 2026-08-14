import Foundation
import Combine

/// Local archive of attestations the user cares about. Two rings:
///
/// - **Self-authored** — what *this* user signed via "Tag this location".
///   Persists to `Documents/attestations/self.json`. Pushed back to the
///   node so peers can pick them up via `attest_query` during peer-sync.
/// - **Peer-received** — attestations pulled from a connected node by the
///   on-demand fetch flow (the map view asks for BSSIDs it can't resolve
///   from local content). Persists to `Documents/attestations/peer.json`.
///
/// The published `attestations` collection unifies both rings so views
/// don't have to filter on provenance. `attestationsByBssid` is the
/// fast lookup index map and pin-resolution code uses.
final class AttestationsStore: ObservableObject {
    static let shared = AttestationsStore()

    @Published private(set) var selfAttestations: [Attestation] = []
    @Published private(set) var peerAttestations: [Attestation] = []

    /// Pre-built `bssid → [Attestation]` index. Rebuilt whenever either
    /// ring mutates. Keeps the map's per-pin lookup O(1) per BSSID.
    @Published private(set) var attestationsByBssid: [Data: [Attestation]] = [:]

    /// Combined view, self-first then peer. Map and detail views read
    /// this; nothing else looks at the source.
    var attestations: [Attestation] { selfAttestations + peerAttestations }

    /// Set of BSSIDs covered by *any* stored attestation, across both
    /// rings. Used by the on-demand fetch path to compute "which BSSIDs
    /// from these encounters do I still need to ask the node about?"
    var coveredBssids: Set<Data> { Set(attestationsByBssid.keys) }

    private let selfURL: URL
    private let peerURL: URL

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        let dir  = docs.appendingPathComponent("attestations", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir,
                                                 withIntermediateDirectories: true)
        self.selfURL = dir.appendingPathComponent("self.json")
        self.peerURL = dir.appendingPathComponent("peer.json")

        self.selfAttestations = Self.load(from: selfURL)
        self.peerAttestations = Self.load(from: peerURL)
        rebuildIndex()
    }

    /// Insert a self-authored attestation (dedup by signature). Persists.
    func add(_ attestation: Attestation) {
        if selfAttestations.contains(where: { $0.signature == attestation.signature }) {
            return
        }
        selfAttestations.append(attestation)
        selfAttestations.sort { $0.capturedAt < $1.capturedAt }
        save(selfAttestations, to: selfURL)
        rebuildIndex()
    }

    /// Merge peer-received attestations (dedup by signature against both
    /// rings). Returns the number actually added.
    @discardableResult
    func mergePeer(_ incoming: [Attestation]) -> Int {
        var existing = Set<Data>()
        existing.formUnion(selfAttestations.map(\.signature))
        existing.formUnion(peerAttestations.map(\.signature))
        var added = 0
        for att in incoming where !existing.contains(att.signature) {
            peerAttestations.append(att)
            existing.insert(att.signature)
            added += 1
        }
        if added > 0 {
            peerAttestations.sort { $0.capturedAt < $1.capturedAt }
            save(peerAttestations, to: peerURL)
            rebuildIndex()
        }
        return added
    }

    /// True if any stored attestation lists this BSSID. Cheaper than
    /// computing `coveredBssids` when called in a loop.
    func hasAttestation(for bssid: Data) -> Bool {
        attestationsByBssid[bssid] != nil
    }

    private func rebuildIndex() {
        var idx: [Data: [Attestation]] = [:]
        for att in attestations {
            for b in att.bssids {
                idx[b, default: []].append(att)
            }
        }
        attestationsByBssid = idx
    }

    private static func load(from url: URL) -> [Attestation] {
        guard let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([Attestation].self, from: data)
        else { return [] }
        return decoded.sorted { $0.capturedAt < $1.capturedAt }
    }

    private func save(_ atts: [Attestation], to url: URL) {
        do {
            let data = try JSONEncoder().encode(atts)
            try data.write(to: url, options: .atomic)
        } catch {
            print("[AttestationsStore] save failed: \(error)")
        }
    }
}
