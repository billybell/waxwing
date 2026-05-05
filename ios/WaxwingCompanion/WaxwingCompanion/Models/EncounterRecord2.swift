import Foundation
import CryptoKit

// MARK: - Verifiable two-party encounter record (M4 v2 schema)
//
// Mirror of `firmware/core/encounter_record.h`. A v2 record is a CBOR
// map with integer keys 1..19; both parties sign the canonical body
// (keys 1..17) before persisting. iOS reads these from the firmware's
// `/files/encounters/<id>.cbor` files (M4 stage 6+) and verifies both
// signatures against the parties' Ed25519 transport public keys.
//
// Coexists with the v1 EncounterRecord (single-author, single-sig)
// during the M4 transition. Once the firmware stops creating v1 records
// (stage 9 — already shipped) and the device-side stores have aged out,
// v1 will be removed.

struct EncounterRecord2: Codable, Identifiable, Equatable {
    static let recordVersion: UInt8 = 1
    static let bssidMax: Int        = 8
    static let nonceBytes: Int      = 16
    static let pubBytes: Int        = 32
    static let sigBytes: Int        = 64
    static let idBytes: Int         = 16

    let version: UInt8

    let pubA: Data       // 32 bytes
    let pubB: Data       // 32 bytes

    let meetingCountA: UInt64
    let meetingCountB: UInt64

    let repOfBbyA: Int32
    let repOfAbyB: Int32

    let bssidsA: [Data]  // each 6 bytes; 0..bssidMax entries
    let bssidsB: [Data]

    let nonceA: Data     // 16 bytes
    let nonceB: Data     // 16 bytes

    let txBytesAtoBLifetime: UInt64
    let rxBytesAfromBLifetime: UInt64
    let txBytesBtoALifetime: UInt64
    let rxBytesBfromALifetime: UInt64

    let fileCountAfromBLifetime: UInt64
    let fileCountBfromALifetime: UInt64

    let sigA: Data       // 64 bytes
    let sigB: Data       // 64 bytes

    /// Bytes of the original signed CBOR record. Kept verbatim so iOS
    /// can re-verify both signatures or rebuild the encounter id without
    /// re-encoding (and risking diverging from the firmware's bytes).
    let signedBytes: Data

    // MARK: - Identity

    /// Hex of the encounter id (16 bytes). Stable across both sides.
    var id: String { encounterId.map { String(format: "%02x", $0) }.joined() }

    /// Side-symmetric content-addressed encounter id, identical to what
    /// firmware computes:
    ///   sha256(min(pub_a, pub_b) || max(pub_a, pub_b) || nonce_a || nonce_b)[:16]
    var encounterId: Data {
        let lo: Data
        let hi: Data
        if pubA.lexicographicallyPrecedes(pubB) {
            lo = pubA; hi = pubB
        } else {
            lo = pubB; hi = pubA
        }
        var h = SHA256()
        h.update(data: lo)
        h.update(data: hi)
        h.update(data: nonceA)
        h.update(data: nonceB)
        let digest = h.finalize()
        return Data(digest.prefix(Self.idBytes))
    }

    /// Display labels — `WX:AABBCCDD` style truncated TPK.
    var nodeNameA: String { "WX:" + pubA.prefix(4).map { String(format: "%02X", $0) }.joined() }
    var nodeNameB: String { "WX:" + pubB.prefix(4).map { String(format: "%02X", $0) }.joined() }

    /// Union of both sides' BSSIDs (deduplicated). Used by MapView to
    /// join against the attestation store.
    var allBssids: [Data] {
        var seen = Set<Data>()
        var out: [Data] = []
        for b in bssidsA where seen.insert(b).inserted { out.append(b) }
        for b in bssidsB where seen.insert(b).inserted { out.append(b) }
        return out
    }

    // MARK: - Parse

    /// Parse a CBOR-encoded full record. Returns nil on shape mismatch.
    /// Signatures are NOT verified here — call `verifyBothSignatures()`
    /// explicitly before trusting the contents.
    static func parse(_ data: Data) -> EncounterRecord2? {
        guard let value = try? CBORDecoder.decode(data) else { return nil }

        guard let v = value.value(forIntKey: 1)?.uintValue,
              v == UInt64(recordVersion),
              let pubA       = value.value(forIntKey: 2)?.dataValue, pubA.count == pubBytes,
              let pubB       = value.value(forIntKey: 3)?.dataValue, pubB.count == pubBytes,
              let mcA        = value.value(forIntKey: 4)?.uintValue,
              let mcB        = value.value(forIntKey: 5)?.uintValue,
              let repBA      = value.value(forIntKey: 6)?.intValue,
              let repAB      = value.value(forIntKey: 7)?.intValue,
              case .array(let bssidsAArr)? = value.value(forIntKey: 8),
              case .array(let bssidsBArr)? = value.value(forIntKey: 9),
              let nonceA     = value.value(forIntKey: 10)?.dataValue, nonceA.count == nonceBytes,
              let nonceB     = value.value(forIntKey: 11)?.dataValue, nonceB.count == nonceBytes,
              let txAB       = value.value(forIntKey: 12)?.uintValue,
              let rxAB       = value.value(forIntKey: 13)?.uintValue,
              let txBA       = value.value(forIntKey: 14)?.uintValue,
              let rxBA       = value.value(forIntKey: 15)?.uintValue,
              let fileAB     = value.value(forIntKey: 16)?.uintValue,
              let fileBA     = value.value(forIntKey: 17)?.uintValue,
              let sigA       = value.value(forIntKey: 18)?.dataValue, sigA.count == sigBytes,
              let sigB       = value.value(forIntKey: 19)?.dataValue, sigB.count == sigBytes
        else { return nil }

        guard bssidsAArr.count <= bssidMax, bssidsBArr.count <= bssidMax else { return nil }

        var bssidsA: [Data] = []
        bssidsA.reserveCapacity(bssidsAArr.count)
        for entry in bssidsAArr {
            guard let b = entry.dataValue, b.count == 6 else { return nil }
            bssidsA.append(b)
        }
        var bssidsB: [Data] = []
        bssidsB.reserveCapacity(bssidsBArr.count)
        for entry in bssidsBArr {
            guard let b = entry.dataValue, b.count == 6 else { return nil }
            bssidsB.append(b)
        }

        guard let repBA32 = Int32(exactly: repBA),
              let repAB32 = Int32(exactly: repAB) else { return nil }

        return EncounterRecord2(
            version: UInt8(v),
            pubA: pubA, pubB: pubB,
            meetingCountA: mcA, meetingCountB: mcB,
            repOfBbyA: repBA32, repOfAbyB: repAB32,
            bssidsA: bssidsA, bssidsB: bssidsB,
            nonceA: nonceA, nonceB: nonceB,
            txBytesAtoBLifetime:    txAB,
            rxBytesAfromBLifetime:  rxAB,
            txBytesBtoALifetime:    txBA,
            rxBytesBfromALifetime:  rxBA,
            fileCountAfromBLifetime: fileAB,
            fileCountBfromALifetime: fileBA,
            sigA: sigA, sigB: sigB,
            signedBytes: data
        )
    }

    // MARK: - Verify

    /// Reconstruct the canonical signed body (CBOR map with keys 1..17)
    /// and verify each signature against its respective public key.
    ///
    /// Returns true iff sig_a is valid for pub_a AND sig_b is valid for
    /// pub_b. Either failure is a hard reject — the firmware emits
    /// records with both sigs set, so a valid record always has both.
    func verifyBothSignatures() -> Bool {
        var body = CBORBuilder()
        body.addMapHeader(count: 17)
        body.addUInt(1);  body.addUInt(UInt64(version))
        body.addUInt(2);  body.addByteString(pubA)
        body.addUInt(3);  body.addByteString(pubB)
        body.addUInt(4);  body.addUInt(meetingCountA)
        body.addUInt(5);  body.addUInt(meetingCountB)
        body.addUInt(6);  body.addInt(Int64(repOfBbyA))
        body.addUInt(7);  body.addInt(Int64(repOfAbyB))
        body.addUInt(8);  body.addArrayHeader(count: UInt64(bssidsA.count))
        for b in bssidsA { body.addByteString(b) }
        body.addUInt(9);  body.addArrayHeader(count: UInt64(bssidsB.count))
        for b in bssidsB { body.addByteString(b) }
        body.addUInt(10); body.addByteString(nonceA)
        body.addUInt(11); body.addByteString(nonceB)
        body.addUInt(12); body.addUInt(txBytesAtoBLifetime)
        body.addUInt(13); body.addUInt(rxBytesAfromBLifetime)
        body.addUInt(14); body.addUInt(txBytesBtoALifetime)
        body.addUInt(15); body.addUInt(rxBytesBfromALifetime)
        body.addUInt(16); body.addUInt(fileCountAfromBLifetime)
        body.addUInt(17); body.addUInt(fileCountBfromALifetime)

        let bodyBytes = body.data
        return Self.verify(sig: sigA, pub: pubA, message: bodyBytes)
            && Self.verify(sig: sigB, pub: pubB, message: bodyBytes)
    }

    private static func verify(sig: Data, pub: Data, message: Data) -> Bool {
        guard let pubKey = try? Curve25519.Signing.PublicKey(rawRepresentation: pub) else {
            return false
        }
        return pubKey.isValidSignature(sig, for: message)
    }
}

private extension Data {
    func lexicographicallyPrecedes(_ other: Data) -> Bool {
        let n = Swift.min(count, other.count)
        for i in 0..<n {
            let a = self[startIndex + i]
            let b = other[other.startIndex + i]
            if a < b { return true }
            if a > b { return false }
        }
        return count < other.count
    }
}
