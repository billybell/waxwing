import Foundation

/// A signed encounter record retrieved from a Waxwing node. The full
/// signed CBOR bytes are preserved (`signedBytes`) so iOS can re-verify
/// the Ed25519 signature against the node's published transport pub key.
struct EncounterRecord: Codable, Identifiable, Equatable {
    let nodePub: Data         // 32 bytes
    let capturedAtMs: UInt64
    let bssids: [Data]        // each 6 bytes
    let signature: Data       // 64 bytes
    let signedBytes: Data     // full CBOR record (round-tripped for re-verify)

    var id: String { signature.map { String(format: "%02x", $0) }.joined() }

    /// Parse the wire-format encounter record. The firmware encodes a
    /// CBOR map with integer keys (1=v, 2=node, 3=captured_at_ms, 4=bssids,
    /// 5=sig). Returns nil on shape mismatch.
    static func parse(_ data: Data) -> EncounterRecord? {
        guard let value = try? CBORDecoder.decode(data) else { return nil }
        guard let nodePub = value.value(forIntKey: 2)?.dataValue,
              nodePub.count == 32,
              let capturedAtMs = value.value(forIntKey: 3)?.uintValue,
              case .array(let bssidArr)? = value.value(forIntKey: 4),
              let signature = value.value(forIntKey: 5)?.dataValue,
              signature.count == 64 else { return nil }
        var bssids: [Data] = []
        bssids.reserveCapacity(bssidArr.count)
        for entry in bssidArr {
            guard let bs = entry.dataValue, bs.count == 6 else { return nil }
            bssids.append(bs)
        }
        return EncounterRecord(
            nodePub: nodePub,
            capturedAtMs: capturedAtMs,
            bssids: bssids,
            signature: signature,
            signedBytes: data
        )
    }
}
