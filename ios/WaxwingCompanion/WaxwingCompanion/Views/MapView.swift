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
    @ObservedObject private var attStore  = AttestationsStore.shared
    @ObservedObject private var encStore  = EncountersStore.shared
    @ObservedObject private var encStore2 = EncountersStore2.shared
    @EnvironmentObject private var bleManager: BLEManager
    @State private var position: MapCameraPosition = .automatic
    @State private var selection: String?
    @State private var detailPin: EncounterPin?
    @State private var fetchStatus: String?

    var body: some View {
        Map(position: $position, selection: $selection) {
            ForEach(pins) { p in
                Marker(p.label,
                       systemImage: p.systemImage,
                       coordinate: p.coord)
                    .tint(p.tint)
                    .tag(p.id)
            }
        }
        .navigationTitle("Map")
        .task {
            encStore.loadAll()
            requestMissingAttestations()
        }
        .overlay(alignment: .bottom) { statusOverlay }
        .onChange(of: selection) { newId in
            // Open the per-pin detail sheet when a v2 pin is tapped.
            // v1 pins carry no rich payload; ignore them.
            guard let newId,
                  let pin = pins.first(where: { $0.id == newId }),
                  pin.isV2,
                  !pin.records.isEmpty
            else { detailPin = nil; return }
            detailPin = pin
        }
        .sheet(item: $detailPin, onDismiss: { selection = nil }) { pin in
            EncounterDetailSheet(pin: pin)
        }
    }

    @ViewBuilder
    private var statusOverlay: some View {
        let totalV1 = encStore.byNode.values.reduce(0) { $0 + $1.count }
        let totalV2 = encStore2.records.count
        let total   = totalV1 + totalV2
        let matched = pins.reduce(0) { $0 + $1.count }
        let unmatched = total - matched

        if total == 0 {
            overlayText("No encounters pulled from any node yet — open Scans and tap refresh.")
        } else if pins.isEmpty {
            overlayText("\(total) encounter(s) — none have BSSIDs that match a known attestation. Tag more locations to anchor them.")
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

    /// On-demand attestation fetch. Computes the union of BSSIDs across
    /// all loaded encounters that AttestationsStore can't already
    /// resolve, then asks the first connected node for them in batches.
    /// Skipped silently if no node is connected — re-runs when the map
    /// re-appears, which is typically after a connection.
    private func requestMissingAttestations() {
        // First connected node wins. We don't ask multiple nodes for the
        // same BSSIDs since attestations dedup by signature anyway.
        guard let node = bleManager.discoveredNodes.first(where: {
            $0.connectionState == .ready
        }) else { return }

        var missing = Set<Data>()
        for (_, recs) in encStore.byNode {
            for rec in recs {
                for b in rec.bssids where !attStore.hasAttestation(for: b) {
                    missing.insert(b)
                }
            }
        }
        for rec in encStore2.records {
            for b in rec.allBssids where !attStore.hasAttestation(for: b) {
                missing.insert(b)
            }
        }
        if missing.isEmpty { return }

        let bssids = Array(missing)
        fetchStatus = "Fetching \(bssids.count) attestation\(bssids.count == 1 ? "" : "s")…"
        bleManager.requestAttestations(node: node, for: bssids) { added, err in
            DispatchQueue.main.async {
                if let err {
                    fetchStatus = "Fetch error: \(err)"
                } else if added > 0 {
                    fetchStatus = "Added \(added) attestation\(added == 1 ? "" : "s")"
                } else {
                    fetchStatus = nil
                }
            }
        }
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

        // Bucket key: "<encountering node hex>|<geohash>". v2 records
        // contribute one bucket per (node, geohash) pair — both pubA
        // and pubB get pins because the encounter is evidence of co-
        // presence for both sides. v1 records contribute one bucket
        // per (nodePub, geohash) the way they always did.
        struct Bucket {
            let nodePub: Data
            let geohash: String
            let isV2: Bool
            var count: Int = 0
            // v2 records that placed *this* node at this geohash. Lets
            // the detail sheet show per-record lifetime byte counts and
            // meeting counts oriented around `nodePub`. Empty for v1.
            var records: [EncounterRecord2] = []
        }
        var buckets: [String: Bucket] = [:]

        // --- v1 (single-author, soon-to-retire) ---------------------
        // Skipped — v1 records have no tx/rx byte data, so they are invisible under this filter.

        // --- v2 (two-party verifiable) ------------------------------
        // Both bssidsA and bssidsB count toward locating the encounter:
        // one solid match on either list pins both sides at the same
        // cell. We add buckets for pubA AND pubB so the map shows the
        // co-presence event from both perspectives.
        for rec in encStore2.records {
            // Only include encounters where bytes have actually been exchanged
            let totalBytes = rec.txBytesAtoBLifetime
                          + rec.rxBytesAfromBLifetime
                          + rec.txBytesBtoALifetime
                          + rec.rxBytesBfromALifetime
            guard totalBytes > 0 else { continue }

            var hits: [String: Int] = [:]
            for b in rec.allBssids {
                if let g = bssidGeohash[b] { hits[g, default: 0] += 1 }
            }
            guard let best = hits.max(by: { $0.value < $1.value })?.key else { continue }

            for nodePub in [rec.pubA, rec.pubB] {
                let nodeHex = nodePub.map { String(format: "%02x", $0) }.joined()
                let key     = nodeHex + "|" + best
                if buckets[key] == nil {
                    buckets[key] = Bucket(nodePub: nodePub, geohash: best, isV2: true)
                }
                buckets[key]!.count += 1
                buckets[key]!.records.append(rec)
            }
        }

        return buckets.values.compactMap { b -> EncounterPin? in
            guard let coord = Geohash.decode(b.geohash) else { return nil }
            let prefix = b.nodePub.prefix(4).map { String(format: "%02X", $0) }.joined()
            let name   = "WX:\(prefix)"
            // v2 encounters are cryptographic proof of co-presence;
            // the link.circle glyph distinguishes them from v1's wifi
            // self-attestations. Once v1 is fully retired, the
            // distinction goes away.
            let glyph  = b.isV2 ? "link.circle.fill"
                                : "antenna.radiowaves.left.and.right"
            return EncounterPin(
                id: name + "@" + b.geohash + (b.isV2 ? "#v2" : ""),
                coord: CLLocationCoordinate2D(latitude: coord.latitude,
                                              longitude: coord.longitude),
                label: b.count > 1 ? "\(name) ×\(b.count)" : name,
                systemImage: glyph,
                count: b.count,
                tint: tintFor(node: b.nodePub),
                nodePub: b.nodePub,
                geohash: b.geohash,
                isV2: b.isV2,
                records: b.records
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
    let systemImage: String
    let count: Int
    let tint: Color
    let nodePub: Data
    let geohash: String
    let isV2: Bool
    let records: [EncounterRecord2]
}

// MARK: - Detail sheet
//
// Per-pin detail oriented around `pin.nodePub` ("us"). For each v2
// record contributing to the pin, surface the cryptographically signed
// per-side counters that the M4 responder late-bind tweak made
// meaningful on both sides — meeting counts, lifetime tx/rx bytes,
// lifetime file count. The "you ↔ peer" labels flip depending on
// which side of the record `nodePub` is on.
private struct EncounterDetailSheet: View {
    let pin: EncounterPin

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack {
                        Text("Cell")
                        Spacer()
                        Text(pin.geohash).monospaced()
                    }
                    HStack {
                        Text("Node")
                        Spacer()
                        Text(nodeName(pin.nodePub)).monospaced()
                    }
                    HStack {
                        Text("Encounters here")
                        Spacer()
                        Text("\(pin.records.count)")
                    }
                }
                ForEach(pin.records) { rec in
                    Section(header: Text("Encounter \(rec.id.prefix(16))").monospaced()) {
                        recordRows(for: rec)
                    }
                }
            }
            .navigationTitle("Encounter detail")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    @ViewBuilder
    private func recordRows(for rec: EncounterRecord2) -> some View {
        // `nodePub` is one side of the record. Pick the matching side
        // ("us") and orient the lifetime counters around it.
        let weAreA = rec.pubA == pin.nodePub
        let peerPub        = weAreA ? rec.pubB : rec.pubA
        let myMeetingCount = weAreA ? rec.meetingCountA : rec.meetingCountB
        let peerMeetingCount = weAreA ? rec.meetingCountB : rec.meetingCountA
        let myTxLifetime   = weAreA ? rec.txBytesAtoBLifetime  : rec.txBytesBtoALifetime
        let myRxLifetime   = weAreA ? rec.rxBytesAfromBLifetime : rec.rxBytesBfromALifetime
        let myFilesFromPeer = weAreA ? rec.fileCountAfromBLifetime : rec.fileCountBfromALifetime
        let peerTxLifetime  = weAreA ? rec.txBytesBtoALifetime  : rec.txBytesAtoBLifetime
        let peerRxLifetime  = weAreA ? rec.rxBytesBfromALifetime : rec.rxBytesAfromBLifetime
        let peerFilesFromUs = weAreA ? rec.fileCountBfromALifetime : rec.fileCountAfromBLifetime
        let peerRepOfUs    = weAreA ? rec.repOfAbyB : rec.repOfBbyA

        HStack {
            Text("Peer")
            Spacer()
            Text(nodeName(peerPub)).monospaced()
        }
        HStack {
            Text("Meetings (you / peer)")
            Spacer()
            Text("\(myMeetingCount) / \(peerMeetingCount)").monospaced()
        }
        HStack {
            Text("Peer's rep of you")
            Spacer()
            Text("\(peerRepOfUs)").monospaced()
        }
        Group {
            HStack {
                Text("You sent (lifetime)")
                Spacer()
                Text(formatBytes(myTxLifetime)).monospaced()
            }
            HStack {
                Text("You received (lifetime)")
                Spacer()
                Text(formatBytes(myRxLifetime)).monospaced()
            }
            HStack {
                Text("Files you got from peer")
                Spacer()
                Text("\(myFilesFromPeer)").monospaced()
            }
        }
        Group {
            HStack {
                Text("Peer sent (lifetime)")
                Spacer()
                Text(formatBytes(peerTxLifetime)).monospaced()
            }
            HStack {
                Text("Peer received (lifetime)")
                Spacer()
                Text(formatBytes(peerRxLifetime)).monospaced()
            }
            HStack {
                Text("Files peer got from you")
                Spacer()
                Text("\(peerFilesFromUs)").monospaced()
            }
        }
    }

    private func nodeName(_ pub: Data) -> String {
        "WX:" + pub.prefix(4).map { String(format: "%02X", $0) }.joined()
    }

    private func formatBytes(_ n: UInt64) -> String {
        if n < 1024 { return "\(n) B" }
        let kb = Double(n) / 1024.0
        if kb < 1024 { return String(format: "%.1f KB", kb) }
        let mb = kb / 1024.0
        return String(format: "%.1f MB", mb)
    }
}
