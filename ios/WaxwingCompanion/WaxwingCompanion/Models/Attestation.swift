import Foundation

/// A location attestation: the user's claim that a given BSSID set was
/// observed at a given geohash cell, signed by the user's content
/// identity. M2 ships the "self" source path; "wigle" / "imported" are
/// reserved for follow-up work.
struct Attestation: Codable, Identifiable, Equatable {
    enum Source: String, Codable, CaseIterable {
        case `self`
        case wigle
        case imported
    }

    let author: Data         // 32 bytes (companion tpk)
    let capturedAt: Date     // unix seconds (truncated for canonical encoding)
    let bssids: [Data]       // top BSSIDs, each 6 bytes
    let geohash: String
    let source: Source
    let signature: Data      // 64 bytes
    let signedBytes: Data    // full CBOR record (round-trippable + verifiable)

    var id: String { signature.map { String(format: "%02x", $0) }.joined() }

    /// Build a signed attestation. `signer` produces a 64-byte Ed25519
    /// signature over the canonical signed body. Throws whatever the
    /// signer throws.
    static func build(author: Data,
                      capturedAt: Date,
                      bssids: [Data],
                      geohash: String,
                      source: Source,
                      signer: (Data) throws -> Data) rethrows -> Attestation {
        let unixSec = UInt64(capturedAt.timeIntervalSince1970)

        // Signed body: map(6) of integer keys 1..6.
        var body = CBORBuilder()
        body.addMapHeader(count: 6)
        body.addUInt(1); body.addUInt(1)
        body.addUInt(2); body.addByteString(author)
        body.addUInt(3); body.addUInt(unixSec)
        body.addUInt(4); body.addArrayHeader(count: UInt64(bssids.count))
        for b in bssids { body.addByteString(b) }
        body.addUInt(5); body.addTextString(geohash)
        body.addUInt(6); body.addTextString(source.rawValue)

        let signature = try signer(body.data)

        // Full record: same fields plus key 7 = signature.
        var full = CBORBuilder()
        full.addMapHeader(count: 7)
        full.addUInt(1); full.addUInt(1)
        full.addUInt(2); full.addByteString(author)
        full.addUInt(3); full.addUInt(unixSec)
        full.addUInt(4); full.addArrayHeader(count: UInt64(bssids.count))
        for b in bssids { full.addByteString(b) }
        full.addUInt(5); full.addTextString(geohash)
        full.addUInt(6); full.addTextString(source.rawValue)
        full.addUInt(7); full.addByteString(signature)

        return Attestation(
            author: author,
            capturedAt: Date(timeIntervalSince1970: TimeInterval(unixSec)),
            bssids: bssids,
            geohash: geohash,
            source: source,
            signature: signature,
            signedBytes: full.data
        )
    }

    /// Parse a signed attestation blob. Used by the round-trip loader and
    /// (later) by the M3 lookup channel.
    static func parse(_ data: Data) -> Attestation? {
        guard let value = try? CBORDecoder.decode(data) else { return nil }
        guard let author    = value.value(forIntKey: 2)?.dataValue, author.count == 32,
              let capturedS = value.value(forIntKey: 3)?.uintValue,
              case .array(let bssidArr)? = value.value(forIntKey: 4),
              let geohash   = value.value(forIntKey: 5)?.stringValue,
              let sourceStr = value.value(forIntKey: 6)?.stringValue,
              let signature = value.value(forIntKey: 7)?.dataValue, signature.count == 64
        else { return nil }
        var bssids: [Data] = []
        bssids.reserveCapacity(bssidArr.count)
        for entry in bssidArr {
            guard let b = entry.dataValue, b.count == 6 else { return nil }
            bssids.append(b)
        }
        return Attestation(
            author: author,
            capturedAt: Date(timeIntervalSince1970: TimeInterval(capturedS)),
            bssids: bssids,
            geohash: geohash,
            source: Source(rawValue: sourceStr) ?? .imported,
            signature: signature,
            signedBytes: data
        )
    }
}
