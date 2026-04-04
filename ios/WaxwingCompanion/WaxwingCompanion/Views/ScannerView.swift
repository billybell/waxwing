import SwiftUI
import CoreBluetooth

struct ScannerView: View {
    @EnvironmentObject var bleManager: BLEManager

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                // Status bar
                statusBar

                // Node list or empty state
                if bleManager.discoveredNodes.isEmpty {
                    emptyState
                } else {
                    nodeList
                }
            }
            .navigationTitle("Waxwing")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    scanButton
                }
            }
        }
    }

    // MARK: - Status Bar

    private var statusBar: some View {
        HStack {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
            Text(bleManager.statusMessage)
                .font(.caption)
                .foregroundStyle(.secondary)
            Spacer()
            if bleManager.isScanning {
                ProgressView()
                    .scaleEffect(0.7)
            }
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial)
    }

    private var statusColor: Color {
        switch bleManager.bluetoothState {
        case .poweredOn: return bleManager.isScanning ? .green : .blue
        case .poweredOff: return .red
        case .unauthorized: return .orange
        default: return .gray
        }
    }

    // MARK: - Scan Button

    private var scanButton: some View {
        Button {
            if bleManager.isScanning {
                bleManager.stopScanning()
            } else {
                bleManager.startScanning()
            }
        } label: {
            Text(bleManager.isScanning ? "Stop" : "Scan")
        }
        .disabled(bleManager.bluetoothState != .poweredOn)
    }

    // MARK: - Empty State

    private var emptyState: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "antenna.radiowaves.left.and.right")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)

            if bleManager.bluetoothState == .poweredOn {
                Text("No Waxwing nodes found")
                    .font(.headline)
                    .foregroundStyle(.secondary)
                Text("Make sure your Pico W is powered on\nand advertising nearby.")
                    .font(.subheadline)
                    .foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)

                if !bleManager.isScanning {
                    Button("Start Scanning") {
                        bleManager.startScanning()
                    }
                    .buttonStyle(.borderedProminent)
                    .padding(.top, 8)
                }
            } else {
                Text(bleManager.statusMessage)
                    .font(.headline)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding()
    }

    // MARK: - Node List

    private var nodeList: some View {
        List {
            ForEach(bleManager.discoveredNodes) { node in
                NavigationLink(destination: NodeDetailView(node: node)) {
                    NodeRow(node: node)
                }
            }
        }
        .listStyle(.insetGrouped)
    }
}

// MARK: - Node Row

struct NodeRow: View {
    @ObservedObject var node: WaxwingNode

    var body: some View {
        HStack(spacing: 12) {
            // Signal indicator
            signalIcon
                .frame(width: 32)

            // Name and details
            VStack(alignment: .leading, spacing: 2) {
                Text(node.displayName)
                    .font(.body.weight(.medium))

                HStack(spacing: 8) {
                    if let fp = node.fingerprint {
                        Text(fp)
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    }

                    Text("\(node.rssi) dBm")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            // Connection state indicator
            if node.connectionState.isConnected {
                Image(systemName: "link.circle.fill")
                    .foregroundStyle(.green)
            }
        }
        .padding(.vertical, 4)
    }

    private var signalIcon: some View {
        Image(systemName: signalImageName)
            .font(.title3)
            .foregroundStyle(signalColor)
    }

    private var signalImageName: String {
        switch node.rssi {
        case -50...0:     return "wifi"
        case -65..<(-50): return "wifi"
        case -80..<(-65): return "wifi.exclamationmark"
        default:          return "wifi.slash"
        }
    }

    private var signalColor: Color {
        switch node.rssi {
        case -50...0:     return .green
        case -65..<(-50): return .blue
        case -80..<(-65): return .orange
        default:          return .red
        }
    }
}
