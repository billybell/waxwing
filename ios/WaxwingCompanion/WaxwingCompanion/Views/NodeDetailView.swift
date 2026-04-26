import SwiftUI

/// Primary view shown after tapping a discovered node.
/// Displays a compact connection header, then a segmented view of
/// files (list) and Waxwing images (grid). Settings are accessible
/// via a gear icon in the toolbar.
struct NodeDetailView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @State private var showingSettings = false
    @State private var showingCreateFile = false
    @State private var showingPhotoUpload = false
    @State private var showingCompose = false
    @State private var viewMode: ViewMode = .files
    @State private var fileEditMode: EditMode = .inactive
    @State private var fileSelection = Set<String>()
    @State private var pendingDelete: [String] = []
    @State private var isDeleting = false

    /// Process-wide cache for Waxwing images. Backed by a content-addressed
    /// on-disk store so already-downloaded images persist across reconnects
    /// and app launches.
    @ObservedObject private var imageCache = WaxwingImageCache.shared

    enum ViewMode: String, CaseIterable {
        case files = "Files"
        case grid = "Images"
    }

    var body: some View {
        VStack(spacing: 0) {
            // Compact connection banner
            connectionBanner

            // Main content area
            if node.connectionState == .ready {
                // Segmented picker
                Picker("View", selection: $viewMode) {
                    ForEach(ViewMode.allCases, id: \.self) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal)
                .padding(.vertical, 8)

                switch viewMode {
                case .files:
                    fileListContent
                case .grid:
                    ImageGridView(
                        imageCache: imageCache,
                        palette: WaxwingPalettes.cedar
                    )
                    .environmentObject(bleManager)
                }
            } else {
                connectingState
            }
        }
        .navigationTitle(node.displayName)
        .navigationBarTitleDisplayMode(.large)
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                if node.connectionState == .ready {
                    Menu {
                        Button {
                            showingCompose = true
                        } label: {
                            Label("Compose Image", systemImage: "wand.and.stars")
                        }
                        Button {
                            showingCreateFile = true
                        } label: {
                            Label("New Text File", systemImage: "doc.badge.plus")
                        }
                        Button {
                            showingPhotoUpload = true
                        } label: {
                            Label("Upload Photo", systemImage: "photo.badge.plus")
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                }
            }

            ToolbarItem(placement: .automatic) {
                if node.connectionState == .ready {
                    Button {
                        showingSettings = true
                    } label: {
                        Image(systemName: "gearshape")
                    }
                }
            }
        }
        .sheet(isPresented: $showingSettings) {
            NodeSettingsView(node: node)
                .environmentObject(bleManager)
        }
        .sheet(isPresented: $showingCreateFile) {
            CreateFileView(node: node) {
                bleManager.listFiles()
            }
            .environmentObject(bleManager)
        }
        .sheet(isPresented: $showingPhotoUpload) {
            PhotoUploadView(node: node) {
                bleManager.listFiles()
            }
            .environmentObject(bleManager)
        }
        .sheet(isPresented: $showingCompose) {
            ComposeImageView(
                node: node,
                imageCache: imageCache
            ) {
                bleManager.listFiles()
            }
            .environmentObject(bleManager)
        }
    }

    // MARK: - Connection Banner

    private var connectionBanner: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(stateColor)
                .frame(width: 8, height: 8)

            Text(node.connectionState.label)
                .font(.caption)
                .foregroundStyle(.secondary)

            if node.connectionState == .connecting || node.connectionState == .readingIdentity {
                ProgressView()
                    .scaleEffect(0.6)
            }

            Spacer()

            // Signal strength pill
            HStack(spacing: 4) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .font(.caption2)
                Text("\(node.rssi) dBm")
                    .font(.caption2)
            }
            .foregroundStyle(.secondary)

            // Connect / Disconnect
            Button {
                if node.connectionState.isConnected || node.connectionState == .connecting {
                    bleManager.disconnect()
                } else {
                    bleManager.connect(to: node)
                }
            } label: {
                Text(node.connectionState.isConnected || node.connectionState == .connecting ? "Disconnect" : "Connect")
                    .font(.caption.weight(.medium))
            }
            .buttonStyle(.bordered)
            .controlSize(.mini)
            .tint(node.connectionState.isConnected ? .red : .blue)
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial)
    }

    // MARK: - Connecting / Pre-ready State

    private var connectingState: some View {
        VStack(spacing: 20) {
            Spacer()

            if case .failed(let msg) = node.connectionState {
                Image(systemName: "exclamationmark.triangle")
                    .font(.system(size: 40))
                    .foregroundStyle(.red)
                Text(msg)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Button("Retry") {
                    bleManager.connect(to: node)
                }
                .buttonStyle(.borderedProminent)
            } else if node.connectionState == .disconnected {
                Image(systemName: "link.badge.plus")
                    .font(.system(size: 40))
                    .foregroundStyle(.secondary)
                Text("Connect to browse files")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                Button("Connect") {
                    bleManager.connect(to: node)
                }
                .buttonStyle(.borderedProminent)
            } else {
                ProgressView()
                    .scaleEffect(1.2)
                Text("Connecting to node...")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Spacer()
        }
        .padding()
    }

    // MARK: - File List (Primary Content)

    private var fileListContent: some View {
        VStack(spacing: 0) {
            List(selection: $fileSelection) {
                if bleManager.isFileOperationInProgress && bleManager.fileList.isEmpty {
                    HStack {
                        ProgressView()
                            .padding(.trailing, 8)
                        Text("Loading files...")
                            .foregroundStyle(.secondary)
                    }
                } else if bleManager.fileList.isEmpty {
                    emptyFileState
                } else {
                    filesSection
                }

                if let error = bleManager.fileOperationError {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                            .font(.caption)
                    }
                }
            }
            .listStyle(.insetGrouped)
            .environment(\.editMode, $fileEditMode)
            .refreshable {
                bleManager.listFiles()
            }
            .onAppear {
                if node.connectionState == .ready {
                    bleManager.listFiles()
                }
            }

            if fileEditMode.isEditing {
                fileEditBar
            }
        }
        .alert(deleteAlertTitle, isPresented: deleteAlertBinding) {
            Button("Cancel", role: .cancel) { pendingDelete = [] }
            Button("Delete", role: .destructive) {
                performDelete(names: pendingDelete)
            }
        } message: {
            Text(deleteAlertMessage)
        }
    }

    // Bottom action bar shown only while in edit mode.
    private var fileEditBar: some View {
        HStack {
            Text(fileSelection.isEmpty
                 ? "Select files to delete"
                 : "\(fileSelection.count) selected")
                .font(.footnote)
                .foregroundStyle(.secondary)

            Spacer()

            Button(role: .destructive) {
                pendingDelete = Array(fileSelection)
            } label: {
                Label("Delete", systemImage: "trash")
                    .labelStyle(.titleAndIcon)
            }
            .tint(.red)
            .disabled(fileSelection.isEmpty || isDeleting)
        }
        .padding(.horizontal)
        .padding(.vertical, 10)
        .background(.bar)
    }

    private var deleteAlertTitle: String {
        pendingDelete.count == 1
            ? "Delete \(pendingDelete[0])?"
            : "Delete \(pendingDelete.count) files?"
    }

    private var deleteAlertMessage: String {
        pendingDelete.count == 1
            ? "This permanently removes the file from the node."
            : "This permanently removes \(pendingDelete.count) files from the node."
    }

    private var deleteAlertBinding: Binding<Bool> {
        Binding(
            get: { !pendingDelete.isEmpty },
            set: { if !$0 { pendingDelete = [] } }
        )
    }

    private func performDelete(names: [String]) {
        guard !names.isEmpty else { return }
        isDeleting = true
        deleteNext(remaining: names)
    }

    private func deleteNext(remaining: [String]) {
        guard let next = remaining.first else {
            DispatchQueue.main.async {
                isDeleting = false
                pendingDelete = []
                fileSelection.removeAll()
                fileEditMode = .inactive
                bleManager.listFiles()
            }
            return
        }
        bleManager.deleteFile(name: next) { _ in
            DispatchQueue.main.async {
                deleteNext(remaining: Array(remaining.dropFirst()))
            }
        }
    }

    private var emptyFileState: some View {
        Section {
            VStack(spacing: 12) {
                Image(systemName: "doc.text")
                    .font(.system(size: 36))
                    .foregroundStyle(.secondary)
                Text("No files on this node")
                    .font(.headline)
                    .foregroundStyle(.secondary)
                Text("Tap + to compose an image, create a text file, or upload a photo")
                    .font(.subheadline)
                    .foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 24)
        }
    }

    private var filesSection: some View {
        Section {
            ForEach(bleManager.fileList) { file in
                fileRow(for: file)
                    .tag(file.name)
                    .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                        Button(role: .destructive) {
                            pendingDelete = [file.name]
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                        .disabled(isDeleting)
                    }
                    .contextMenu {
                        Button(role: .destructive) {
                            pendingDelete = [file.name]
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
            }
        } header: {
            HStack {
                Text("\(bleManager.fileList.count) file(s)")
                Spacer()
                if !bleManager.fileList.isEmpty {
                    Button(fileEditMode.isEditing ? "Done" : "Edit") {
                        withAnimation {
                            if fileEditMode.isEditing {
                                fileEditMode = .inactive
                                fileSelection.removeAll()
                            } else {
                                fileEditMode = .active
                            }
                        }
                    }
                    .font(.footnote)
                    .textCase(nil)
                }
            }
        }
    }

    @ViewBuilder
    private func fileRow(for file: NodeFile) -> some View {
        if fileEditMode.isEditing {
            HStack {
                Image(systemName: iconForFile(file.name))
                    .foregroundStyle(.blue)
                VStack(alignment: .leading, spacing: 2) {
                    Text(file.name).font(.body)
                    Text(file.sizeDescription)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 2)
        } else {
            NavigationLink(destination: FileContentView(node: node, fileName: file.name)) {
                HStack {
                    Image(systemName: iconForFile(file.name))
                        .foregroundStyle(.blue)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(file.name).font(.body)
                        Text(file.sizeDescription)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 2)
            }
        }
    }

    // MARK: - Helpers

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

    private func iconForFile(_ name: String) -> String {
        let ext = (name as NSString).pathExtension.lowercased()
        switch ext {
        case "jpg", "jpeg", "png", "gif", "bmp", "webp":
            return "photo"
        case "txt", "md", "log":
            return "doc.text"
        case "json", "xml", "csv":
            return "doc.badge.gearshape"
        default:
            return "doc"
        }
    }
}
