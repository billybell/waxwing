import SwiftUI
import MapKit

/// Map of all attestations the user has authored. Each pin is at the
/// center of the geohash-7 cell the attestation claims.
///
/// k-anonymity gate: a geohash-6 cell with attestations from ≥3
/// distinct authors is considered "verified" and shows in the default
/// view. Cells below the threshold (typical for solo testing with one
/// or two devices on the same identity) are hidden unless the user
/// flips on "Show under-attested" — useful for confirming that local
/// captures are landing in the store while walking around.
struct MapView: View {
    private static let kAnonAuthorThreshold = 3
    private static let kAnonGeohashPrefix   = 6

    @ObservedObject private var store = AttestationsStore.shared
    @AppStorage("map.showUnderAttested") private var showUnderAttested = true
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
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Toggle(isOn: $showUnderAttested) {
                    Label("Show under-attested",
                          systemImage: "eye.slash")
                }
                .toggleStyle(.button)
            }
        }
        .overlay(alignment: .bottom) {
            statusOverlay
        }
    }

    @ViewBuilder
    private var statusOverlay: some View {
        if store.attestations.isEmpty {
            overlayText("No attestations yet — tap a node and use 'Tag this location' to add one.")
        } else if annotations.isEmpty {
            overlayText("\(store.attestations.count) attestation(s) — none meet the k-anonymity gate. Toggle 'Show under-attested' to see them.")
        } else if showUnderAttested {
            let verified = annotations.filter { !$0.underAttested }.count
            let under    = annotations.count - verified
            overlayText("Showing \(verified) verified + \(under) under-attested")
        }
    }

    private func overlayText(_ s: String) -> some View {
        Text(s)
            .font(.callout)
            .multilineTextAlignment(.center)
            .padding()
            .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 12))
            .padding()
    }

    /// Set of geohash-6 prefixes that pass the k-anon gate.
    private var verifiedGeohashPrefixes: Set<String> {
        var byPrefix: [String: Set<Data>] = [:]
        for att in store.attestations {
            guard att.geohash.count >= Self.kAnonGeohashPrefix else { continue }
            let pre = String(att.geohash.prefix(Self.kAnonGeohashPrefix))
            byPrefix[pre, default: []].insert(att.author)
        }
        return Set(byPrefix.compactMap {
            $0.value.count >= Self.kAnonAuthorThreshold ? $0.key : nil
        })
    }

    private var annotations: [AttestationPin] {
        let verified = verifiedGeohashPrefixes
        return store.attestations.compactMap { att in
            guard let coord = Geohash.decode(att.geohash) else { return nil }
            let pre = String(att.geohash.prefix(Self.kAnonGeohashPrefix))
            let isUnder = !verified.contains(pre)
            if isUnder && !showUnderAttested { return nil }
            return AttestationPin(
                id: att.id,
                coord: CLLocationCoordinate2D(latitude: coord.latitude,
                                              longitude: coord.longitude),
                label: att.geohash,
                systemImage: att.source == .wigle ? "globe" : "wifi",
                tint: tintFor(source: att.source, underAttested: isUnder),
                underAttested: isUnder
            )
        }
    }

    private func tintFor(source: Attestation.Source, underAttested: Bool) -> Color {
        if underAttested { return .gray }
        switch source {
        case .wigle:    return .orange
        case .self, .imported: return .blue
        }
    }
}

private struct AttestationPin: Identifiable {
    let id: String
    let coord: CLLocationCoordinate2D
    let label: String
    let systemImage: String
    let tint: Color
    let underAttested: Bool
}
