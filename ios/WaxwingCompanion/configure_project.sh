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

# Idempotent insert helpers — skip if the key already exists.
key_exists() {
    plutil -extract "$1" xml1 -o /dev/null "${INFO_PLIST}" 2>/dev/null
}

add_plist_key() {
    local key="$1"; local value="$2"
    if key_exists "$key"; then
        echo "Key '$key' already exists, skipping..."
    else
        plutil -insert "$key" -string "$value" "${INFO_PLIST}"
        echo "Added: $key"
    fi
}

add_plist_bool() {
    local key="$1"; local value="$2"   # YES or NO
    if key_exists "$key"; then
        echo "Key '$key' already exists, skipping..."
    else
        plutil -insert "$key" -bool "$value" "${INFO_PLIST}"
        echo "Added: $key (bool)"
    fi
}

add_plist_empty_dict() {
    local key="$1"
    if key_exists "$key"; then
        echo "Key '$key' already exists, skipping..."
    else
        plutil -insert "$key" -dictionary "${INFO_PLIST}"
        echo "Added: $key (dict)"
    fi
}

add_plist_string_array() {
    local key="$1"; shift
    if key_exists "$key"; then
        echo "Key '$key' already exists, skipping..."
        return
    fi
    plutil -insert "$key" -array "${INFO_PLIST}"
    local i=0
    for item in "$@"; do
        plutil -insert "${key}.${i}" -string "$item" "${INFO_PLIST}"
        i=$((i + 1))
    done
    echo "Added: $key (array, ${i} items)"
}

echo ""
echo "Adding standard bundle keys..."

# Required for device install (Xcode does not auto-inject these when
# INFOPLIST_FILE points at a manual plist).
add_plist_key   "CFBundleDevelopmentRegion"    '$(DEVELOPMENT_LANGUAGE)'
add_plist_key   "CFBundleDisplayName"          "Waxwing"
add_plist_key   "CFBundleExecutable"           '$(EXECUTABLE_NAME)'
add_plist_key   "CFBundleIdentifier"           '$(PRODUCT_BUNDLE_IDENTIFIER)'
add_plist_key   "CFBundleInfoDictionaryVersion" "6.0"
add_plist_key   "CFBundleName"                 '$(PRODUCT_NAME)'
add_plist_key   "CFBundlePackageType"          '$(PRODUCT_BUNDLE_PACKAGE_TYPE)'
add_plist_key   "CFBundleShortVersionString"   "1.0"
add_plist_key   "CFBundleVersion"              "1"
add_plist_bool  "LSRequiresIPhoneOS"           YES

echo ""
echo "Adding launch screen and supported orientations..."

# Empty UILaunchScreen dict satisfies the "needs launch storyboard" check.
add_plist_empty_dict "UILaunchScreen"

# iPhone: portrait + both landscapes (no upside-down).
add_plist_string_array "UISupportedInterfaceOrientations" \
    "UIInterfaceOrientationPortrait" \
    "UIInterfaceOrientationLandscapeLeft" \
    "UIInterfaceOrientationLandscapeRight"

# iPad: all four orientations.
add_plist_string_array "UISupportedInterfaceOrientations~ipad" \
    "UIInterfaceOrientationPortrait" \
    "UIInterfaceOrientationPortraitUpsideDown" \
    "UIInterfaceOrientationLandscapeLeft" \
    "UIInterfaceOrientationLandscapeRight"

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
echo "Added/verified:"
echo "  - Standard bundle keys (CFBundleIdentifier, executable, version, ...)"
echo "  - LSRequiresIPhoneOS"
echo "  - UILaunchScreen + UISupportedInterfaceOrientations (iPhone & iPad)"
echo "  - Bluetooth LE (scan + peripheral)"
echo "  - Camera"
echo "  - Location (when in use)"
echo "  - Photo Library (read + write)"
echo ""
echo "Next: Set your Development Team in Xcode → Signing & Capabilities"
