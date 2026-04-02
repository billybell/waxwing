# Waxwing Mesh — Tools

Development and testing utilities. To be built as the firmware and app implementations progress.

## Planned Tools

### `ble-scanner`
A Python script (using `bleak` library) that scans for Waxwing nodes, reads their Device Identity and Manifest characteristics, and prints a human-readable summary. Useful for verifying firmware advertisement and GATT service correctness.

```
python ble-scanner/scan.py
```

### `manifest-gen`
A Python script that generates test manifests with synthetic file metadata, for testing manifest exchange without needing a live node.

### `transfer-test`
A Python script that connects to a node as a GATT Central and runs a file transfer, verifying chunk sequencing, integrity, and resume behaviour.

### `reputation-inspect`
A Python script that reads and prints the reputation ledger from a connected node's Reputation Exchange characteristic.

### `wire-transfer-test`
A Python script that connects to a node's WiFi transfer server and tests the Waxwing Wire Transfer protocol.

## Requirements

- Python 3.10+
- `bleak` (cross-platform BLE library: `pip install bleak`)
- `cbor2` (`pip install cbor2`)
- `cryptography` (`pip install cryptography`)
