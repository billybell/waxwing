import SwiftUI

/// Displays the text content of a file stored on the connected node.
struct FileContentView: View {
    @EnvironmentObject var bleManager: BLEManager
    @ObservedObject var node: WaxwingNode
    let fileName: String

    var body: some View {
        Group {
            if bleManager.isFileOperationInProgress {
                VStack(spacing: 16) {
                    ProgressView()
                    Text("Reading file...")
                        .foregroundStyle(.secondary)
                }
            } else if let error = bleManager.fileOperationError {
                VStack(spacing: 16) {
                    Image(systemName: "exclamationmark.triangle")
                        .font(.system(size: 36))
                        .foregroundStyle(.red)
                    Text(error)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)

                    Button("Retry") {
                        bleManager.readFile(name: fileName)
                    }
                    .buttonStyle(.borderedProminent)
                }
                .padding()
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
                Button {
                    bleManager.readFile(name: fileName)
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(bleManager.isFileOperationInProgress)
            }
        }
        .onAppear {
            // Clear previous content and load this file
            bleManager.fileContent = nil
            bleManager.fileOperationError = nil
            if node.connectionState == .ready {
                bleManager.readFile(name: fileName)
            }
        }
    }
}
