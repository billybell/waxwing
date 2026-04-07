import Foundation
import CoreLocation

// MARK: - Image Metadata
//
// Metadata attached to an uploaded image, stored as a sidecar .meta file
// on the Pico.  Fields are all optional — the user opts in to each one.

struct ImageMetadata {
    let uploader: String?
    let latitude: Double?
    let longitude: Double?
    let timestamp: Date

    var hasLocation: Bool {
        latitude != nil && longitude != nil
    }

    var coordinate: CLLocationCoordinate2D? {
        guard let lat = latitude, let lon = longitude else { return nil }
        return CLLocationCoordinate2D(latitude: lat, longitude: lon)
    }

    // MARK: - CBOR Serialization

    /// Build a [String: Any] dict suitable for CBOREncoder.
    func toCBORDict() -> [String: Any] {
        var dict: [String: Any] = [
            "ts": Int(timestamp.timeIntervalSince1970)
        ]
        if let uploader, !uploader.isEmpty {
            dict["uploader"] = uploader
        }
        if let latitude {
            dict["lat"] = latitude
        }
        if let longitude {
            dict["lon"] = longitude
        }
        return dict
    }

    /// Parse from a CBOR response value.
    static func fromCBOR(_ cbor: CBORValue) -> ImageMetadata? {
        let uploader = cbor["uploader"]?.stringValue
        let lat = cbor["lat"]?.doubleValue
        let lon = cbor["lon"]?.doubleValue
        let tsRaw = cbor["ts"]?.uintValue ?? 0
        let date = tsRaw > 0
            ? Date(timeIntervalSince1970: Double(tsRaw))
            : Date()

        return ImageMetadata(
            uploader: uploader,
            latitude: lat,
            longitude: lon,
            timestamp: date
        )
    }
}
