import Foundation
import CoreBluetooth
import Combine

// MARK: - Discovered Waxwing Node

class WaxwingNode: Identifiable, ObservableObject {
    let id: UUID  // CoreBluetooth peripheral identifier
    let peripheral: CBPeripheral

    // From advertisement
    @Published var localName: String?
    @Published var rssi: Int
    @Published var lastSeen: Date

    // From Device Identity characteristic (populated after connection)
    @Published var identity: DeviceIdentity?
    @Published var connectionState: ConnectionState = .disconnected

    init(peripheral: CBPeripheral, rssi: Int, localName: String?) {
        self.id = peripheral.identifier
        self.peripheral = peripheral
        self.rssi = rssi
        self.localName = localName
        self.lastSeen = Date()
    }

    /// The "WX:AABBCCDD" fingerprint parsed from local name, if available
    var fingerprint: String? {
        guard let name = localName, name.hasPrefix("WX:") else { return nil }
        return String(name.dropFirst(3))
    }

    /// Display name: node name from identity, or local BLE name, or "Unknown"
    var displayName: String {
        if let name = identity?.name, !name.isEmpty { return name }
        if let name = localName { return name }
        return "Unknown Node"
    }

    /// Signal strength description
    var signalDescription: String {
        switch rssi {
        case -50...0:    return "Excellent"
        case -65..<(-50): return "Good"
        case -80..<(-65): return "Fair"
        default:          return "Weak"
        }
    }
}

// MARK: - Connection State

enum ConnectionState: Equatable {
    case disconnected
    case connecting
    case connected
    case readingIdentity
    case ready
    case failed(String)

    var label: String {
        switch self {
        case .disconnected:     return "Disconnected"
        case .connecting:       return "Connecting..."
        case .connected:        return "Connected"
        case .readingIdentity:  return "Reading identity..."
        case .ready:            return "Ready"
        case .failed(let msg):  return "Error: \(msg)"
        }
    }

    var isConnected: Bool {
        switch self {
        case .connected, .readingIdentity, .ready: return true
        default: return false
        }
    }
}

// MARK: - Node File

/// Represents a file stored on a Waxwing node's flash storage.
struct NodeFile: Identifiable {
    let name: String
    let size: Int

    var id: String { name }

    /// Human-readable file size
    var sizeDescription: String {
        if size < 1024 {
            return "\(size) B"
        } else {
            return String(format: "%.1f KB", Double(size) / 1024.0)
        }
    }
}

// MARK: - Parsed Device Identity

struct DeviceIdentity {
    let protocolVersion: UInt64
    let transportPublicKey: Data
    let name: String?
    let capabilities: WaxwingCapability
    let manifestCount: UInt64
    let attended: Bool
    let unattendedMode: String?
    let protocolName: String?
    let firmware: String?
    let firmwareVersion: String?
    let timestamp: UInt64?

    /// Hex string of full Transport Public Key
    var tpkHex: String {
        transportPublicKey.map { String(format: "%02x", $0) }.joined()
    }

    /// Short fingerprint (first 4 bytes hex, matching BLE local name)
    var fingerprint: String {
        String(tpkHex.prefix(8))
    }

    /// Parse from CBOR data read from Device Identity characteristic
    static func fromCBOR(_ data: Data) -> DeviceIdentity? {
        guard let cbor = try? CBORDecoder.decode(data) else { return nil }

        guard let version = cbor["v"]?.uintValue,
              let tpk = cbor["tpk"]?.dataValue else {
            return nil
        }

        let capsRaw = cbor["caps"]?.uintValue ?? 0
        let caps = WaxwingCapability(rawValue: UInt32(capsRaw))

        return DeviceIdentity(
            protocolVersion: version,
            transportPublicKey: tpk,
            name: cbor["name"]?.stringValue,
            capabilities: caps,
            manifestCount: cbor["manifest_count"]?.uintValue ?? 0,
            attended: cbor["attended"]?.boolValue ?? false,
            unattendedMode: cbor["unattended_mode"]?.stringValue,
            protocolName: cbor["protocol"]?.stringValue,
            firmware: cbor["firmware"]?.stringValue,
            firmwareVersion: cbor["firmware_ver"]?.stringValue,
            timestamp: cbor["timestamp"]?.uintValue
        )
    }
}
