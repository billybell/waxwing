import Foundation
import CryptoKit
import Combine
import Security

// MARK: - Content Identity
//
// The user's Content Identity per Waxwing PROTOCOL.md §4.2 — an Ed25519
// keypair derived from a BIP-39 mnemonic. This is a *user* identity, not
// a *device* identity: it follows the human across nodes.
//
// Storage strategy:
//   - The mnemonic is stored in the iOS Keychain with the
//     `WhenUnlockedThisDeviceOnly` accessibility attribute, so it cannot be
//     restored from an iCloud backup or migrate to another device.
//   - The public key is mirrored into UserDefaults for fast UI access.
//   - The private key is never persisted directly; it is re-derived from the
//     mnemonic on demand for signing.

final class ContentIdentity: ObservableObject {

    static let shared = ContentIdentity()

    /// The Ed25519 public key (32 bytes), or nil if no identity has been created yet.
    @Published private(set) var publicKey: Data?

    /// The plaintext mnemonic, only populated while the user is actively
    /// viewing their backup phrase. Cleared by `hideMnemonic()`.
    @Published private(set) var revealedMnemonic: [String]?

    private static let mnemonicKeychainAccount = "waxwing.contentIdentity.mnemonic"
    private static let publicKeyDefaultsKey    = "waxwing_contentId_publicKey"

    private init() {
        if let data = UserDefaults.standard.data(forKey: Self.publicKeyDefaultsKey) {
            self.publicKey = data
        }
    }

    // MARK: - Computed

    var hasIdentity: Bool { publicKey != nil }

    /// Hex encoding of the public key for display.
    var publicKeyHex: String? {
        publicKey.map { $0.map { String(format: "%02x", $0) }.joined() }
    }

    /// Base64URL encoding (matches `cpk` field in file metadata schema).
    var publicKeyBase64URL: String? {
        publicKey.map { $0.base64URLEncodedString() }
    }

    /// 8-character fingerprint suitable for compact UI display.
    var fingerprint: String? {
        guard let pk = publicKey else { return nil }
        let hash = SHA256.hash(data: pk)
        return Array(hash).prefix(4).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Lifecycle

    /// Generate a brand-new identity. Returns the freshly generated mnemonic
    /// so the caller can show it to the user for backup.
    @discardableResult
    func generateNew() throws -> [String] {
        let words = try BIP39.generateMnemonic()
        try install(mnemonic: words)
        return words
    }

    /// Restore an identity from a user-supplied mnemonic phrase.
    func restore(mnemonic words: [String]) throws {
        let normalized = words
            .map { $0.lowercased().trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        try BIP39.validate(normalized)
        try install(mnemonic: normalized)
    }

    /// Sign a payload with the current identity. The private key is loaded
    /// from the keychain transiently and not retained.
    func sign(_ payload: Data) throws -> Data {
        let key = try loadPrivateKey()
        return try key.signature(for: payload)
    }

    /// Reveal the mnemonic for backup. Sets `revealedMnemonic`.
    func revealMnemonic() throws {
        let words = try loadMnemonicFromKeychain()
        DispatchQueue.main.async { self.revealedMnemonic = words }
    }

    /// Clear the in-memory revealed mnemonic. Call when the user dismisses
    /// the backup view.
    func hideMnemonic() {
        DispatchQueue.main.async { self.revealedMnemonic = nil }
    }

    /// Wipe the identity entirely (mnemonic + cached public key).
    func wipe() {
        deleteMnemonicFromKeychain()
        UserDefaults.standard.removeObject(forKey: Self.publicKeyDefaultsKey)
        DispatchQueue.main.async {
            self.publicKey = nil
            self.revealedMnemonic = nil
        }
    }

    // MARK: - Internal

    private func install(mnemonic words: [String]) throws {
        let seed     = try BIP39.seed(from: words)
        let keyBytes = seed.prefix(32)
        let key      = try Curve25519.Signing.PrivateKey(rawRepresentation: keyBytes)
        try saveMnemonicToKeychain(words)
        let pk = key.publicKey.rawRepresentation
        UserDefaults.standard.set(pk, forKey: Self.publicKeyDefaultsKey)
        DispatchQueue.main.async { self.publicKey = pk }
    }

    private func loadPrivateKey() throws -> Curve25519.Signing.PrivateKey {
        let words = try loadMnemonicFromKeychain()
        let seed  = try BIP39.seed(from: words)
        return try Curve25519.Signing.PrivateKey(rawRepresentation: seed.prefix(32))
    }

    // MARK: - Keychain

    private func saveMnemonicToKeychain(_ words: [String]) throws {
        let data = Data(words.joined(separator: " ").utf8)
        let baseQuery: [String: Any] = [
            kSecClass            as String: kSecClassGenericPassword,
            kSecAttrAccount      as String: Self.mnemonicKeychainAccount
        ]
        SecItemDelete(baseQuery as CFDictionary)

        var add = baseQuery
        add[kSecValueData      as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly

        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw NSError(domain: "WaxwingKeychain", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Could not save mnemonic to keychain (status \(status))"])
        }
    }

    private func loadMnemonicFromKeychain() throws -> [String] {
        let query: [String: Any] = [
            kSecClass       as String: kSecClassGenericPassword,
            kSecAttrAccount as String: Self.mnemonicKeychainAccount,
            kSecReturnData  as String: true,
            kSecMatchLimit  as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess,
              let data = item as? Data,
              let str = String(data: data, encoding: .utf8) else {
            throw NSError(domain: "WaxwingKeychain", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "Mnemonic not found in keychain (status \(status))"])
        }
        return str.split(separator: " ").map(String.init)
    }

    private func deleteMnemonicFromKeychain() {
        let query: [String: Any] = [
            kSecClass       as String: kSecClassGenericPassword,
            kSecAttrAccount as String: Self.mnemonicKeychainAccount
        ]
        SecItemDelete(query as CFDictionary)
    }
}

// MARK: - Data extensions

extension Data {
    func base64URLEncodedString() -> String {
        base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}
