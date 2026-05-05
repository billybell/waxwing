import Foundation

/// Minimal CBOR writer for hand-rolled records that don't fit the
/// `CBOREncoder.encode([String: Any])` shape — specifically attestations,
/// which key fields by small unsigned integers rather than strings.
struct CBORBuilder {
    private(set) var data = Data()

    mutating func addMapHeader(count: UInt64)   { writeArg(majorType: 5, value: count) }
    mutating func addArrayHeader(count: UInt64) { writeArg(majorType: 4, value: count) }
    mutating func addUInt(_ value: UInt64)      { writeArg(majorType: 0, value: value) }

    /// CBOR signed integer (RFC 8949 §3.1). Positive → major type 0;
    /// negative → major type 1 with arg = -1 - value. Used by the v2
    /// encounter record's reputation fields, which can be negative.
    mutating func addInt(_ value: Int64) {
        if value >= 0 {
            writeArg(majorType: 0, value: UInt64(value))
        } else {
            writeArg(majorType: 1, value: UInt64(-(value + 1)))
        }
    }

    mutating func addByteString(_ bytes: Data) {
        writeArg(majorType: 2, value: UInt64(bytes.count))
        data.append(bytes)
    }

    mutating func addTextString(_ string: String) {
        let bytes = Data(string.utf8)
        writeArg(majorType: 3, value: UInt64(bytes.count))
        data.append(bytes)
    }

    private mutating func writeArg(majorType: UInt8, value: UInt64) {
        let mt = majorType << 5
        if value <= 23 {
            data.append(mt | UInt8(value))
        } else if value <= 0xFF {
            data.append(mt | 24)
            data.append(UInt8(value))
        } else if value <= 0xFFFF {
            data.append(mt | 25)
            data.append(UInt8(value >> 8))
            data.append(UInt8(value & 0xFF))
        } else if value <= 0xFFFF_FFFF {
            data.append(mt | 26)
            for shift in stride(from: 24, through: 0, by: -8) {
                data.append(UInt8((value >> shift) & 0xFF))
            }
        } else {
            data.append(mt | 27)
            for shift in stride(from: 56, through: 0, by: -8) {
                data.append(UInt8((value >> shift) & 0xFF))
            }
        }
    }
}
