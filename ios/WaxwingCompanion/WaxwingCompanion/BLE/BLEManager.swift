import Foundation
import CoreBluetooth
import Combine

// MARK: - BLE Manager
//
// Handles scanning for Waxwing nodes, connecting, and reading Device Identity.
// Scans are filtered to only the Waxwing service UUID.

class BLEManager: NSObject, ObservableObject {

    // MARK: - Published State

    @Published var isScanning = false
    @Published var bluetoothState: CBManagerState = .unknown
    @Published var discoveredNodes: [WaxwingNode] = []
    @Published var connectedNode: WaxwingNode?
    @Published var statusMessage: String = "Waiting for Bluetooth..."

    /// File operation results
    @Published var fileList: [NodeFile] = []
    @Published var fileContent: String?
    @Published var fileOperationError: String?
    @Published var isFileOperationInProgress = false

    /// Storage info from node
    @Published var storageInfo: StorageInfo?

    // MARK: - Private

    private var centralManager: CBCentralManager!
    private var nodesByPeripheralID: [UUID: WaxwingNode] = [:]

    /// Discovered BLE characteristics (keyed by UUID)
    private var characteristicsByUUID: [CBUUID: CBCharacteristic] = [:]

    /// Stale node timeout — remove nodes not seen in this many seconds
    private let staleTimeout: TimeInterval = 30
    private var staleTimer: Timer?

    /// Pending file operation completion handler (for the currently active command).
    private var fileResponseHandler: ((CBORValue) -> Void)?

    /// Watchdog that fails the in-flight command if no response arrives
    /// in time. Without this, a single dropped/truncated notification
    /// from the node would silently wedge the entire operation queue.
    private var fileResponseWatchdog: DispatchWorkItem?

    /// How long we wait for a single file-response notification before
    /// declaring the command failed. The slowest legitimate command is
    /// the first paginated `ls` against a node whose hash cache is cold
    /// — the Pico has to stream-hash every image in the page, which can
    /// take several seconds for half a megabyte of pixels. 20 s gives
    /// us plenty of headroom while still catching real wedges fast
    /// enough to feel responsive.
    private let fileResponseTimeout: TimeInterval = 20.0

    /// Queue of top-level file operations waiting to run.
    /// Each entry is a closure that kicks off the operation; it will be
    /// called only after the previous operation has fully completed.
    private var pendingOperations: [() -> Void] = []

    /// Whether a multi-step file operation (e.g. chunked read/write)
    /// is currently in progress and owns the BLE command channel.
    private var operationInFlight = false

    /// True once the file-response notification subscription has been
    /// confirmed live by the peripheral via didUpdateNotificationStateFor.
    /// We MUST NOT issue file commands until this is true, or the response
    /// notification can be missed and the command will hang.
    private var notificationsReady = false

    // MARK: - Init

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: - Public API

    func startScanning() {
        guard bluetoothState == .poweredOn else {
            statusMessage = "Bluetooth not available"
            return
        }

        // Scan for peripherals advertising the Waxwing service UUID
        centralManager.scanForPeripherals(
            withServices: [WaxwingUUID.service],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
        isScanning = true
        statusMessage = "Scanning for Waxwing nodes..."

        // Start stale-node cleanup timer
        staleTimer?.invalidate()
        staleTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.pruneStaleNodes()
        }
    }

    func stopScanning() {
        centralManager.stopScan()
        isScanning = false
        staleTimer?.invalidate()
        staleTimer = nil
        statusMessage = discoveredNodes.isEmpty ? "Scan stopped" : "Found \(discoveredNodes.count) node(s)"
    }

    func connect(to node: WaxwingNode) {
        // Disconnect existing if any
        if let existing = connectedNode, existing.id != node.id {
            disconnect()
        }

        // No-op if we're already connected to this same node — avoid
        // stomping on an in-flight handshake or already-ready session.
        if connectedNode?.id == node.id, node.connectionState.isConnected {
            print("[BLE] connect() ignored: already connected to \(node.displayName)")
            return
        }

        // Defensive: clear any leftover characteristic cache so the next
        // service discovery populates fresh references. This matters on
        // reconnect: a stale entry from the previous session could otherwise
        // be used to write to a now-invalid characteristic instance.
        characteristicsByUUID.removeAll()

        node.connectionState = .connecting
        connectedNode = node
        statusMessage = "Connecting to \(node.displayName)..."

        print("[BLE] connect: \(node.displayName) (\(node.peripheral.identifier))")
        centralManager.connect(node.peripheral, options: nil)
    }

    func disconnect() {
        guard let node = connectedNode else { return }
        print("[BLE] disconnect: \(node.displayName)")
        centralManager.cancelPeripheralConnection(node.peripheral)
        node.connectionState = .disconnected
        node.identity = nil
        connectedNode = nil
        characteristicsByUUID.removeAll()
        notificationsReady = false

        // Fire any in-flight handler so operations fail cleanly
        let handler = fileResponseHandler
        fileResponseHandler = nil
        fileResponseWatchdog?.cancel()
        fileResponseWatchdog = nil
        isFileOperationInProgress = false
        pendingOperations.removeAll()
        operationInFlight = false

        if let handler {
            let errorResponse = CBORValue.map([
                (.textString("error"), .textString("BLE disconnected"))
            ])
            handler(errorResponse)
        }

        statusMessage = "Disconnected"
    }

    // MARK: - File Operations
    //
    // Every public file operation is wrapped in `enqueueOperation` so
    // only one runs at a time.  Each MUST call `finishOperation()`
    // on every exit path (success, error, early return).

    /// Request the list of files stored on the connected node.
    ///
    /// The node returns the manifest in pages — a single BLE notification
    /// can only carry MTU-3 bytes (~244 with the default iOS-negotiated
    /// MTU), and the full listing easily exceeds that with even a handful
    /// of images. We accumulate pages here and only publish `fileList`
    /// once the final page (no `next_offset`) arrives.
    func listFiles() {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.fetchFilesPage(offset: 0, accumulated: [])
        }
    }

    /// Internal helper that issues `{"cmd": "ls", "offset": N}` and
    /// recurses until the node stops sending a `next_offset`. The
    /// outer enqueueOperation already serializes us against other
    /// operations, so it is safe to chain sendFileCommand calls without
    /// re-enqueuing — we hold the slot until the final page resolves.
    private func fetchFilesPage(offset: Int, accumulated: [NodeFile]) {
        let cmd: [String: Any] = ["cmd": "ls", "offset": offset]
        sendFileCommand(cmd) { [weak self] response in
            guard let self else { return }

            if let error = response["error"]?.stringValue {
                self.fileOperationError = error
                self.finishOperation()
                return
            }

            guard case .array(let items)? = response["files"] else {
                self.fileOperationError = "Invalid file list response"
                self.finishOperation()
                return
            }

            let pageEntries: [NodeFile] = items.compactMap { item in
                guard let name = item["name"]?.stringValue else { return nil }
                let size = item["size"]?.uintValue ?? 0
                let hash = item["hash"]?.dataValue
                return NodeFile(name: name, size: Int(size), hash: hash)
            }

            let merged = accumulated + pageEntries

            if let next = response["next_offset"]?.uintValue {
                // More pages remain — chain the next request inside the
                // same operation slot.
                self.fetchFilesPage(offset: Int(next), accumulated: merged)
            } else {
                // Final page — publish and release the operation slot.
                self.fileList = merged
                self.finishOperation()
            }
        }
    }

    /// Fetch storage statistics from the connected node.
    func fetchStorageInfo() {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "storage_info"]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    return
                }
                if let info = response["info"] {
                    self.storageInfo = StorageInfo.fromCBOR(info)
                }
            }
        }
    }

    /// Read a text file from the connected node.
    func readFile(name: String) {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "read", "name": name]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    self.fileContent = nil
                    return
                }
                self.fileContent = response["data"]?.stringValue
            }
        }
    }

    /// Write a text file to the connected node.
    func writeFile(name: String, content: String, completion: ((Bool) -> Void)? = nil) {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "write", "name": name, "data": content]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion?(false)
                    return
                }
                if response["ok"]?.boolValue == true {
                    self.fileOperationError = nil
                    completion?(true)
                } else {
                    self.fileOperationError = "Write failed"
                    completion?(false)
                }
            }
        }
    }

    /// Write binary data to the node in chunks.
    ///
    /// The node-side protocol accepts chunked commands:
    ///   1. `{cmd: "write_start", name: "photo.jpg", size: 12345}` — open file
    ///   2. `{cmd: "write_chunk", name: "photo.jpg", offset: N, data: <bytes>}` — append chunk
    ///   3. `{cmd: "write_end", name: "photo.jpg"}` — finalise and close
    ///
    /// Chunk data is sent as a CBOR byte string (major type 2) — no base64
    /// encoding — to maximize throughput within BLE write limits.
    ///
    /// The default chunk size of 384 raw bytes produces CBOR payloads of
    /// ~440 bytes, which fits comfortably within the Pico W's 512-byte
    /// BLE ATT write limit after MTU negotiation.
    func writeFileChunked(
        name: String,
        data: Data,
        chunkSize: Int = 384,
        progress: ((Double) -> Void)? = nil,
        completion: ((Bool) -> Void)? = nil
    ) {
        enqueueOperation { [weak self] in
            guard let self else { return }
            let totalSize = data.count
            self.sendFileCommand(["cmd": "write_start", "name": name, "size": totalSize]) { [weak self] response in
                guard let self else { return }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion?(false)
                    self.finishOperation()
                    return
                }
                guard response["ok"]?.boolValue == true else {
                    self.fileOperationError = "Failed to start chunked write"
                    completion?(false)
                    self.finishOperation()
                    return
                }

                self.sendNextChunk(
                    name: name,
                    data: data,
                    offset: 0,
                    chunkSize: chunkSize,
                    totalSize: totalSize,
                    progress: progress,
                    completion: completion
                )
            }
        }
    }

    /// Recursively sends data chunks until the entire payload is transferred.
    /// finishOperation() is called when the entire write completes or fails.
    private func sendNextChunk(
        name: String,
        data: Data,
        offset: Int,
        chunkSize: Int,
        totalSize: Int,
        progress: ((Double) -> Void)?,
        completion: ((Bool) -> Void)?
    ) {
        if offset >= totalSize {
            // All chunks sent — finalise
            sendFileCommand(["cmd": "write_end", "name": name]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion?(false)
                    return
                }
                if response["ok"]?.boolValue == true {
                    self.fileOperationError = nil
                    progress?(1.0)
                    completion?(true)
                } else {
                    self.fileOperationError = "Failed to finalize file"
                    completion?(false)
                }
            }
            return
        }

        let end = min(offset + chunkSize, totalSize)
        let chunk = data.subdata(in: offset..<end)

        let cmd: [String: Any] = [
            "cmd": "write_chunk",
            "name": name,
            "offset": offset,
            "data": chunk
        ]

        sendFileCommand(cmd) { [weak self] response in
            guard let self else { return }
            if let error = response["error"]?.stringValue {
                self.fileOperationError = error
                completion?(false)
                self.finishOperation()
                return
            }
            guard response["ok"]?.boolValue == true else {
                self.fileOperationError = "Chunk write failed at offset \(offset)"
                completion?(false)
                self.finishOperation()
                return
            }

            let newOffset = end
            let currentProgress = Double(newOffset) / Double(totalSize)
            progress?(currentProgress)

            self.sendNextChunk(
                name: name,
                data: data,
                offset: newOffset,
                chunkSize: chunkSize,
                totalSize: totalSize,
                progress: progress,
                completion: completion
            )
        }
    }

    /// Read binary file data from the node in chunks.
    ///
    /// Mirrors the chunked write protocol:
    ///   1. `{cmd: "read_start", name: "photo.png"}` — open + get size
    ///   2. `{cmd: "read_chunk", name: "photo.png", offset: N, size: 384}` — fetch chunk
    ///   3. Repeat until all bytes received
    ///
    /// Falls back to returning data directly if the node includes it in the
    /// read_start response (for very small files).
    func readFileChunked(
        name: String,
        chunkSize: Int = 384,
        progress: ((Double) -> Void)? = nil,
        completion: @escaping (Data?) -> Void
    ) {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "read_start", "name": name]) { [weak self] response in
                guard let self else { return }

                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion(nil)
                    self.finishOperation()
                    return
                }

                guard response["ok"]?.boolValue == true,
                      let totalSize = response["size"]?.uintValue else {
                    // Fallback: maybe the node returned data directly
                    if let data = response["data"]?.dataValue {
                        completion(data)
                    } else {
                        self.fileOperationError = "read_start failed"
                        completion(nil)
                    }
                    self.finishOperation()
                    return
                }

                let total = Int(totalSize)
                if total == 0 {
                    completion(Data())
                    self.finishOperation()
                    return
                }

                self.readNextChunk(
                    name: name,
                    accumulated: Data(),
                    offset: 0,
                    chunkSize: chunkSize,
                    totalSize: total,
                    progress: progress,
                    completion: completion
                )
            }
        }
    }

    /// Recursively reads data chunks until the entire file is downloaded.
    /// finishOperation() is called when the entire read completes or fails.
    private func readNextChunk(
        name: String,
        accumulated: Data,
        offset: Int,
        chunkSize: Int,
        totalSize: Int,
        progress: ((Double) -> Void)?,
        completion: @escaping (Data?) -> Void
    ) {
        if offset >= totalSize {
            progress?(1.0)
            completion(accumulated)
            finishOperation()
            return
        }

        let requestSize = min(chunkSize, totalSize - offset)
        let cmd: [String: Any] = [
            "cmd": "read_chunk",
            "name": name,
            "offset": offset,
            "size": requestSize
        ]

        sendFileCommand(cmd) { [weak self] response in
            guard let self else { return }

            if let error = response["error"]?.stringValue {
                self.fileOperationError = error
                completion(nil)
                self.finishOperation()
                return
            }

            guard response["ok"]?.boolValue == true,
                  let chunkData = response["data"]?.dataValue else {
                self.fileOperationError = "read_chunk failed at offset \(offset)"
                completion(nil)
                self.finishOperation()
                return
            }

            var buffer = accumulated
            buffer.append(chunkData)
            let newOffset = offset + chunkData.count
            let currentProgress = Double(newOffset) / Double(totalSize)
            progress?(currentProgress)

            self.readNextChunk(
                name: name,
                accumulated: buffer,
                offset: newOffset,
                chunkSize: chunkSize,
                totalSize: totalSize,
                progress: progress,
                completion: completion
            )
        }
    }

    /// Write metadata for a file on the connected node.
    /// The metadata dict is sent as a CBOR map inside the command.
    func writeFileMeta(name: String, metadata: ImageMetadata, completion: ((Bool) -> Void)? = nil) {
        let metaDict = metadata.toCBORDict()
        let cmd: [String: Any] = ["cmd": "write_meta", "name": name, "meta": metaDict]
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(cmd) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion?(false)
                    return
                }
                completion?(response["ok"]?.boolValue == true)
            }
        }
    }

    /// Read metadata for a file on the connected node.
    func readFileMeta(name: String, completion: @escaping (ImageMetadata?) -> Void) {
        let cmd: [String: Any] = ["cmd": "read_meta", "name": name]
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(cmd) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if response["error"]?.stringValue != nil {
                    completion(nil)
                    return
                }
                if let meta = response["meta"] {
                    completion(ImageMetadata.fromCBOR(meta))
                } else {
                    completion(nil)
                }
            }
        }
    }

    /// Delete a file on the connected node.
    func deleteFile(name: String, completion: ((Bool) -> Void)? = nil) {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "delete", "name": name]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    completion?(false)
                    return
                }
                completion?(response["ok"]?.boolValue == true)
            }
        }
    }

    // MARK: - Operation Queue
    //
    // BLE can only process one file command/response pair at a time
    // (single fileResponseHandler).  Multi-step operations like
    // chunked reads and writes issue many sequential commands.
    //
    // `enqueueOperation` serializes top-level operations so they
    // don't clobber each other.  Each operation MUST call
    // `finishOperation()` when it's completely done (success or
    // failure) so the next queued operation can start.

    /// Schedule a top-level file operation.  If nothing is in flight
    /// the operation starts immediately; otherwise it waits in line.
    private func enqueueOperation(_ operation: @escaping () -> Void) {
        if operationInFlight {
            pendingOperations.append(operation)
        } else {
            operationInFlight = true
            operation()
        }
    }

    /// Mark the current operation as complete and start the next
    /// queued one, if any.  MUST be called exactly once per
    /// enqueued operation, on success AND failure paths.
    private func finishOperation() {
        if pendingOperations.isEmpty {
            operationInFlight = false
        } else {
            let next = pendingOperations.removeFirst()
            // Stay operationInFlight = true, hand off to next
            next()
        }
    }

    // MARK: - File Command Transport

    /// Send a single BLE file command and invoke `handler` when the
    /// node responds.  This is the low-level primitive used by all
    /// file operations.  Callers that issue multiple sequential
    /// commands (chunked read/write) must be wrapped in a single
    /// `enqueueOperation` block so they don't interleave.
    private func sendFileCommand(_ command: [String: Any],
                                  handler: @escaping (CBORValue) -> Void) {
        // Helper: synthesize an error response and deliver it to the caller.
        // Calling the handler keeps the operation queue moving — every caller
        // wraps its handler in a `defer { finishOperation() }` block, so as
        // long as we ALWAYS deliver a response (even an error one) the queue
        // will not deadlock.
        func failCommand(_ message: String) {
            print("[BLE] sendFileCommand failed: \(message)")
            fileOperationError = message
            isFileOperationInProgress = false
            let errorResponse = CBORValue.map([
                (.textString("error"), .textString(message))
            ])
            handler(errorResponse)
        }

        guard connectedNode?.connectionState == .ready else {
            failCommand("Not connected to a node")
            return
        }
        guard notificationsReady else {
            failCommand("File response subscription not yet active")
            return
        }
        guard let char = characteristicsByUUID[WaxwingUUID.fileCommand] else {
            failCommand("File command characteristic not found")
            return
        }
        guard let peripheral = connectedNode?.peripheral else {
            failCommand("No peripheral")
            return
        }

        isFileOperationInProgress = true
        fileOperationError = nil

        let cmdName = (command["cmd"] as? String) ?? "?"
        let sentAt = Date()
        let extra: String = {
            if let off = command["offset"] as? Int { return " offset=\(off)" }
            if let name = command["name"] as? String { return " name=\(name)" }
            return ""
        }()
        print("[BLE] sendFileCommand: \(cmdName)\(extra)")

        // Wrap the caller's handler so it ALSO cancels the watchdog and
        // resets in-progress state on every exit path.
        let wrapped: (CBORValue) -> Void = { [weak self] response in
            guard let self else { return }
            let elapsed = Date().timeIntervalSince(sentAt)
            print(String(format: "[BLE] sendFileCommand: %@%@ ← response in %.2fs",
                         cmdName, extra, elapsed))
            self.fileResponseWatchdog?.cancel()
            self.fileResponseWatchdog = nil
            self.isFileOperationInProgress = false
            handler(response)
        }
        fileResponseHandler = wrapped

        let data = CBOREncoder.encode(command)
        peripheral.writeValue(data, for: char, type: .withResponse)

        // Arm the watchdog. If the response notification never arrives
        // (truncated by MTU, lost, dropped because the link went idle,
        // etc.) we synthesize an error response so the operation queue
        // never gets permanently wedged.
        let watchdog = DispatchWorkItem { [weak self] in
            guard let self else { return }
            // If somebody already delivered a real response, the wrapped
            // handler will have cleared fileResponseHandler. In that case
            // we have nothing to do.
            guard self.fileResponseHandler != nil else { return }
            print("[BLE] sendFileCommand WATCHDOG fired for \(cmdName) — no response after \(self.fileResponseTimeout)s")
            self.deliverFileResponseError("No response from node (timeout)")
        }
        fileResponseWatchdog = watchdog
        DispatchQueue.main.asyncAfter(deadline: .now() + fileResponseTimeout,
                                      execute: watchdog)
    }

    /// Synthesize a CBOR error response and deliver it to the in-flight
    /// command handler (if any). This is the single chokepoint for
    /// "the response failed somehow" — it guarantees finishOperation()
    /// runs and the queue keeps moving.
    private func deliverFileResponseError(_ message: String) {
        fileOperationError = message
        isFileOperationInProgress = false
        let handler = fileResponseHandler
        fileResponseHandler = nil
        fileResponseWatchdog?.cancel()
        fileResponseWatchdog = nil
        guard let handler else { return }
        let errorResponse = CBORValue.map([
            (.textString("error"), .textString(message))
        ])
        handler(errorResponse)
    }

    // MARK: - Private Helpers

    private func pruneStaleNodes() {
        let cutoff = Date().addingTimeInterval(-staleTimeout)
        discoveredNodes.removeAll { node in
            // Don't prune the connected node
            guard node.id != connectedNode?.id else { return false }
            return node.lastSeen < cutoff
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothState = central.state

        switch central.state {
        case .poweredOn:
            statusMessage = "Bluetooth ready"
        case .poweredOff:
            statusMessage = "Bluetooth is turned off"
            isScanning = false
        case .unauthorized:
            statusMessage = "Bluetooth permission not granted"
        case .unsupported:
            statusMessage = "Bluetooth LE not supported"
        default:
            statusMessage = "Bluetooth state: \(central.state.rawValue)"
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let rssi = RSSI.intValue
        guard rssi != 127 else { return } // 127 = unavailable

        // Double-check the peripheral is actually advertising the Waxwing service.
        // scanForPeripherals(withServices:) is a hint, but CoreBluetooth can still
        // surface cached peripherals that aren't actively advertising our UUID.
        guard let serviceUUIDs = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID],
              serviceUUIDs.contains(WaxwingUUID.service) else {
            return
        }

        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String

        if let existing = nodesByPeripheralID[peripheral.identifier] {
            // Update existing node
            existing.rssi = rssi
            existing.lastSeen = Date()
            if let name = localName { existing.localName = name }
        } else {
            // New node discovered
            let node = WaxwingNode(peripheral: peripheral, rssi: rssi, localName: localName)
            nodesByPeripheralID[peripheral.identifier] = node
            discoveredNodes.append(node)
            statusMessage = "Found \(discoveredNodes.count) node(s)"
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }
        node.connectionState = .connected
        statusMessage = "Connected to \(node.displayName)"

        // Start service discovery
        peripheral.delegate = self
        peripheral.discoverServices([WaxwingUUID.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }
        let msg = error?.localizedDescription ?? "Unknown error"
        node.connectionState = .failed(msg)
        statusMessage = "Connection failed: \(msg)"

        if connectedNode?.id == node.id {
            connectedNode = nil
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }

        print("[BLE] didDisconnectPeripheral: \(node.displayName) periphState=\(peripheral.state.rawValue) error=\(error?.localizedDescription ?? "none")")

        // RACE GUARD: cancelPeripheralConnection is async, so the system can
        // deliver this disconnect callback AFTER the user has already tapped
        // Connect again. By that point peripheral.state will be .connecting
        // (or .connected) for the new attempt — this callback is stale and
        // we must NOT clobber the new connection's state. The peripheral
        // is only truly torn down when peripheral.state == .disconnected.
        guard peripheral.state == .disconnected else {
            print("[BLE] Ignoring stale disconnect — a new connection is already in flight")
            return
        }

        node.connectionState = .disconnected
        node.identity = nil

        // Use object identity (===) on the peripheral, not just the UUID, so
        // that even if some other path has put a fresh node into connectedNode
        // for the same identifier, we don't tear down its state.
        if let current = connectedNode, current.peripheral === peripheral {
            connectedNode = nil
            characteristicsByUUID.removeAll()
            notificationsReady = false

            // Fire the in-flight handler with a disconnect error so
            // multi-step operations (chunked read/write) can call their
            // completion handlers rather than silently disappearing.
            let handler = fileResponseHandler
            fileResponseHandler = nil
            fileResponseWatchdog?.cancel()
            fileResponseWatchdog = nil
            isFileOperationInProgress = false
            pendingOperations.removeAll()
            operationInFlight = false

            if let handler {
                // Synthesize a CBOR-like error response
                let errorResponse = CBORValue.map([
                    (.textString("error"), .textString("BLE disconnected"))
                ])
                handler(errorResponse)
            }

            statusMessage = "Disconnected from \(node.displayName)"
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }

        if let error = error {
            node.connectionState = .failed(error.localizedDescription)
            return
        }

        guard let service = peripheral.services?.first(where: { $0.uuid == WaxwingUUID.service }) else {
            node.connectionState = .failed("Waxwing service not found")
            return
        }

        // Discover all characteristics on the Waxwing service.
        // Passing nil avoids filtering to a fixed list, which ensures newly
        // added characteristics are always picked up (especially important
        // after firmware updates, since iOS caches the GATT structure).
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }

        if let error = error {
            node.connectionState = .failed(error.localizedDescription)
            return
        }

        // Reset our notification-ready flag for this fresh discovery; it
        // will be set true once didUpdateNotificationStateFor confirms.
        notificationsReady = false

        // Cache all discovered characteristics
        if let chars = service.characteristics {
            print("[BLE] Discovered \(chars.count) characteristics:")
            for char in chars {
                let props = char.properties
                var propList: [String] = []
                if props.contains(.read) { propList.append("READ") }
                if props.contains(.write) { propList.append("WRITE") }
                if props.contains(.writeWithoutResponse) { propList.append("WRITE_NR") }
                if props.contains(.notify) { propList.append("NOTIFY") }
                if props.contains(.indicate) { propList.append("INDICATE") }
                print("[BLE]   \(char.uuid) [\(propList.joined(separator: ", "))] isNotifying=\(char.isNotifying)")
                characteristicsByUUID[char.uuid] = char
            }
        }

        // Debug: confirm file characteristics were found
        let hasFileCmd = characteristicsByUUID[WaxwingUUID.fileCommand] != nil
        let hasFileResp = characteristicsByUUID[WaxwingUUID.fileResponse] != nil
        print("[BLE] File Command found: \(hasFileCmd), File Response found: \(hasFileResp)")

        // Subscribe to file response notifications. We do NOT proceed to
        // read Device Identity yet — we wait for didUpdateNotificationStateFor
        // to confirm the subscription is actually live before declaring the
        // node ready. Otherwise the very first file command's response
        // notification can be missed and the operation hangs.
        guard let respChar = characteristicsByUUID[WaxwingUUID.fileResponse] else {
            node.connectionState = .failed("File response characteristic missing")
            return
        }

        // On a reconnect, iOS may hand back a CACHED CBCharacteristic whose
        // isNotifying is sticky from the previous session. In that case
        // setNotifyValue(true) is a no-op and didUpdateNotificationStateFor
        // never fires, so our handshake stalls forever. Force a fresh
        // subscription by toggling off-then-on whenever it's already true.
        if respChar.isNotifying {
            print("[BLE] fileResponse already isNotifying; toggling off then on to force fresh CCCD")
            peripheral.setNotifyValue(false, for: respChar)
        }
        print("[BLE] Subscribing to fileResponse notifications")
        peripheral.setNotifyValue(true, for: respChar)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }

        if let error = error {
            print("[BLE] didUpdateNotificationStateFor error: \(error)")
            node.connectionState = .failed("Notify subscribe failed: \(error.localizedDescription)")
            return
        }

        print("[BLE] didUpdateNotificationStateFor \(characteristic.uuid): isNotifying=\(characteristic.isNotifying)")

        // We only gate readiness on the file response subscription becoming
        // active (the off→on toggle's "off" callback is intentionally ignored).
        guard characteristic.uuid == WaxwingUUID.fileResponse,
              characteristic.isNotifying else { return }

        notificationsReady = true

        // Notifications confirmed live — now safe to read Device Identity.
        guard let service = peripheral.services?.first(where: { $0.uuid == WaxwingUUID.service }),
              let identityChar = service.characteristics?.first(where: { $0.uuid == WaxwingUUID.deviceIdentity }) else {
            node.connectionState = .failed("Identity characteristic missing")
            return
        }

        node.connectionState = .readingIdentity
        statusMessage = "Reading device identity..."
        peripheral.readValue(for: identityChar)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let node = nodesByPeripheralID[peripheral.identifier] else { return }

        if let error = error {
            // Only fail the connection state if we're still reading identity
            if node.connectionState == .readingIdentity {
                node.connectionState = .failed("Read error: \(error.localizedDescription)")
            } else {
                fileOperationError = "Read error: \(error.localizedDescription)"
                isFileOperationInProgress = false
            }
            return
        }

        if characteristic.uuid == WaxwingUUID.deviceIdentity {
            handleDeviceIdentityRead(node: node, data: characteristic.value)
        } else if characteristic.uuid == WaxwingUUID.fileResponse {
            handleFileResponse(data: characteristic.value)
        }
    }

    private func handleFileResponse(data: Data?) {
        // CRITICAL: every exit path here MUST end up either calling the
        // pending fileResponseHandler or routing through
        // deliverFileResponseError. Returning silently leaves the
        // operation queue wedged forever (every queued upload / read /
        // listing will sit in pendingOperations and never run).
        guard let data = data, !data.isEmpty else {
            print("[BLE] handleFileResponse: empty notification — failing in-flight command")
            deliverFileResponseError("Empty file response")
            return
        }

        print("[BLE] handleFileResponse: \(data.count) bytes")

        guard let cbor = try? CBORDecoder.decode(data) else {
            // This is the smoking gun for an MTU overflow on the node:
            // a partial CBOR blob arrives that our decoder rejects.
            print("[BLE] handleFileResponse: CBOR decode failed for \(data.count) bytes")
            deliverFileResponseError("Failed to decode file response (\(data.count) bytes — possible MTU overflow)")
            return
        }

        let handler = fileResponseHandler
        fileResponseHandler = nil
        fileResponseWatchdog?.cancel()
        fileResponseWatchdog = nil
        isFileOperationInProgress = false
        handler?(cbor)
    }

    private func handleDeviceIdentityRead(node: WaxwingNode, data: Data?) {
        guard let data = data, !data.isEmpty else {
            node.connectionState = .failed("Empty identity response")
            return
        }

        if let identity = DeviceIdentity.fromCBOR(data) {
            node.identity = identity
            node.connectionState = .ready
            statusMessage = "Connected to \(node.displayName)"
        } else {
            node.connectionState = .failed("Failed to decode identity (got \(data.count) bytes)")
        }
    }
}
