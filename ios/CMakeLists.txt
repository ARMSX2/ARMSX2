# CMake configuration for ARMSX2 iOS build
# iOS 26+ with JIT support

cmake_minimum_required(VERSION 3.20)

project(ARMSX2_iOS VERSION 1.0.0 LANGUAGES C CXX OBJCXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# iOS specific settings
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum iOS deployment version")
set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "26.0")
set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2") # iPhone and iPad

# Enable JIT compilation flags
add_compile_definitions(
    TARGET_IOS=1
    IOS_JIT_ENABLED=1
    PCSX2_TARGET_IOS=1
    ARM64=1
    _M_ARM64=1
)

# iOS SDK and architecture
set(CMAKE_OSX_SYSROOT "iphoneos")
set(CMAKE_OSX_ARCHITECTURES "arm64")

# Compiler flags for optimization and JIT
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffast-math")

# iOS specific compiler flags
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/pcsx2
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/common
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/3rdparty
    ${CMAKE_SOURCE_DIR}/ARMSX2/JIT
    ${CMAKE_SOURCE_DIR}/ARMSX2/Bridge
)

# Add subdirectories for the PCSX2 core
# This links to the existing C++ emulator code
set(PCSX2_SOURCE_DIR ${CMAKE_SOURCE_DIR}/../app/src/main/cpp)

# Include PCSX2 common library
if(EXISTS ${PCSX2_SOURCE_DIR}/common/CMakeLists.txt)
    add_subdirectory(${PCSX2_SOURCE_DIR}/common ${CMAKE_BINARY_DIR}/common)
endif()

# Include PCSX2 core
if(EXISTS ${PCSX2_SOURCE_DIR}/pcsx2/CMakeLists.txt)
    # Note: You may need to modify the PCSX2 CMakeLists.txt to support iOS
    # add_subdirectory(${PCSX2_SOURCE_DIR}/pcsx2 ${CMAKE_BINARY_DIR}/pcsx2)
endif()

# iOS-specific source files
set(IOS_SOURCES
    ${CMAKE_SOURCE_DIR}/ARMSX2/JIT/JITManager.mm
    ${CMAKE_SOURCE_DIR}/ARMSX2/Bridge/EmulatorBridge.mm
)

set(IOS_HEADERS
    ${CMAKE_SOURCE_DIR}/ARMSX2/JIT/JITManager.h
    ${CMAKE_SOURCE_DIR}/ARMSX2/Bridge/EmulatorBridge.h
)

# Create iOS library
add_library(ARMSX2_iOS_Core STATIC
    ${IOS_SOURCES}
    ${IOS_HEADERS}
)

# Link frameworks
target_link_libraries(ARMSX2_iOS_Core
    "-framework Foundation"
    "-framework UIKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework CoreGraphics"
    "-framework QuartzCore"
    "-framework AVFoundation"
    "-framework AudioToolbox"
    "-framework GameController"
)

# Set target properties
set_target_properties(ARMSX2_iOS_Core PROPERTIES
    FRAMEWORK TRUE
    MACOSX_FRAMEWORK_IDENTIFIER net.armsx2.core
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${IOS_HEADERS}"
)

# Installation
install(TARGETS ARMSX2_iOS_Core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    FRAMEWORK DESTINATION Frameworks
    PUBLIC_HEADER DESTINATION include
)

message(STATUS "ARMSX2 iOS Build Configuration")
message(STATUS "  iOS Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
message(STATUS "  Architecture: ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "  SDK: ${CMAKE_OSX_SYSROOT}")
message(STATUS "  JIT Enabled: YES")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
