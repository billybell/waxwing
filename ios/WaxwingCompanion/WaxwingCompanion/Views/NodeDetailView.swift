import SwiftUI

struct NodeDetailView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode

    var body: some View {
        List {
            // Connection section
            connectionSection

            // File management (shown when connection is ready)
            if node.connectionState == .ready {
                filesSection
            }

            // Identity section (shown after reading Device Identity)
            if let identity = node.identity {
                identitySection(identity)
                capabilitiesSection(identity)
                meshSection(identity)
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(node.displayName)
        .navigationBarTitleDisplayMode(.large)
    }

    // MARK: - Connection Section

    private var connectionSection: some View {
        Section {
            // State row
            HStack {
                Label("Status", systemImage: stateIcon)
                Spacer()
                Text(node.connectionState.label)
                    .foregroundStyle(stateColor)

                if case .connecting = node.connectionState {
                    ProgressView()
                        .scaleEffect(0.7)
                        .padding(.leading, 4)
                }
                if case .readingIdentity = node.connectionState {
                    ProgressView()
                        .scaleEffect(0.7)
                        .padding(.leading, 4)
                }
            }

            // Signal
            HStack {
                Label("Signal", systemImage: "antenna.radiowaves.left.and.right")
                Spacer()
                Text("\(node.rssi) dBm (\(node.signalDescription))")
                    .foregroundStyle(.secondary)
            }

            // BLE name
            if let name = node.localName {
                HStack {
                    Label("BLE Name", systemImage: "tag")
                    Spacer()
                    Text(name)
                        .font(.body.monospaced())
                        .foregroundStyle(.secondary)
                }
            }

            // Connect / Disconnect button
            connectButton
        } header: {
            Text("Connection")
        }
    }

    private var connectButton: some View {
        Button {
            if node.connectionState.isConnected || node.connectionState == .connecting {
                bleManager.disconnect()
            } else {
                bleManager.connect(to: node)
            }
        } label: {
            HStack {
                Spacer()
                if node.connectionState.isConnected || node.connectionState == .connecting {
                    Label("Disconnect", systemImage: "link.badge.plus")
                } else {
                    Label("Connect", systemImage: "link")
                }
                Spacer()
            }
        }
        .tint(node.connectionState.isConnected ? .red : .blue)
    }

    // MARK: - Files Section

    private var filesSection: some View {
        Section {
            NavigationLink(destination: NodeFilesView(node: node)) {
                HStack {
                    Label("Browse Files", systemImage: "folder")
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
            }
        } header: {
            Text("Storage")
        } footer: {
            Text("View and manage text files on this node's flash storage")
        }
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
