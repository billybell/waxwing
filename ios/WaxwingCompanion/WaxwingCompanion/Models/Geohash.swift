import Foundation

/// Standard geohash (Niemeyer) encode/decode.
/// Geohash-7 ≈ 150m precision, the granularity Waxwing uses for the map.
enum Geohash {
    private static let alphabet = Array("0123456789bcdefghjkmnpqrstuvwxyz")

    static func encode(latitude: Double, longitude: Double, precision: Int = 7) -> String {
        var latRange  = (-90.0, 90.0)
        var lonRange  = (-180.0, 180.0)
        var even      = true   // even bit positions are longitude
        var charBuf   = 0
        var bitsInChar = 0
        var result    = ""
        result.reserveCapacity(precision)
        while result.count < precision {
            if even {
                let mid = (lonRange.0 + lonRange.1) / 2
                if longitude >= mid { charBuf = (charBuf << 1) | 1; lonRange.0 = mid }
                else                { charBuf = charBuf << 1;       lonRange.1 = mid }
            } else {
                let mid = (latRange.0 + latRange.1) / 2
                if latitude >= mid  { charBuf = (charBuf << 1) | 1; latRange.0 = mid }
                else                { charBuf = charBuf << 1;       latRange.1 = mid }
            }
            even.toggle()
            bitsInChar += 1
            if bitsInChar == 5 {
                result.append(alphabet[charBuf])
                charBuf    = 0
                bitsInChar = 0
            }
        }
        return result
    }

    /// Decode the *center* coordinate of the cell named by `hash`.
    static func decode(_ hash: String) -> (latitude: Double, longitude: Double)? {
        var latRange = (-90.0, 90.0)
        var lonRange = (-180.0, 180.0)
        var even = true
        for ch in hash.lowercased() {
            guard let idx = alphabet.firstIndex(of: ch) else { return nil }
            for bit in stride(from: 4, through: 0, by: -1) {
                let isOne = ((idx >> bit) & 1) == 1
                if even {
                    let mid = (lonRange.0 + lonRange.1) / 2
                    if isOne { lonRange.0 = mid } else { lonRange.1 = mid }
                } else {
                    let mid = (latRange.0 + latRange.1) / 2
                    if isOne { latRange.0 = mid } else { latRange.1 = mid }
                }
                even.toggle()
            }
        }
        return ((latRange.0 + latRange.1) / 2, (lonRange.0 + lonRange.1) / 2)
    }
}
