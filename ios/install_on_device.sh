#!/bin/bash
# Install ARMSX2 on iOS device
# Requires: libimobiledevice, ios-deploy

set -e

echo "================================"
echo "ARMSX2 iOS Device Installer"
echo "================================"

# Check if ios-deploy is installed
if ! command -v ios-deploy &> /dev/null; then
    echo "Error: ios-deploy not found."
    echo "Install with: brew install ios-deploy"
    exit 1
fi

# Find the built app
APP_PATH=$(find build/DerivedData -name "ARMSX2.app" -type d | head -1)

if [ -z "$APP_PATH" ]; then
    echo "Error: ARMSX2.app not found. Build the project first."
    echo "Run: ./build_ios.sh"
    exit 1
fi

echo "Found app: $APP_PATH"

# Check for connected device
echo "Looking for connected iOS device..."
DEVICE_ID=$(ios-deploy -c 2>/dev/null | grep "Found" | awk '{print $2}')

if [ -z "$DEVICE_ID" ]; then
    echo "Error: No iOS device found."
    echo "Please connect your device and trust this computer."
    exit 1
fi

echo "Found device: $DEVICE_ID"

# Install the app
echo "Installing ARMSX2 on device..."
ios-deploy --bundle "$APP_PATH" --id "$DEVICE_ID" --justlaunch

if [ $? -eq 0 ]; then
    echo "================================"
    echo "Installation successful!"
    echo "================================"
    echo ""
    echo "Note: If the app doesn't launch:"
    echo "1. Go to Settings > General > VPN & Device Management"
    echo "2. Trust your developer certificate"
    echo "3. Launch ARMSX2 from the home screen"
else
    echo "Installation failed!"
    exit 1
fi
