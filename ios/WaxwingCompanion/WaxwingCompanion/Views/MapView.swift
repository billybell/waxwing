import SwiftUI
import MapKit

/// Map of node-to-node encounters.
///
/// An encounter record carries no location of its own — it is a signed
/// list of BSSIDs one node observed at a moment in time. We anchor it
/// to the map by joining its BSSIDs against the attestation store: any
/// attestation (self-authored or pulled from a peer over BLE) whose
/// BSSID list overlaps the encounter's BSSIDs gives us a geohash to
/// pin at. Encounters with no matching BSSID stay invisible — there's
/// nowhere honest to put them.
///
/// Multiple encounters that resolve to the same (encountering node,
/// geohash) cell are coalesced into one pin, so a node that walks
/// past the same Wi-Fi twice doesn't stack markers.
struct MapView: View {
    @ObservedObject private var attStore = AttestationsStore.shared
    @ObservedObject private var encStore = EncountersStore.shared
    @State private var position: MapCameraPosition = .automatic
    @State private var selection: String?

    var body: some View {
        Map(position: $position, selection: $selection) {
            ForEach(pins) { p in
                Marker(p.label,
                       systemImage: "antenna.radiowaves.left.and.right",
                       coordinate: p.coord)
                    .tint(p.tint)
                    .tag(p.id)
            }
        }
        .navigationTitle("Map")
        .task { encStore.loadAll() }
        .overlay(alignment: .bottom) { statusOverlay }
    }

    @ViewBuilder
    private var statusOverlay: some View {
        let totalEncounters = encStore.byNode.values.reduce(0) { $0 + $1.count }
        let matched         = pins.reduce(0) { $0 + $1.count }
        let unmatched       = totalEncounters - matched

        if totalEncounters == 0 {
            overlayText("No encounters pulled from any node yet — open Scans and tap refresh.")
        } else if pins.isEmpty {
            overlayText("\(totalEncounters) encounter(s) — none have BSSIDs that match a known attestation. Tag more locations to anchor them.")
        } else if unmatched > 0 {
            overlayText("\(pins.count) cell(s) plotted • \(unmatched) encounter(s) unmatched")
        } else {
            overlayText("\(pins.count) cell(s) plotted from \(matched) encounter(s)")
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

    private var pins: [EncounterPin] {
        // BSSID → geohash from the most recent attestation that covers
        // that BSSID. Most-recent wins on ties so a moved AP relocates
        // the encounter to its newer location.
        var bssidGeohash: [Data: String] = [:]
        var bssidWhen:    [Data: Date]   = [:]
        for att in attStore.attestations {
            for b in att.bssids {
                if let prev = bssidWhen[b], prev >= att.capturedAt { continue }
                bssidGeohash[b] = att.geohash
                bssidWhen[b]    = att.capturedAt
            }
        }

        // Bucket key: "<encountering node hex>|<geohash>".
        struct Bucket {
            let nodePub: Data
            let geohash: String
            var count: Int = 0
        }
        var buckets: [String: Bucket] = [:]

        for (_, recs) in encStore.byNode {
            for rec in recs {
                // Best-fit geohash for this encounter = geohash that
                // covers the most BSSIDs in the record.
                var hits: [String: Int] = [:]
                for b in rec.bssids {
                    if let g = bssidGeohash[b] { hits[g, default: 0] += 1 }
                }
                guard let best = hits.max(by: { $0.value < $1.value })?.key else { continue }
                let nodeHex = rec.nodePub.map { String(format: "%02x", $0) }.joined()
                let key     = nodeHex + "|" + best
                if buckets[key] == nil {
                    buckets[key] = Bucket(nodePub: rec.nodePub, geohash: best)
                }
                buckets[key]!.count += 1
            }
        }

        return buckets.values.compactMap { b -> EncounterPin? in
            guard let coord = Geohash.decode(b.geohash) else { return nil }
            let prefix = b.nodePub.prefix(4).map { String(format: "%02X", $0) }.joined()
            let name   = "WX:\(prefix)"
            return EncounterPin(
                id: name + "@" + b.geohash,
                coord: CLLocationCoordinate2D(latitude: coord.latitude,
                                              longitude: coord.longitude),
                label: b.count > 1 ? "\(name) ×\(b.count)" : name,
                count: b.count,
                tint: tintFor(node: b.nodePub)
            )
        }
    }

    /// Deterministic per-node hue so two pins from the same node share
    /// a color and different nodes are visually distinct.
    private func tintFor(node: Data) -> Color {
        let bytes = node.prefix(2)
        let h = bytes.reduce(UInt16(0)) { ($0 << 8) | UInt16($1) }
        let hue = Double(h) / Double(UInt16.max)
        return Color(hue: hue, saturation: 0.75, brightness: 0.85)
    }
}

private struct EncounterPin: Identifiable {
    let id: String
    let coord: CLLocationCoordinate2D
    let label: String
    let count: Int
    let tint: Color
}
