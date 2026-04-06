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

    /// Queue of top-level file operations waiting to run.
    /// Each entry is a closure that kicks off the operation; it will be
    /// called only after the previous operation has fully completed.
    private var pendingOperations: [() -> Void] = []

    /// Whether a multi-step file operation (e.g. chunked read/write)
    /// is currently in progress and owns the BLE command channel.
    private var operationInFlight = false

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

        node.connectionState = .connecting
        connectedNode = node
        statusMessage = "Connecting to \(node.displayName)..."

        centralManager.connect(node.peripheral, options: nil)
    }

    func disconnect() {
        guard let node = connectedNode else { return }
        centralManager.cancelPeripheralConnection(node.peripheral)
        node.connectionState = .disconnected
        node.identity = nil
        connectedNode = nil
        characteristicsByUUID.removeAll()

        // Fire any in-flight handler so operations fail cleanly
        let handler = fileResponseHandler
        fileResponseHandler = nil
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
    func listFiles() {
        enqueueOperation { [weak self] in
            guard let self else { return }
            self.sendFileCommand(["cmd": "ls"]) { [weak self] response in
                guard let self else { return }
                defer { self.finishOperation() }
                if let error = response["error"]?.stringValue {
                    self.fileOperationError = error
                    return
                }
                guard case .array(let items)? = response["files"] else {
                    self.fileOperationError = "Invalid file list response"
                    return
                }
                self.fileList = items.compactMap { item -> NodeFile? in
                    guard let name = item["name"]?.stringValue else { return nil }
                    let size = item["size"]?.uintValue ?? 0
                    return NodeFile(name: name, size: Int(size))
                }
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
        guard connectedNode?.connectionState == .ready else {
            fileOperationError = "Not connected to a node"
            return
        }
        guard let char = characteristicsByUUID[WaxwingUUID.fileCommand] else {
            fileOperationError = "File command characteristic not found"
            return
        }
        guard let peripheral = connectedNode?.peripheral else { return }

        isFileOperationInProgress = true
        fileOperationError = nil
        fileResponseHandler = { [weak self] response in
            self?.isFileOperationInProgress = false
            handler(response)
        }

        let data = CBOREncoder.encode(command)
        peripheral.writeValue(data, for: char, type: .withResponse)
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
        node.connectionState = .disconnected
        node.identity = nil

        if connectedNode?.id == node.id {
            connectedNode = nil
            characteristicsByUUID.removeAll()

            // Fire the in-flight handler with a disconnect error so
            // multi-step operations (chunked read/write) can call their
            // completion handlers rather than silently disappearing.
            let handler = fileResponseHandler
            fileResponseHandler = nil
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
                print("[BLE]   \(char.uuid) [\(propList.joined(separator: ", "))]")
                characteristicsByUUID[char.uuid] = char
            }
        }

        // Debug: confirm file characteristics were found
        let hasFileCmd = characteristicsByUUID[WaxwingUUID.fileCommand] != nil
        let hasFileResp = characteristicsByUUID[WaxwingUUID.fileResponse] != nil
        print("[BLE] File Command found: \(hasFileCmd), File Response found: \(hasFileResp)")

        // Subscribe to file response notifications immediately so they're
        // ready before the first file command is sent (setNotifyValue is async)
        if let respChar = characteristicsByUUID[WaxwingUUID.fileResponse] {
            peripheral.setNotifyValue(true, for: respChar)
        }

        // Read Device Identity
        if let identityChar = service.characteristics?.first(where: { $0.uuid == WaxwingUUID.deviceIdentity }) {
            node.connectionState = .readingIdentity
            statusMessage = "Reading device identity..."
            peripheral.readValue(for: identityChar)
        }
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
        guard let data = data, !data.isEmpty else {
            fileOperationError = "Empty file response"
            isFileOperationInProgress = false
            return
        }

        guard let cbor = try? CBORDecoder.decode(data) else {
            fileOperationError = "Failed to decode file response"
            isFileOperationInProgress = false
            return
        }

        if let handler = fileResponseHandler {
            fileResponseHandler = nil
            handler(cbor)
        }
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
