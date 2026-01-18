#!/bin/bash
# Build script for ARMSX2 iOS
# Requires Xcode 16+ and iOS 26 SDK

set -e

echo "================================"
echo "ARMSX2 iOS Build Script"
echo "Target: iOS 26+ with JIT"
echo "================================"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if Xcode is installed
if ! command -v xcodebuild &> /dev/null; then
    echo -e "${RED}Error: Xcode not found. Please install Xcode 16 or later.${NC}"
    exit 1
fi

# Check Xcode version
XCODE_VERSION=$(xcodebuild -version | grep "Xcode" | cut -d ' ' -f2 | cut -d '.' -f1)
if [ "$XCODE_VERSION" -lt 16 ]; then
    echo -e "${YELLOW}Warning: Xcode 16 or later recommended for iOS 26 support${NC}"
fi

# Configuration
BUILD_TYPE=${1:-Release}
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
DERIVED_DATA_DIR="${BUILD_DIR}/DerivedData"

echo -e "${GREEN}Build Type: ${BUILD_TYPE}${NC}"
echo -e "${GREEN}Project Directory: ${PROJECT_DIR}${NC}"
echo -e "${GREEN}Build Directory: ${BUILD_DIR}${NC}"

# Clean previous build (optional)
if [ "$2" == "clean" ]; then
    echo -e "${YELLOW}Cleaning previous build...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
mkdir -p "${DERIVED_DATA_DIR}"

# Build using xcodebuild
echo -e "${GREEN}Building ARMSX2 for iOS...${NC}"

xcodebuild \
    -project "${PROJECT_DIR}/ARMSX2.xcodeproj" \
    -scheme ARMSX2 \
    -configuration ${BUILD_TYPE} \
    -sdk iphoneos \
    -derivedDataPath "${DERIVED_DATA_DIR}" \
    -destination 'generic/platform=iOS' \
    CODE_SIGN_IDENTITY="" \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGNING_ALLOWED=NO \
    ONLY_ACTIVE_ARCH=NO \
    ARCHS="arm64" \
    IPHONEOS_DEPLOYMENT_TARGET=26.0 \
    build

if [ $? -eq 0 ]; then
    echo -e "${GREEN}================================${NC}"
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "${GREEN}================================${NC}"

    # Find the built app
    APP_PATH=$(find "${DERIVED_DATA_DIR}" -name "ARMSX2.app" -type d | head -1)
    if [ -n "$APP_PATH" ]; then
        echo -e "${GREEN}App location: ${APP_PATH}${NC}"
        echo -e "${YELLOW}Note: To install on device, you need to sign the app with a valid certificate${NC}"
    fi
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

# Optional: Build CMake-based components
echo ""
echo -e "${YELLOW}Building CMake components...${NC}"
cd "${BUILD_DIR}"

cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    "${PROJECT_DIR}"

cmake --build . --config ${BUILD_TYPE}

echo ""
echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}All builds complete!${NC}"
echo -e "${GREEN}================================${NC}"
