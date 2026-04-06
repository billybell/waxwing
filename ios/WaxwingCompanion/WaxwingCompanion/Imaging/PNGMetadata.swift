import Foundation

// ============================================================
// PNG tEXt Metadata — Unicode-safe
//
// Embeds and extracts caption strings as PNG tEXt chunks
// (ISO/IEC 15948, Section 11.3.4.3).
//
// A tEXt chunk contains:
//   keyword (1-79 bytes, ASCII) + null separator + text
//
// The PNG spec defines tEXt text as Latin-1, but in practice
// nearly all modern tools (libpng, ImageMagick, macOS Preview,
// ExifTool) read tEXt payloads as UTF-8 — and UTF-8 is a
// superset of ASCII, so the keyword remains valid.
//
// We encode the text portion as UTF-8 so captions in any
// language (CJK, Arabic, emoji, etc.) survive round-trip.
//
// We use the keyword "Comment", one of the predefined PNG text
// keywords, making captions visible in most image viewers.
// ============================================================

enum PNGMetadata {

    /// The PNG tEXt keyword used for Waxwing image captions.
    private static let captionKeyword = "Comment"

    /// Embed a caption into existing PNG data as a tEXt chunk.
    ///
    /// The tEXt chunk is inserted just before the IDAT chunk(s),
    /// which is the conventional location for ancillary chunks.
    ///
    /// - Parameters:
    ///   - pngData: Valid PNG file data.
    ///   - caption: The caption string (max 140 characters).
    /// - Returns: New PNG data with the embedded caption, or the
    ///   original data if the caption is empty or insertion fails.
    static func embedCaption(in pngData: Data, caption: String) -> Data {
        let trimmed = String(caption.prefix(140))
        guard !trimmed.isEmpty else { return pngData }

        // Build the tEXt chunk payload: keyword (ASCII) + \0 + text (UTF-8)
        guard let keywordData = captionKeyword.data(using: .ascii),
              let textData = trimmed.data(using: .utf8) else {
            return pngData
        }

        var payload = Data()
        payload.append(keywordData)
        payload.append(0x00)  // null separator
        payload.append(textData)

        // Build the full chunk: length (4B) + "tEXt" + payload + CRC (4B)
        let chunkType: [UInt8] = [0x74, 0x45, 0x58, 0x74]  // "tEXt"
        var chunk = Data()
        chunk.append(contentsOf: uint32BigEndian(UInt32(payload.count)))
        chunk.append(contentsOf: chunkType)
        chunk.append(payload)

        // CRC covers chunk type + payload
        var crcInput = Data(chunkType)
        crcInput.append(payload)
        let crc = crc32(crcInput)
        chunk.append(contentsOf: uint32BigEndian(crc))

        // Find the first IDAT chunk to insert before it
        guard let insertOffset = findFirstChunk(named: "IDAT", in: pngData) else {
            return pngData
        }

        var result = Data()
        result.append(pngData[0..<insertOffset])
        result.append(chunk)
        result.append(pngData[insertOffset...])
        return result
    }

    /// Extract a caption from PNG data by reading the first tEXt chunk
    /// with the "Comment" keyword.
    ///
    /// - Parameter pngData: Valid PNG file data.
    /// - Returns: The caption string, or nil if none found.
    static func extractCaption(from pngData: Data) -> String? {
        let bytes = [UInt8](pngData)
        guard bytes.count > 8 else { return nil }

        // Skip PNG signature (8 bytes)
        var offset = 8

        while offset + 12 <= bytes.count {
            let length = Int(readUInt32(bytes, at: offset))
            let typeStart = offset + 4
            guard typeStart + 4 <= bytes.count else { break }

            let typeBytes = Array(bytes[typeStart..<typeStart + 4])
            let typeName = String(bytes: typeBytes, encoding: .ascii) ?? ""

            let dataStart = typeStart + 4
            let dataEnd = dataStart + length

            if typeName == "tEXt", dataEnd <= bytes.count {
                let chunkData = Data(bytes[dataStart..<dataEnd])
                // Find null separator between keyword and text
                if let nullIndex = chunkData.firstIndex(of: 0x00) {
                    let keyword = String(data: chunkData[0..<nullIndex], encoding: .ascii)
                    if keyword == captionKeyword {
                        let textStart = chunkData.index(after: nullIndex)
                        // Decode as UTF-8 to support all languages/emoji
                        return String(data: chunkData[textStart...], encoding: .utf8)
                    }
                }
            }

            // Move to next chunk: length(4) + type(4) + data(length) + crc(4)
            offset = dataEnd + 4
        }

        return nil
    }

    // MARK: - Helpers

    /// Find the byte offset of the first chunk with the given 4-char name.
    private static func findFirstChunk(named name: String, in data: Data) -> Int? {
        let bytes = [UInt8](data)
        guard bytes.count > 8 else { return nil }

        var offset = 8  // skip PNG signature

        while offset + 12 <= bytes.count {
            let length = Int(readUInt32(bytes, at: offset))
            let typeStart = offset + 4
            guard typeStart + 4 <= bytes.count else { break }

            let typeBytes = Array(bytes[typeStart..<typeStart + 4])
            let typeName = String(bytes: typeBytes, encoding: .ascii) ?? ""

            if typeName == name {
                return offset
            }

            // Next chunk
            offset = typeStart + 4 + length + 4  // type + data + crc
        }

        return nil
    }

    private static func readUInt32(_ bytes: [UInt8], at offset: Int) -> UInt32 {
        UInt32(bytes[offset]) << 24
        | UInt32(bytes[offset + 1]) << 16
        | UInt32(bytes[offset + 2]) << 8
        | UInt32(bytes[offset + 3])
    }

    private static func uint32BigEndian(_ value: UInt32) -> [UInt8] {
        [UInt8((value >> 24) & 0xFF),
         UInt8((value >> 16) & 0xFF),
         UInt8((value >> 8) & 0xFF),
         UInt8(value & 0xFF)]
    }

    /// Standard CRC-32 used in PNG (ISO 3309 / ITU-T V.42).
    private static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFFFFFF
        for byte in data {
            let index = Int((crc ^ UInt32(byte)) & 0xFF)
            crc = crcTable[index] ^ (crc >> 8)
        }
        return crc ^ 0xFFFFFFFF
    }

    /// Pre-computed CRC-32 lookup table.
    private static let crcTable: [UInt32] = {
        var table = [UInt32](repeating: 0, count: 256)
        for n in 0..<256 {
            var c = UInt32(n)
            for _ in 0..<8 {
                if c & 1 != 0 {
                    c = 0xEDB88320 ^ (c >> 1)
                } else {
                    c = c >> 1
                }
            }
            table[n] = c
        }
        return table
    }()
}
