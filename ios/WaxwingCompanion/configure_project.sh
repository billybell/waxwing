#!/bin/bash
#
# Waxwing iOS Project Configuration Script
# Adds privacy permissions to Info.plist after project creation.
#
# Usage: ./configure_project.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="${SCRIPT_DIR}/WaxwingCompanion"
INFO_PLIST="${SOURCE_DIR}/Info.plist"

echo "=== Configuring Waxwing iOS Project ==="

# Check if Info.plist exists
if [ ! -f "${INFO_PLIST}" ]; then
    echo "Error: Info.plist not found at ${INFO_PLIST}"
    echo "Please create the Xcode project first, then run this script."
    exit 1
fi

echo "Found Info.plist at: ${INFO_PLIST}"

# Add privacy keys if they don't exist
add_plist_key() {
    local key="$1"
    local value="$2"

    if plutil -extract "$key" xml1 -o /dev/null "${INFO_PLIST}" 2>/dev/null; then
        echo "Key '$key' already exists, skipping..."
    else
        plutil -insert "$key" -string "$value" "${INFO_PLIST}"
        echo "Added: $key"
    fi
}

echo ""
echo "Adding privacy permissions..."

# Bluetooth
add_plist_key "NSBluetoothAlwaysUsageDescription" "Waxwing scans for and connects to nearby Waxwing mesh nodes over Bluetooth LE."
add_plist_key "NSBluetoothPeripheralUsageDescription" "Waxwing uses Bluetooth to communicate with nearby mesh nodes."

# Camera
add_plist_key "NSCameraUsageDescription" "Waxwing uses the camera to capture photos for uploading to your Waxwing mesh node."

# Location
add_plist_key "NSLocationWhenInUseUsageDescription" "Your location is used to geo-tag images you share with the Waxwing mesh."

# Photo Library
add_plist_key "NSPhotoLibraryUsageDescription" "Waxwing needs photo library access to select photos for upload."
add_plist_key "NSPhotoLibraryAddUsageDescription" "Waxwing saves processed images to your photo library."

echo ""
echo "=== Configuration Complete! ==="
echo ""
echo "Added permissions:"
echo "  - Bluetooth LE (scan + peripheral)"
echo "  - Camera"
echo "  - Location (when in use)"
echo "  - Photo Library (read + write)"
echo ""
echo "Next: Set your Development Team in Xcode → Signing & Capabilities"
