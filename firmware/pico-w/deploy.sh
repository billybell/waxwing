#!/usr/bin/env bash
# deploy.sh — Upload Waxwing firmware to a Pico W via mpremote
#
# Usage:
#   ./deploy.sh              # auto-detect Pico W
#   ./deploy.sh /dev/ttyACM0 # specify port (Linux)
#   ./deploy.sh /dev/cu.usbmodem* # macOS wildcard example
#
# Prerequisites:
#   pip install mpremote
#
# What it does:
#   1. Create /waxwing/ directory on the Pico
#   2. Upload waxwing/__init__.py and all firmware modules
#   3. Upload main.py
#   4. Soft-reset the Pico so the new firmware runs immediately

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-}"

# ---------------------------------------------------------------------------
# Resolve mpremote connection string
# ---------------------------------------------------------------------------
if [[ -n "$PORT" ]]; then
    CONN="connect $PORT"
else
    # Let mpremote auto-detect (works if only one Pico is connected)
    CONN=""
fi

MPR="mpremote $CONN"

echo "=== Waxwing Mesh Firmware Deploy ==="
echo "Source : $SCRIPT_DIR"
[[ -n "$PORT" ]] && echo "Port   : $PORT" || echo "Port   : auto-detect"
echo ""

# ---------------------------------------------------------------------------
# Helper: run mpremote command with error context
# ---------------------------------------------------------------------------
run() {
    echo "  >> mpremote $*"
    $MPR "$@"
}

# ---------------------------------------------------------------------------
# 1. Ensure directories exist on device
# ---------------------------------------------------------------------------
echo "[1/4] Creating directories on device..."
# mkdir returns an error if it already exists; suppress that
$MPR exec "
import os
for d in ('waxwing', 'files'):
    try:
        os.mkdir(d)
        print('  Created /' + d + '/')
    except OSError:
        print('  /' + d + '/ already exists')
" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 2. Upload waxwing package files
# ---------------------------------------------------------------------------
echo "[2/4] Uploading waxwing/ package..."
WAXWING_FILES=(
    "waxwing/__init__.py"
    "waxwing/constants.py"
    "waxwing/cbor.py"
    "waxwing/identity.py"
    "waxwing/messages.py"
    "waxwing/ble.py"
    "waxwing/filestore.py"
)

for f in "${WAXWING_FILES[@]}"; do
    echo "  Uploading $f"
    $MPR cp "$SCRIPT_DIR/$f" ":$f"
done

# ---------------------------------------------------------------------------
# 3. Upload main.py
# ---------------------------------------------------------------------------
echo "[3/4] Uploading main.py..."
$MPR cp "$SCRIPT_DIR/main.py" ":main.py"

# ---------------------------------------------------------------------------
# 4. Soft reset
# ---------------------------------------------------------------------------
echo "[4/4] Soft-resetting Pico W..."
$MPR reset

echo ""
echo "=== Deploy complete ==="
echo ""
echo "To monitor serial output:"
if [[ "$(uname)" == "Darwin" ]]; then
    echo "  mpremote connect $(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || echo '/dev/cu.usbmodem*') repl"
else
    echo "  mpremote repl"
fi
echo ""
echo "To wipe the identity and start fresh:"
echo "  mpremote exec \"from waxwing.identity import wipe; wipe()\""
