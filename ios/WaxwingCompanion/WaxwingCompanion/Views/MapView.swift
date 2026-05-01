import SwiftUI
import MapKit

/// Map of all attestations the user has authored. Each pin is at the
/// center of the geohash-7 cell the attestation claims.
struct MapView: View {
    @ObservedObject private var store = AttestationsStore.shared
    @State private var position: MapCameraPosition = .automatic
    @State private var selection: String?

    var body: some View {
        Map(position: $position, selection: $selection) {
            ForEach(annotations) { ann in
                Marker(ann.label, systemImage: ann.systemImage,
                       coordinate: ann.coord)
                    .tint(ann.tint)
                    .tag(ann.id)
            }
        }
        .navigationTitle("Map")
        .overlay(alignment: .bottom) {
            if annotations.isEmpty {
                Text("No attestations yet — tap a node and use 'Tag this location' to add one.")
                    .font(.callout)
                    .multilineTextAlignment(.center)
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 12))
                    .padding()
            }
        }
    }

    private var annotations: [AttestationPin] {
        store.attestations.compactMap { att in
            guard let coord = Geohash.decode(att.geohash) else { return nil }
            return AttestationPin(
                id: att.id,
                coord: CLLocationCoordinate2D(latitude: coord.latitude,
                                              longitude: coord.longitude),
                label: att.geohash,
                systemImage: att.source == .wigle ? "globe" : "wifi",
                tint: att.source == .wigle ? .orange : .blue
            )
        }
    }
}

private struct AttestationPin: Identifiable {
    let id: String
    let coord: CLLocationCoordinate2D
    let label: String
    let systemImage: String
    let tint: Color
}
