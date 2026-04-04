import Foundation

// MARK: - Minimal CBOR Encoder
//
// Encodes the subset of CBOR needed for Waxwing file commands:
//   - Unsigned integers (major type 0)
//   - Text strings (major type 3)
//   - Arrays (major type 4)
//   - Maps with string keys (major type 5)
//   - Simple values: true, false (major type 7)

struct CBOREncoder {

    /// Encode a [String: Any] dictionary as a CBOR map.
    /// Supported value types: String, Int, UInt, Bool, [Any], [String: Any]
    static func encode(_ map: [String: Any]) -> Data {
        var data = Data()
        encodeMap(map, into: &data)
        return data
    }

    // MARK: - Internal

    private static func encodeItem(_ value: Any, into data: inout Data) {
        switch value {
        case let d as Data:
            encodeByteString(d, into: &data)
        case let s as String:
            encodeTextString(s, into: &data)
        case let i as Int:
            if i >= 0 {
                encodeUnsignedInt(UInt64(i), into: &data)
            } else {
                encodeNegativeInt(i, into: &data)
            }
        case let u as UInt:
            encodeUnsignedInt(UInt64(u), into: &data)
        case let u as UInt64:
            encodeUnsignedInt(u, into: &data)
        case let b as Bool:
            data.append(b ? 0xF5 : 0xF4)
        case let arr as [Any]:
            encodeArray(arr, into: &data)
        case let map as [String: Any]:
            encodeMap(map, into: &data)
        default:
            // Encode unknown types as CBOR null
            data.append(0xF6)
        }
    }

    private static func encodeArgumentHeader(majorType: UInt8, value: UInt64, into data: inout Data) {
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
            data.append(UInt8((value >> 24) & 0xFF))
            data.append(UInt8((value >> 16) & 0xFF))
            data.append(UInt8((value >> 8) & 0xFF))
            data.append(UInt8(value & 0xFF))
        } else {
            data.append(mt | 27)
            for shift in stride(from: 56, through: 0, by: -8) {
                data.append(UInt8((value >> shift) & 0xFF))
            }
        }
    }

    private static func encodeUnsignedInt(_ value: UInt64, into data: inout Data) {
        encodeArgumentHeader(majorType: 0, value: value, into: &data)
    }

    private static func encodeNegativeInt(_ value: Int, into data: inout Data) {
        // CBOR negative int: -1 - n, so n = -1 - value
        let n = UInt64(-1 - value)
        encodeArgumentHeader(majorType: 1, value: n, into: &data)
    }

    private static func encodeByteString(_ bytes: Data, into data: inout Data) {
        encodeArgumentHeader(majorType: 2, value: UInt64(bytes.count), into: &data)
        data.append(bytes)
    }

    private static func encodeTextString(_ string: String, into data: inout Data) {
        let utf8 = Array(string.utf8)
        encodeArgumentHeader(majorType: 3, value: UInt64(utf8.count), into: &data)
        data.append(contentsOf: utf8)
    }

    private static func encodeArray(_ array: [Any], into data: inout Data) {
        encodeArgumentHeader(majorType: 4, value: UInt64(array.count), into: &data)
        for item in array {
            encodeItem(item, into: &data)
        }
    }

    private static func encodeMap(_ map: [String: Any], into data: inout Data) {
        encodeArgumentHeader(majorType: 5, value: UInt64(map.count), into: &data)
        // Sort keys for deterministic encoding
        for key in map.keys.sorted() {
            encodeTextString(key, into: &data)
            encodeItem(map[key]!, into: &data)
        }
    }
}
