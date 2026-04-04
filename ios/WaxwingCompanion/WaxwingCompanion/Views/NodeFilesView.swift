import SwiftUI

/// Shows the files stored on the connected Waxwing node.
/// Allows navigating to view file contents or create new files.
struct NodeFilesView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    @State private var showingCreateFile = false

    var body: some View {
        List {
            if bleManager.isFileOperationInProgress {
                HStack {
                    ProgressView()
                        .padding(.trailing, 8)
                    Text("Loading files...")
                        .foregroundStyle(.secondary)
                }
            } else if bleManager.fileList.isEmpty {
                emptyState
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
        .navigationTitle("Files")
        .navigationBarTitleDisplayMode(.large)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    showingCreateFile = true
                } label: {
                    Image(systemName: "plus")
                }
                .disabled(node.connectionState != .ready)
            }

            ToolbarItem(placement: .automatic) {
                Button {
                    bleManager.listFiles()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(node.connectionState != .ready || bleManager.isFileOperationInProgress)
            }
        }
        .sheet(isPresented: $showingCreateFile) {
            CreateFileView(node: node) {
                // Refresh the file list after creating a file
                bleManager.listFiles()
            }
            .environmentObject(bleManager)
        }
        .onAppear {
            if node.connectionState == .ready {
                bleManager.listFiles()
            }
        }
    }

    // MARK: - Empty State

    private var emptyState: some View {
        Section {
            VStack(spacing: 12) {
                Image(systemName: "doc.text")
                    .font(.system(size: 36))
                    .foregroundStyle(.secondary)
                Text("No files on this node")
                    .font(.headline)
                    .foregroundStyle(.secondary)
                Text("Tap + to create a text file")
                    .font(.subheadline)
                    .foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 24)
        }
    }

    // MARK: - Files List

    private var filesSection: some View {
        Section {
            ForEach(bleManager.fileList) { file in
                NavigationLink(destination: FileContentView(node: node, fileName: file.name)) {
                    HStack {
                        Image(systemName: "doc.text")
                            .foregroundStyle(.blue)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(file.name)
                                .font(.body)
                            Text(file.sizeDescription)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 2)
                }
            }
        } header: {
            Text("\(bleManager.fileList.count) file(s)")
        }
    }
}
