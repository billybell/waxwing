import SwiftUI
import CoreLocation

/// Shows the node's most recent live SSID scan and the encounter records
/// pulled into the local store. Also exposes the M2 attestation flow:
/// "Tag this location" reads the live scan + GPS, signs an attestation,
/// writes it back to the node, and adds it to the local map index.
struct EncountersView: View {
    @ObservedObject var node: WaxwingNode
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject private var store = EncountersStore.shared
    @ObservedObject private var attestations = AttestationsStore.shared
    @ObservedObject private var locationManager = LocationManager.shared
    @ObservedObject private var contentIdentity = ContentIdentity.shared

    @State private var pulling = false
    @State private var tagging = false
    @State private var tagStatus: String?
    @State private var lastError: String?
    @State private var pullCount: Int = 0
    @State private var liveScan: LiveScan?
    @State private var attestSyncStatus: String?

    private var nodeHex: String { node.identity?.tpkHex ?? "" }
    private var records: [EncounterRecord] {
        store.byNode[nodeHex] ?? store.load(node: nodeHex)
    }

    var body: some View {
        List {
            liveSection
            attestSection
            encountersSection
            if let lastError {
                Section {
                    Text(lastError).foregroundColor(.red).font(.callout)
                }
            }
        }
        .navigationTitle("Scans")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    refresh()
                } label: {
                    if pulling { ProgressView() } else { Image(systemName: "arrow.clockwise") }
                }
                .disabled(pulling || node.connectionState != .ready)
            }
        }
        .task {
            _ = store.load(node: nodeHex)
            if node.connectionState == .ready {
                refresh()
            }
        }
    }

    private var liveSection: some View {
        Section("Live Scan") {
            if let live = liveScan {
                HStack {
                    Text("scanned_ms").foregroundStyle(.secondary)
                    Spacer()
                    Text("\(live.scannedMs)").monospaced()
                }
                .font(.caption)
                if live.observations.isEmpty {
                    Text("No observations").foregroundColor(.secondary)
                } else {
                    ForEach(Array(live.observations.enumerated()), id: \.offset) { _, obs in
                        observationRow(obs)
                    }
                }
            } else {
                Text("Tap refresh to fetch the latest scan from the node.")
                    .foregroundColor(.secondary)
                    .font(.callout)
            }
        }
    }

    private var attestSection: some View {
        Section("Attest This Location") {
            if !contentIdentity.hasIdentity {
                Text("Onboard a content identity first (Settings → Identity).")
                    .font(.callout).foregroundColor(.secondary)
            } else if !locationManager.isAuthorized {
                Button {
                    locationManager.requestPermission()
                } label: {
                    Label("Grant location access", systemImage: "location")
                }
            } else {
                Button {
                    tagCurrentLocation()
                } label: {
                    HStack {
                        if tagging { ProgressView() }
                        Label("Tag this location", systemImage: "mappin.and.ellipse")
                    }
                }
                .disabled(tagging || node.connectionState != .ready)
            }
            if let tagStatus {
                Text(tagStatus).font(.caption).foregroundColor(.secondary)
            }
            HStack {
                Text("Attestations on this device")
                    .font(.caption).foregroundColor(.secondary)
                Spacer()
                Text("\(attestations.attestations.count)")
                    .font(.caption.monospaced())
            }
            if let attestSyncStatus {
                Text(attestSyncStatus).font(.caption).foregroundColor(.secondary)
            }
        }
    }

    private var encountersSection: some View {
        Section("Encounters (\(records.count))") {
            if records.isEmpty {
                Text("No encounters yet")
                    .foregroundColor(.secondary)
                    .font(.callout)
            } else {
                ForEach(records.reversed()) { record in
                    encounterRow(record)
                }
            }
        }
    }

    private func observationRow(_ obs: SSIDObservation) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(obs.ssid.isEmpty ? "(hidden)" : obs.ssid).font(.body)
            HStack(spacing: 10) {
                Text(obs.bssidHex).monospaced()
                Text("ch \(obs.channel)").foregroundColor(.secondary)
                Text("\(obs.rssi) dBm").foregroundColor(.secondary)
            }
            .font(.caption)
        }
    }

    private func encounterRow(_ rec: EncounterRecord) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("ms \(rec.capturedAtMs)").font(.body.monospaced())
            Text("\(rec.bssids.count) BSSID\(rec.bssids.count == 1 ? "" : "s")")
                .font(.caption).foregroundColor(.secondary)
        }
    }

    private func tagCurrentLocation() {
        guard !tagging else { return }
        tagging = true
        tagStatus = "Capturing scan and location…"
        bleManager.requestScan { scan, scanErr in
            DispatchQueue.main.async {
                if let err = scanErr {
                    tagging = false
                    tagStatus = "Scan failed: \(err)"
                    return
                }
                guard let scan, !scan.observations.isEmpty else {
                    tagging = false
                    tagStatus = "Node has no usable observations yet."
                    return
                }
                guard let coord = locationManager.location?.coordinate else {
                    locationManager.requestLocation()
                    tagging = false
                    tagStatus = "Waiting for location fix — tap again in a moment."
                    return
                }
                let bssids = scan.observations.map(\.bssid)
                bleManager.writeAttestation(
                    bssids: bssids,
                    location: (coord.latitude, coord.longitude)
                ) { attestation, writeErr in
                    DispatchQueue.main.async {
                        tagging = false
                        if let writeErr {
                            tagStatus = "Write failed: \(writeErr)"
                        } else if let attestation {
                            tagStatus = "Tagged \(attestation.geohash) (\(attestation.bssids.count) BSSIDs)"
                        }
                    }
                }
            }
        }
    }

    private func refresh() {
        guard !pulling else { return }
        pulling = true
        lastError = nil

        bleManager.requestScan { scan, err in
            DispatchQueue.main.async {
                self.liveScan = scan
                if let err { self.lastError = err }
            }
        }
        bleManager.pullEncounters(node: node) { count, err in
            DispatchQueue.main.async {
                self.pulling = false
                self.pullCount = count
                if let err { self.lastError = err }
            }
        }
        // M4 stage 8b: pull v2 encounter records (signed two-party
        // handshake artefacts) from /files/enc_*.cbor. New records
        // surface on the map alongside any v1 encounters still on
        // the device. Errors are logged but don't surface in the UI
        // — the v1 path covers the primary status field.
        bleManager.pullV2Encounters { added, err in
            if let err {
                print("[EncountersView] pullV2Encounters: \(err)")
            } else if added > 0 {
                print("[EncountersView] pulled \(added) v2 encounter(s)")
            }
        }
        bleManager.pushSelfAttestations(node: node) { pushed, err in
            DispatchQueue.main.async {
                if let err {
                    self.attestSyncStatus = "Push error: \(err)"
                } else if pushed == 0 {
                    self.attestSyncStatus = "No new attestations to push"
                } else {
                    self.attestSyncStatus = "Pushed \(pushed) attestation(s)"
                }
            }
        }
    }
}
