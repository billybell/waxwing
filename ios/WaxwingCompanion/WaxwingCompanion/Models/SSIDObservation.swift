import Foundation

struct SSIDObservation: Codable, Equatable {
    let bssid: Data       // 6 bytes
    let ssid: String
    let rssi: Int
    let channel: Int

    var bssidHex: String {
        bssid.map { String(format: "%02x", $0) }.joined(separator: ":")
    }
}

/// One-shot live scan snapshot returned by the node's `scan_get` command.
struct LiveScan: Equatable {
    let scannedMs: UInt64
    let observations: [SSIDObservation]

    static func fromCBOR(_ value: CBORValue) -> LiveScan? {
        guard let scannedMs = value["scanned_ms"]?.uintValue,
              case .array(let obs)? = value["obs"] else { return nil }
        let parsed: [SSIDObservation] = obs.compactMap { item in
            guard let bssid = item["bssid"]?.dataValue, bssid.count == 6 else { return nil }
            let ssid = item["ssid"]?.stringValue ?? ""
            let rssi = Int(item["rssi"]?.intValue ?? 0)
            let channel = Int(item["channel"]?.uintValue ?? 0)
            return SSIDObservation(bssid: bssid, ssid: ssid, rssi: rssi, channel: channel)
        }
        return LiveScan(scannedMs: scannedMs, observations: parsed)
    }
}
