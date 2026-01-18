# ARMSX2 for iOS

PlayStation 2 emulator for iOS 26+ with JIT compilation support.

## Features

- **iOS 26 JIT Support**: Full dynamic code generation for optimal performance
- **Metal Graphics**: Hardware-accelerated rendering using Metal API
- **Native iOS UI**: Built with SwiftUI for a modern iOS experience
- **Full PS2 Emulation**: Based on the PCSX2 emulator core
- **Game Controller Support**: Compatible with MFi controllers
- **Save States**: Quick save and load functionality
- **File Management**: Easy game and BIOS file management

## Requirements

### System Requirements
- **iOS 26.0 or later** (required for JIT support)
- **Device**: iPhone 12 or newer, iPad Air 4 or newer recommended
- **Storage**: At least 5GB free space
- **RAM**: 4GB minimum, 6GB+ recommended

### Development Requirements
- **Xcode 16.0 or later**
- **macOS 14.0 or later**
- **iOS 26 SDK**
- **Apple Developer account** (for device installation)

## Building from Source

### 1. Clone the Repository

```bash
git clone https://github.com/ARMSX2/ARMSX2.git
cd ARMSX2/ios
```

### 2. Open in Xcode

```bash
open ARMSX2.xcodeproj
```

Or use the build script:

```bash
./build_ios.sh Release
```

### 3. Configure Signing

1. Open the project in Xcode
2. Select the ARMSX2 target
3. Go to "Signing & Capabilities"
4. Select your development team
5. Xcode will automatically manage signing

### 4. Build and Run

- Select your target device or simulator
- Press Cmd+R to build and run

## JIT Configuration

### iOS 26 JIT Support

iOS 26 introduces enhanced JIT capabilities that ARMSX2 leverages for optimal performance:

- **Dynamic Code Generation**: Full JIT compilation for PS2 CPU recompilation
- **Enhanced Memory Permissions**: Better control over executable memory
- **Thread-level JIT Control**: Per-thread JIT enable/disable

### Required Entitlements

The app requires the following entitlements (already configured):

- `com.apple.security.cs.allow-jit` - Enable JIT compilation
- `com.apple.security.cs.allow-unsigned-executable-memory` - Allow dynamic code
- `com.apple.developer.kernel.extended-virtual-addressing` - Extended memory access
- `com.apple.developer.memory.extended-permissions` - JIT memory permissions

### Testing JIT

When the app launches, check the console for JIT status:

```
[ARMSX2-JIT] Initializing JIT Manager for iOS 26
[ARMSX2-JIT] JIT successfully initialized!
[ARMSX2-JIT] JIT permission granted!
```

## Usage

### First Launch

1. **Configure BIOS**
   - Tap the settings icon
   - Select "Select BIOS"
   - Choose your PS2 BIOS file (legally obtained from your PS2)

2. **Add Games**
   - Tap "Add Game" on the main screen
   - Select your PS2 ISO, BIN, or CHD file
   - Game will appear in your library

### Playing Games

1. Tap a game in your library
2. Emulator will load and start automatically
3. Use on-screen controls or connect a MFi controller
4. Tap the menu button (≡) for options

### Controls

**On-Screen Controls:**
- Left: D-Pad for directional input
- Right: Action buttons (✕, ○, □, △)

**Physical Controllers:**
- All MFi (Made for iPhone) controllers supported
- PlayStation DualSense/DualShock 4 supported
- Xbox controllers supported

### Save States

- Tap menu during gameplay
- Select "Save State" to save
- Select "Load State" to load
- Supports 10 save slots (0-9)

## Performance Optimization

### Best Practices

1. **Close Background Apps**: Free up RAM for better performance
2. **Enable JIT**: Ensure JIT is enabled in settings (default)
3. **Use Metal Renderer**: Default and fastest option
4. **Frame Limiting**: Keep enabled to prevent battery drain

### Expected Performance

- **iPhone 14 Pro and newer**: 60 FPS on most games
- **iPhone 13/14**: 45-60 FPS depending on game
- **iPhone 12**: 30-45 FPS, some games may struggle
- **iPad Pro (M1/M2)**: Excellent performance, 60 FPS

## Troubleshooting

### JIT Not Working

If you see "JIT Status: Failed" in settings:

1. Verify you're running iOS 26 or later
2. Reinstall the app
3. Restart your device
4. Check that entitlements are properly signed

### Games Won't Load

- Ensure BIOS is configured correctly
- Verify game file is a valid PS2 ISO/BIN/CHD
- Check available storage space
- Try a different game to isolate the issue

### Poor Performance

- Close all background apps
- Restart the app
- Lower graphics settings
- Ensure device is not overheating
- Check that JIT is enabled

### App Crashes

- Check iOS version (26.0+ required)
- Verify sufficient free storage (5GB+)
- Try a clean reinstall
- Check crash logs in Console app

## Project Structure

```
ios/
├── ARMSX2.xcodeproj/          # Xcode project
├── ARMSX2/
│   ├── Sources/               # Swift UI code
│   │   ├── ARMSX2App.swift   # App entry point
│   │   └── ContentView.swift # Main UI
│   ├── Bridge/                # Objective-C++ bridge
│   │   ├── EmulatorBridge.h
│   │   └── EmulatorBridge.mm
│   ├── JIT/                   # JIT manager
│   │   ├── JITManager.h
│   │   └── JITManager.mm
│   ├── Resources/             # Assets and resources
│   ├── Info.plist            # App configuration
│   └── ARMSX2.entitlements   # App entitlements
├── CMakeLists.txt            # CMake configuration
├── build_ios.sh              # Build script
└── README_iOS.md             # This file
```

## Architecture

### Components

1. **Swift UI Layer**: Modern iOS interface using SwiftUI
2. **Bridge Layer**: Objective-C++ bridge connecting Swift to C++
3. **JIT Manager**: iOS 26 JIT memory management
4. **Emulator Core**: PCSX2 C++ emulation engine
5. **Metal Renderer**: Hardware-accelerated graphics

### Data Flow

```
SwiftUI → EmulatorBridge → JITManager → PCSX2 Core → Metal Renderer
```

## Contributing

Contributions are welcome! Please see the main [CONTRIBUTING.md](../CONTRIBUTING.md) for guidelines.

### iOS-Specific Guidelines

- Use SwiftUI for all UI components
- Follow Apple's Human Interface Guidelines
- Test on real devices, not just simulator
- Ensure JIT works correctly on all iOS 26+ devices
- Optimize for battery life

## Legal

- ARMSX2 is licensed under GPLv3
- You must own a PS2 and legally dump your own BIOS
- Game ISOs must be from games you own
- This project is not affiliated with Sony

## Support

- **Issues**: [GitHub Issues](https://github.com/ARMSX2/ARMSX2/issues)
- **Discord**: [ARMSX2 Discord](https://discord.gg/KwAChKDctz)
- **Website**: [armsx2.net](https://armsx2.net)

## Acknowledgments

- **PCSX2 Team**: For the incredible PS2 emulator core
- **Apple**: For iOS 26 JIT improvements
- **Contributors**: Everyone who helped build ARMSX2

---

**Note**: iOS 26 is required for optimal JIT performance. Earlier iOS versions may work but with reduced performance.
