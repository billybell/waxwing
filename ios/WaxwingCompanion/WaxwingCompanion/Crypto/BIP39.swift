import Foundation
import CryptoKit
import CommonCrypto
import Security

// MARK: - BIP-39 Mnemonic
//
// Per Waxwing PROTOCOL.md §4.3, Content Identity keys are derived from a
// BIP-39 mnemonic phrase using the standard derivation:
//
//     seed     = PBKDF2-HMAC-SHA512("mnemonic" + passphrase, mnemonic, 2048, 64)
//     keypair  = Ed25519 key from seed[0:32]
//
// The 2048-word English wordlist is loaded from a bundled `bip39-english.txt`
// resource. If you ship this app you MUST drop the official wordlist into the
// app bundle — see Crypto/Resources/README.md for instructions.

enum BIP39Error: LocalizedError {
    case wordlistMissing
    case invalidEntropy
    case invalidMnemonic
    case checksumMismatch
    case unknownWord(String)

    var errorDescription: String? {
        switch self {
        case .wordlistMissing:
            return "BIP-39 wordlist is missing from the app bundle. Add bip39-english.txt to Resources."
        case .invalidEntropy:    return "Invalid entropy length."
        case .invalidMnemonic:   return "Invalid mnemonic phrase."
        case .checksumMismatch:  return "Mnemonic checksum is invalid — please double-check the words."
        case .unknownWord(let w): return "“\(w)” is not in the BIP-39 word list."
        }
    }
}

enum BIP39 {

    /// Standard 2048-word English wordlist. Loaded once from
    /// `bip39-english.txt` in the main bundle.
    static let wordlist: [String] = {
        guard let url = Bundle.main.url(forResource: "bip39-english", withExtension: "txt"),
              let text = try? String(contentsOf: url, encoding: .utf8) else {
            return []
        }
        return text
            .split(whereSeparator: { $0.isNewline })
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }()

    static var isWordlistLoaded: Bool { wordlist.count == 2048 }

    // MARK: - Generation

    /// Generate a fresh 12-word mnemonic from 128 bits of system entropy.
    static func generateMnemonic() throws -> [String] {
        var entropy = Data(count: 16)
        let result = entropy.withUnsafeMutableBytes { ptr -> Int32 in
            guard let base = ptr.baseAddress else { return errSecParam }
            return SecRandomCopyBytes(kSecRandomDefault, 16, base)
        }
        guard result == errSecSuccess else { throw BIP39Error.invalidEntropy }
        return try mnemonic(from: entropy)
    }

    /// Convert raw entropy to a BIP-39 mnemonic word list.
    static func mnemonic(from entropy: Data) throws -> [String] {
        guard isWordlistLoaded else { throw BIP39Error.wordlistMissing }
        guard entropy.count == 16 || entropy.count == 32 else { throw BIP39Error.invalidEntropy }

        // Build a bitstring of entropy + checksum.
        var bits = ""
        for byte in entropy {
            bits += String(byte, radix: 2).leftPadded(to: 8, with: "0")
        }
        let checksumBitLen = entropy.count * 8 / 32     // 4 bits for 128-bit, 8 bits for 256-bit
        let hash = SHA256.hash(data: entropy)
        let checksumByte = Array(hash)[0]
        let csBits = String(checksumByte, radix: 2).leftPadded(to: 8, with: "0")
        bits += String(csBits.prefix(checksumBitLen))

        // Split into 11-bit groups → indices into the wordlist.
        var words: [String] = []
        var i = bits.startIndex
        while i < bits.endIndex {
            let end = bits.index(i, offsetBy: 11)
            let chunk = String(bits[i..<end])
            guard let idx = Int(chunk, radix: 2), idx < wordlist.count else {
                throw BIP39Error.invalidEntropy
            }
            words.append(wordlist[idx])
            i = end
        }
        return words
    }

    // MARK: - Validation

    /// Validate a user-supplied mnemonic phrase against the BIP-39 checksum.
    static func validate(_ words: [String]) throws {
        guard isWordlistLoaded else { throw BIP39Error.wordlistMissing }
        guard [12, 15, 18, 21, 24].contains(words.count) else { throw BIP39Error.invalidMnemonic }

        var bits = ""
        for w in words {
            guard let idx = wordlist.firstIndex(of: w.lowercased()) else {
                throw BIP39Error.unknownWord(w)
            }
            bits += String(idx, radix: 2).leftPadded(to: 11, with: "0")
        }

        let totalBits     = words.count * 11
        let entropyBits   = totalBits * 32 / 33
        let checksumBits  = totalBits - entropyBits
        let entropyStr    = String(bits.prefix(entropyBits))
        let checksumStr   = String(bits.suffix(checksumBits))

        // Reconstruct entropy bytes from the bitstring.
        var entropyBytes = [UInt8]()
        var idx = entropyStr.startIndex
        while idx < entropyStr.endIndex {
            let end = entropyStr.index(idx, offsetBy: 8)
            entropyBytes.append(UInt8(entropyStr[idx..<end], radix: 2) ?? 0)
            idx = end
        }
        let hash = SHA256.hash(data: Data(entropyBytes))
        let csByte = Array(hash)[0]
        let expected = String(csByte, radix: 2).leftPadded(to: 8, with: "0").prefix(checksumBits)

        if checksumStr != expected { throw BIP39Error.checksumMismatch }
    }

    // MARK: - Seed derivation

    /// PBKDF2-HMAC-SHA512 of the mnemonic to produce a 64-byte seed,
    /// per BIP-39 §"From mnemonic to seed".
    static func seed(from words: [String], passphrase: String = "") throws -> Data {
        let mnemonic = words.joined(separator: " ")
        let salt = "mnemonic" + passphrase
        guard let mnemonicData = mnemonic.data(using: .utf8),
              let saltData = salt.data(using: .utf8) else {
            throw BIP39Error.invalidMnemonic
        }
        var derived = Data(count: 64)
        let status = derived.withUnsafeMutableBytes { dPtr -> Int32 in
            saltData.withUnsafeBytes { sPtr in
                mnemonicData.withUnsafeBytes { mPtr in
                    CCKeyDerivationPBKDF(
                        CCPBKDFAlgorithm(kCCPBKDF2),
                        mPtr.baseAddress?.assumingMemoryBound(to: Int8.self),
                        mnemonicData.count,
                        sPtr.baseAddress?.assumingMemoryBound(to: UInt8.self),
                        saltData.count,
                        CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA512),
                        2048,
                        dPtr.baseAddress?.assumingMemoryBound(to: UInt8.self),
                        64
                    )
                }
            }
        }
        guard status == kCCSuccess else { throw BIP39Error.invalidMnemonic }
        return derived
    }
}

// MARK: - String padding helper

private extension String {
    func leftPadded(to length: Int, with pad: Character) -> String {
        if count >= length { return self }
        return String(repeating: pad, count: length - count) + self
    }
}
