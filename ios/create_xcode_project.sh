#!/bin/bash
#
# Create Xcode project for WaxwingCompanion
# This script creates the Xcode project using built-in tools.
#
# Usage: ./create_xcode_project.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Creating Xcode Project for WaxwingCompanion ==="
echo ""

# Create the project using xcodebuild -create (Xcode 15+)
if xcodebuild -version | grep -qE "Xcode [12][0-9]"; then
    echo "Creating Xcode project with built-in templates..."
    
    # Use xcodebuild to create a basic project structure
    # This creates the project at ios/WaxwingCompanion.xcodeproj
    xcodebuild -createTarget \
        -project ios/WaxwingCompanion.xcodeproj \
        -template "iOS\Application" \
        -name WaxwingCompanion \
        -path "$SCRIPT_DIR" \
        -bundleIdentifier "com.waxwing.WaxwingCompanion" \
        -deploymentTarget "17.0" \
        -language Swift \
        -uiInterface SwiftUI
    
    echo "Project created! Opening Xcode..."
    open "$SCRIPT_DIR/WaxwingCompanion.xcodeproj"
else
    echo "Xcode 15+ not found or not in PATH."
    echo ""
    echo "Please install Xcode from the Mac App Store and run:"
    echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
    echo ""
    echo "Then run this script again."
    exit 1
fi
