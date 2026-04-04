import CoreBluetooth

// MARK: - Waxwing BLE UUIDs

enum WaxwingUUID {
    /// Primary service UUID — encodes "WX" + "ING" as ASCII mnemonic
    static let service = CBUUID(string: "CE575800-494E-4700-8000-00805F9B34FB")

    /// Device Identity (READ) — CBOR-encoded node info
    static let deviceIdentity = CBUUID(string: "CE575801-494E-4700-8000-00805F9B34FB")

    /// Manifest Meta (READ, INDICATE) — manifest metadata
    static let manifestMeta = CBUUID(string: "CE575802-494E-4700-8000-00805F9B34FB")

    /// Manifest Chunk (READ) — chunked manifest delivery
    static let manifestChunk = CBUUID(string: "CE575803-494E-4700-8000-00805F9B34FB")

    /// Transfer Request (WRITE) — file request + manifest offset
    static let transferRequest = CBUUID(string: "CE575804-494E-4700-8000-00805F9B34FB")

    /// Transfer Data (NOTIFY) — file chunk streaming
    static let transferData = CBUUID(string: "CE575805-494E-4700-8000-00805F9B34FB")

    /// Transfer ACK (WRITE, NOTIFY) — ACK/NACK/flow control
    static let transferAck = CBUUID(string: "CE575806-494E-4700-8000-00805F9B34FB")

    /// Rating Submission (WRITE) — companion submits review
    static let ratingSubmission = CBUUID(string: "CE575807-494E-4700-8000-00805F9B34FB")

    /// Reputation Exchange (READ, WRITE) — gossip
    static let reputationExchange = CBUUID(string: "CE575808-494E-4700-8000-00805F9B34FB")

    /// WiFi Negotiate (READ, WRITE, NOTIFY) — WiFi upgrade negotiation
    static let wifiNegotiate = CBUUID(string: "CE575809-494E-4700-8000-00805F9B34FB")

    /// Pairing Auth (WRITE, INDICATE) — authenticated companion channel
    static let pairingAuth = CBUUID(string: "CE57580A-494E-4700-8000-00805F9B34FB")

    /// Sync Attestation (WRITE, NOTIFY) — cryptographic proof of sync
    static let syncAttestation = CBUUID(string: "CE57580B-494E-4700-8000-00805F9B34FB")

    /// Encounter Ledger (READ, auth) — per-peer sync stats
    static let encounterLedger = CBUUID(string: "CE57580C-494E-4700-8000-00805F9B34FB")

    /// File Command (WRITE) — companion sends file management commands
    static let fileCommand = CBUUID(string: "CE57580D-494E-4700-8000-00805F9B34FB")

    /// File Response (READ, NOTIFY) — node sends file command responses
    static let fileResponse = CBUUID(string: "CE57580E-494E-4700-8000-00805F9B34FB")

    /// Characteristics we want to discover on connection
    static let phase1Characteristics: [CBUUID] = [
        deviceIdentity,
        fileCommand,
        fileResponse,
    ]
}

// MARK: - Capability Flags

struct WaxwingCapability: OptionSet {
    let rawValue: UInt32

    static let bleTransfer   = WaxwingCapability(rawValue: 0x01)
    static let wifiAP        = WaxwingCapability(rawValue: 0x02)
    static let wifiClient    = WaxwingCapability(rawValue: 0x04)
    static let wifiDirect    = WaxwingCapability(rawValue: 0x08)
    static let gps           = WaxwingCapability(rawValue: 0x10)
    static let storageSD     = WaxwingCapability(rawValue: 0x20)
    static let attended      = WaxwingCapability(rawValue: 0x40)
    static let unattended    = WaxwingCapability(rawValue: 0x80)

    var descriptions: [String] {
        var result: [String] = []
        if contains(.bleTransfer)  { result.append("BLE Transfer") }
        if contains(.wifiAP)       { result.append("WiFi AP") }
        if contains(.wifiClient)   { result.append("WiFi Client") }
        if contains(.wifiDirect)   { result.append("WiFi Direct") }
        if contains(.gps)          { result.append("GPS") }
        if contains(.storageSD)    { result.append("SD Card") }
        if contains(.attended)     { result.append("Attended") }
        if contains(.unattended)   { result.append("Unattended") }
        return result
    }
}

// MARK: - Protocol Version

enum WaxwingProtocol {
    static let version: UInt = 1
    static let name = "waxwing-mesh"
}
