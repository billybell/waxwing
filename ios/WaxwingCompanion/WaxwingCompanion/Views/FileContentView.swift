import SwiftUI

/// Displays the text or binary content of a file stored on the connected node.
struct FileContentView: View {
        @EnvironmentObject var bleManager: BLEManager
        @ObservedObject var node: WaxwingNode
    let fileName: String

        /// Whether the image detail sheet is visible
    @State private var showingDetails = false

        /// Downloaded file data stored locally so SwiftUI re-renders without
        /// relying on shared BLEManager state that races across navigation.
    @State private var fileData: Data?

    var body: some View {
        Group {
            if bleManager.isFileOperationInProgress, fileData == nil {
                VStack(spacing: 16) {
                    ProgressView()
                    Text("Reading file...")
                            .foregroundStyle(.secondary)
                    }
                } else if let error = bleManager.fileOperationError, fileData == nil {
                VStack(spacing: 16) {
                    Image(systemName: "exclamationmark.triangle")
                            .font(.system(size: 36))
                            .foregroundStyle(.red)
                    Text(error)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)

                    Button("Retry") { readFile() }
                        .buttonStyle(.borderedProminent)
                    }
                    .padding()
                } else if let data = fileData, isImage(data), let image = UIImage(data: data) {
                VStack(spacing: 0) {
                    Image(uiImage: image)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .padding(.horizontal)

                    Text(fileName)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .padding(.horizontal)
                            .padding(.top, 4)

                    Text("Tap for details")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .padding(.bottom, 8)
                    }
                    .frame(maxWidth: .infinity)
                    .onTapGesture { showingDetails = true }
                    .fullScreenCover(isPresented: $showingDetails) {
                        NavigationStack {
                            ImageDetailView(
                                fileName: fileName,
                                image: image,
                                caption: nil,
                                palette: WaxwingPalettes.cedar
                                )
                                .toolbar {
                                    ToolbarItem(placement: .topBarLeading) {
                                        Button("Done") { showingDetails = false }
                                        }
                                    }
                            }
                        }
                    } else if let content = bleManager.fileContent {
                ScrollView {
                    Text(content)
                            .font(.body.monospaced())
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding()
                            .textSelection(.enabled)
                    }
                } else {
                Text("No content")
                        .foregroundStyle(.tertiary)
                }
            }
            .navigationTitle(fileName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
            ToolbarItem(placement: .automatic) {
                Button { readFile() } label: {
                    Image(systemName: "arrow.clockwise")
                    }
                    .disabled(bleManager.isFileOperationInProgress || fileData != nil)
                }
            }
            .onAppear {
            if node.connectionState == .ready {
                readFile()
                 }
            }
        }

        /// Kick off a chunked read and store the result locally.
    private func readFile() {
        bleManager.fileOperationError = nil
        self.fileData = nil
          // If bleManager already has data, use it immediately.
        if let existing = bleManager.fileContentData {
            processReceived(data: existing)
            return
          }

        bleManager.readFileChunked(name: fileName) { data in
               // If we're still loading (no previous data), accept the result.
            if fileData == nil {
                processReceived(data: data)
                }
            }
        }

        /// Decode downloaded bytes and set state so SwiftUI re-renders.
    private func processReceived(data: Data?) {
        guard let data else {
             self.fileData = nil
            return
           }
        self.fileData = data
        }

        /// True if the data starts with a recognized image magic header.
    private func isImage(_ data: Data) -> Bool {
        guard data.count >= 3 else { return false }
        // PNG magic bytes
        if data.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) { return true }
        // JPEG magic bytes: cast to [UInt8] for starts(with:) comparison
        if data.starts(with: [0xFF, 0xD8, 0xFF] as [UInt8]) { return true }
        return false
         }}