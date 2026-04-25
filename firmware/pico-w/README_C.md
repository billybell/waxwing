# Waxwing Mesh Pico W C Firmware (Work in Progress)

This directory contains the ongoing work to port the Waxwing Mesh firmware from MicroPython to C for the Raspberry Pi Pico W.

## Current Status
- Basic identity management implemented (identity.c/h)
- Constants ported from Python (constants.h)
- Utility functions started (utils.c/h)
- Main entry point created (main.c)
- CMake build system configured

## Files Created So Far
```
src/
├── constants.h          // Ported from constants.py
├── identity.c           // Identity management
├── identity.h           // Identity management interface
├── main.c               // Entry point and basic initialization
├── utils.c              // Helper functions
├── utils.h              // Helper functions interface
└── CMakeLists.txt       // Build configuration
```

## Next Steps
1. Implement CBOR codec (cbor.c/h)
2. Implement message protocol (messages.c/h)
3. Implement BLE stack (ble.c/h)
4. Implement file store (filestore.c/h)
5. Complete main application logic

## Building
The firmware uses the Raspberry Pi Pico SDK. To build:

```bash
mkdir build
cd build
cmake ..
make
```

This will generate a waxwing_mesh.uf2 file that can be dragged onto the Pico W.

## Implementation Notes
- The identity module currently uses software-based random number generation and a SHA-256 placeholder
- In production, these should be replaced with hardware RNG and proper cryptographic acceleration
- The utils module contains placeholder implementations that should be replaced with Pico SDK functions
- Error handling needs to be strengthened throughout