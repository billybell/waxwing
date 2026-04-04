import SwiftUI

/// Settings sheet showing device identity, capabilities, and mesh status.
/// All the technical details that were previously on the main NodeDetailView
/// are collected here so they don't clutter the file-browsing experience.
struct NodeSettingsView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                connectionSection
                storageSection
                if let identity = node.identity {
                    identitySection(identity)
                    capabilitiesSection(identity)
                    meshSection(identity)
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Node Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        dismiss()
                    }
                }
            }
            .onAppear {
                if node.connectionState == .ready {
                    bleManager.fetchStorageInfo()
                }
            }
        }
    }

    // MARK: - Connection Section

    private var connectionSection: some View {
        Section {
            HStack {
                Label("Status", systemImage: stateIcon)
                Spacer()
                Text(node.connectionState.label)
                    .foregroundStyle(stateColor)
            }

            HStack {
                Label("Signal", systemImage: "antenna.radiowaves.left.and.right")
                Spacer()
                Text("\(node.rssi) dBm (\(node.signalDescription))")
                    .foregroundStyle(.secondary)
            }

            if let name = node.localName {
                HStack {
                    Label("BLE Name", systemImage: "tag")
                    Spacer()
                    Text(name)
                        .font(.body.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Connection")
        }
    }

    // MARK: - Storage Section

    private var storageSection: some View {
        Section {
            if let info = bleManager.storageInfo {
                // Usage bar
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Label("Storage", systemImage: "internaldrive")
                        Spacer()
                        Text("\(info.usedDescription) / \(info.totalDescription)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            RoundedRectangle(cornerRadius: 4)
                                .fill(.quaternary)
                                .frame(height: 8)

                            RoundedRectangle(cornerRadius: 4)
                                .fill(usageColor(info.usageFraction))
                                .frame(width: max(0, geo.size.width * info.usageFraction), height: 8)
                        }
                    }
                    .frame(height: 8)
                }
                .padding(.vertical, 4)

                HStack {
                    Label("Files", systemImage: "doc.on.doc")
                    Spacer()
                    Text("\(info.fileCount)")
                        .foregroundStyle(.secondary)
                }

                HStack {
                    Label("Free", systemImage: "arrow.down.circle")
                    Spacer()
                    Text(info.freeDescription)
                        .foregroundStyle(.secondary)
                }

                HStack {
                    Label("Reserved", systemImage: "lock.shield")
                    Spacer()
                    Text(StorageInfo.formatBytes(info.reserve))
                        .font(.body)
                        .foregroundStyle(.secondary)
                }
            } else if bleManager.isFileOperationInProgress {
                HStack {
                    ProgressView()
                        .padding(.trailing, 8)
                    Text("Loading storage info...")
                        .foregroundStyle(.secondary)
                }
            } else {
                Button {
                    bleManager.fetchStorageInfo()
                } label: {
                    Label("Load Storage Info", systemImage: "arrow.clockwise")
                }
                .disabled(node.connectionState != .ready)
            }
        } header: {
            Text("Storage")
        } footer: {
            if let info = bleManager.storageInfo, info.usageFraction > 0.85 {
                Text("Storage is nearly full. Consider deleting files to free space.")
                    .foregroundStyle(.orange)
            }
        }
    }

    private func usageColor(_ fraction: Double) -> Color {
        if fraction > 0.9 { return .red }
        if fraction > 0.75 { return .orange }
        return .blue
    }

    // MARK: - Identity Section

    private func identitySection(_ identity: DeviceIdentity) -> some View {
        Section {
            if let name = identity.name {
                HStack {
                    Label("Node Name", systemImage: "textformat")
                    Spacer()
                    Text(name)
                        .foregroundStyle(.secondary)
                }
            }

            HStack {
                Label("Protocol", systemImage: "network")
                Spacer()
                Text(identity.protocolName ?? "unknown")
                    .font(.body.monospaced())
                    .foregroundStyle(.secondary)
            }

            HStack {
                Label("Version", systemImage: "number")
                Spacer()
                Text("v\(identity.protocolVersion)")
                    .foregroundStyle(.secondary)
            }

            VStack(alignment: .leading, spacing: 4) {
                Label("Transport Public Key", systemImage: "key")
                Text(identity.tpkHex)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
            .padding(.vertical, 2)

            HStack {
                Label("Fingerprint", systemImage: "hand.wave")
                Spacer()
                Text(identity.fingerprint)
                    .font(.body.monospaced())
                    .foregroundStyle(.secondary)
            }

            if let fw = identity.firmware {
                HStack {
                    Label("Firmware", systemImage: "cpu")
                    Spacer()
                    Text("\(fw) \(identity.firmwareVersion ?? "")")
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Device Identity")
        }
    }

    // MARK: - Capabilities Section

    private func capabilitiesSection(_ identity: DeviceIdentity) -> some View {
        Section {
            let caps = identity.capabilities.descriptions
            if caps.isEmpty {
                Text("None reported")
                    .foregroundStyle(.tertiary)
            } else {
                ForEach(caps, id: \.self) { cap in
                    HStack {
                        Image(systemName: iconForCapability(cap))
                            .frame(width: 24)
                            .foregroundStyle(.blue)
                        Text(cap)
                    }
                }
            }
        } header: {
            Text("Capabilities")
        } footer: {
            Text("Raw flags: 0x\(String(identity.capabilities.rawValue, radix: 16, uppercase: true))")
        }
    }

    // MARK: - Mesh Section

    private func meshSection(_ identity: DeviceIdentity) -> some View {
        Section {
            HStack {
                Label("Manifest Items", systemImage: "doc.on.doc")
                Spacer()
                Text("\(identity.manifestCount)")
                    .foregroundStyle(.secondary)
            }

            HStack {
                Label("Mode", systemImage: identity.attended ? "person.fill" : "server.rack")
                Spacer()
                if identity.attended {
                    Text("Attended")
                        .foregroundStyle(.secondary)
                } else {
                    Text(identity.unattendedMode?.capitalized ?? "Unattended")
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Mesh Status")
        }
    }

    // MARK: - Helpers

    private var stateIcon: String {
        switch node.connectionState {
        case .disconnected: return "circle"
        case .connecting:   return "circle.dotted"
        case .connected:    return "circle.fill"
        case .readingIdentity: return "circle.fill"
        case .ready:        return "checkmark.circle.fill"
        case .failed:       return "exclamationmark.circle.fill"
        }
    }

    private var stateColor: Color {
        switch node.connectionState {
        case .disconnected: return .secondary
        case .connecting:   return .orange
        case .connected:    return .blue
        case .readingIdentity: return .blue
        case .ready:        return .green
        case .failed:       return .red
        }
    }

    private func iconForCapability(_ cap: String) -> String {
        switch cap {
        case "BLE Transfer": return "antenna.radiowaves.left.and.right"
        case "WiFi AP":      return "wifi"
        case "WiFi Client":  return "wifi"
        case "WiFi Direct":  return "wifi.circle"
        case "GPS":          return "location"
        case "SD Card":      return "sdcard"
        case "Attended":     return "person.fill"
        case "Unattended":   return "server.rack"
        default:             return "questionmark.circle"
        }
    }
}
