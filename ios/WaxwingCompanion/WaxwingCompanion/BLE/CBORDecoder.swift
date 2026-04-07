import Foundation

// MARK: - Minimal CBOR Decoder
//
// Decodes the subset of CBOR used by Waxwing Device Identity:
//   - Unsigned integers (major type 0)
//   - Negative integers (major type 1)
//   - Byte strings (major type 2)
//   - Text strings (major type 3)
//   - Arrays (major type 4)
//   - Maps (major type 5)
//   - Simple values: true, false, null (major type 7)
//
// Reference: RFC 8949

enum CBORValue: CustomStringConvertible {
    case unsignedInt(UInt64)
    case negativeInt(Int64)
    case byteString(Data)
    case textString(String)
    case array([CBORValue])
    case map([(CBORValue, CBORValue)])
    case float64(Double)
    case boolean(Bool)
    case null

    var description: String {
        switch self {
        case .unsignedInt(let v): return "\(v)"
        case .negativeInt(let v): return "\(v)"
        case .byteString(let d): return "h'\(d.map { String(format: "%02x", $0) }.joined())'"
        case .textString(let s): return "\"\(s)\""
        case .array(let a): return "[\(a.map(\.description).joined(separator: ", "))]"
        case .map(let m): return "{\(m.map { "\($0.0): \($0.1)" }.joined(separator: ", "))}"
        case .float64(let v): return "\(v)"
        case .boolean(let b): return b ? "true" : "false"
        case .null: return "null"
        }
    }

    // MARK: - Convenience accessors

    var stringValue: String? {
        if case .textString(let s) = self { return s }
        return nil
    }

    var uintValue: UInt64? {
        if case .unsignedInt(let v) = self { return v }
        return nil
    }

    var intValue: Int64? {
        switch self {
        case .unsignedInt(let v): return Int64(exactly: v)
        case .negativeInt(let v): return v
        default: return nil
        }
    }

    var doubleValue: Double? {
        switch self {
        case .float64(let v): return v
        case .unsignedInt(let v): return Double(v)
        case .negativeInt(let v): return Double(v)
        default: return nil
        }
    }

    var boolValue: Bool? {
        if case .boolean(let b) = self { return b }
        return nil
    }

    var dataValue: Data? {
        if case .byteString(let d) = self { return d }
        return nil
    }

    /// Access map by string key
    subscript(key: String) -> CBORValue? {
        guard case .map(let pairs) = self else { return nil }
        for (k, v) in pairs {
            if case .textString(let s) = k, s == key {
                return v
            }
        }
        return nil
    }
}

enum CBORError: Error, LocalizedError {
    case unexpectedEnd
    case unsupportedType(UInt8)
    case invalidUTF8
    case reservedAdditionalInfo(UInt8)

    var errorDescription: String? {
        switch self {
        case .unexpectedEnd: return "Unexpected end of CBOR data"
        case .unsupportedType(let t): return "Unsupported CBOR major type: \(t)"
        case .invalidUTF8: return "Invalid UTF-8 in CBOR text string"
        case .reservedAdditionalInfo(let i): return "Reserved CBOR additional info: \(i)"
        }
    }
}

struct CBORDecoder {
    private var data: Data
    private var offset: Int

    init(data: Data) {
        self.data = data
        self.offset = 0
    }

    /// Decode a single CBOR value from the front of the data.
    static func decode(_ data: Data) throws -> CBORValue {
        var decoder = CBORDecoder(data: data)
        return try decoder.decodeItem()
    }

    // MARK: - Internal

    private mutating func readByte() throws -> UInt8 {
        guard offset < data.count else { throw CBORError.unexpectedEnd }
        let byte = data[data.startIndex + offset]
        offset += 1
        return byte
    }

    private mutating func readBytes(_ count: Int) throws -> Data {
        guard offset + count <= data.count else { throw CBORError.unexpectedEnd }
        let start = data.startIndex + offset
        let slice = data[start..<(start + count)]
        offset += count
        return Data(slice)
    }

    /// Read the "argument" (additional info) for a CBOR item header.
    private mutating func readArgument(_ additionalInfo: UInt8) throws -> UInt64 {
        switch additionalInfo {
        case 0...23:
            return UInt64(additionalInfo)
        case 24:
            return UInt64(try readByte())
        case 25:
            let bytes = try readBytes(2)
            return UInt64(bytes[0]) << 8 | UInt64(bytes[1])
        case 26:
            let bytes = try readBytes(4)
            return UInt64(bytes[0]) << 24 | UInt64(bytes[1]) << 16
                 | UInt64(bytes[2]) << 8  | UInt64(bytes[3])
        case 27:
            let bytes = try readBytes(8)
            var value: UInt64 = 0
            for b in bytes { value = value << 8 | UInt64(b) }
            return value
        default:
            throw CBORError.reservedAdditionalInfo(additionalInfo)
        }
    }

    /// Convert IEEE 754 half-precision (binary16) to Float.
    private func halfToFloat(_ half: UInt16) -> Float {
        let sign     = UInt32((half >> 15) & 0x1) << 31
        let exponent = UInt32((half >> 10) & 0x1F)
        let mantissa = UInt32(half & 0x3FF)

        let bits: UInt32
        if exponent == 0 {
            if mantissa == 0 {
                bits = sign // +-zero
            } else {
                // Subnormal: convert to normalized single precision
                var m = mantissa
                var e: UInt32 = 127 - 14
                while m & 0x400 == 0 { m <<= 1; e -= 1 }
                m &= 0x3FF
                bits = sign | (e << 23) | (m << 13)
            }
        } else if exponent == 31 {
            bits = sign | 0x7F800000 | (mantissa << 13) // Inf / NaN
        } else {
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13)
        }
        return Float(bitPattern: bits)
    }

    private mutating func decodeItem() throws -> CBORValue {
        let initial = try readByte()
        let majorType = initial >> 5
        let additionalInfo = initial & 0x1F

        switch majorType {
        case 0: // Unsigned integer
            let value = try readArgument(additionalInfo)
            return .unsignedInt(value)

        case 1: // Negative integer
            let value = try readArgument(additionalInfo)
            return .negativeInt(-1 - Int64(value))

        case 2: // Byte string
            let length = try readArgument(additionalInfo)
            let bytes = try readBytes(Int(length))
            return .byteString(bytes)

        case 3: // Text string
            let length = try readArgument(additionalInfo)
            let bytes = try readBytes(Int(length))
            guard let string = String(data: bytes, encoding: .utf8) else {
                throw CBORError.invalidUTF8
            }
            return .textString(string)

        case 4: // Array
            let count = try readArgument(additionalInfo)
            var items: [CBORValue] = []
            items.reserveCapacity(Int(count))
            for _ in 0..<count {
                items.append(try decodeItem())
            }
            return .array(items)

        case 5: // Map
            let count = try readArgument(additionalInfo)
            var pairs: [(CBORValue, CBORValue)] = []
            pairs.reserveCapacity(Int(count))
            for _ in 0..<count {
                let key = try decodeItem()
                let value = try decodeItem()
                pairs.append((key, value))
            }
            return .map(pairs)

        case 7: // Simple values and floats
            switch additionalInfo {
            case 20: return .boolean(false)
            case 21: return .boolean(true)
            case 22: return .null
            case 25:
                // Half-precision float (IEEE 754 binary16)
                let bytes = try readBytes(2)
                let half = UInt16(bytes[0]) << 8 | UInt16(bytes[1])
                return .float64(Double(halfToFloat(half)))
            case 26:
                // Single-precision float (IEEE 754 binary32)
                let bytes = try readBytes(4)
                let bits = UInt32(bytes[0]) << 24 | UInt32(bytes[1]) << 16
                      | UInt32(bytes[2]) << 8  | UInt32(bytes[3])
                return .float64(Double(Float(bitPattern: bits)))
            case 27:
                // Double-precision float (IEEE 754 binary64)
                let bytes = try readBytes(8)
                var bits: UInt64 = 0
                for b in bytes { bits = bits << 8 | UInt64(b) }
                return .float64(Double(bitPattern: bits))
            default:
                return .null
            }

        default:
            throw CBORError.unsupportedType(majorType)
        }
    }
}
