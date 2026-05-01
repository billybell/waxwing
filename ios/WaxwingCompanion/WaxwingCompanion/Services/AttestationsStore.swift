import Foundation
import Combine

/// Local archive of attestations the user has authored. Persists to
/// `Documents/attestations/self.json`. The map view reads from here.
final class AttestationsStore: ObservableObject {
    static let shared = AttestationsStore()

    @Published private(set) var attestations: [Attestation] = []

    private let url: URL

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        let dir  = docs.appendingPathComponent("attestations", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir,
                                                 withIntermediateDirectories: true)
        self.url = dir.appendingPathComponent("self.json")
        if let data = try? Data(contentsOf: url),
           let decoded = try? JSONDecoder().decode([Attestation].self, from: data) {
            self.attestations = decoded.sorted { $0.capturedAt < $1.capturedAt }
        }
    }

    /// Insert (dedup by signature). Persists.
    func add(_ attestation: Attestation) {
        if attestations.contains(where: { $0.signature == attestation.signature }) {
            return
        }
        attestations.append(attestation)
        attestations.sort { $0.capturedAt < $1.capturedAt }
        save()
    }

    private func save() {
        do {
            let data = try JSONEncoder().encode(attestations)
            try data.write(to: url, options: .atomic)
        } catch {
            print("[AttestationsStore] save failed: \(error)")
        }
    }
}
