//
//  PCSX2Wrapper.h
//  ARMSX2
//
//  C++ wrapper for PCSX2 emulator core
//

#ifndef PCSX2Wrapper_h
#define PCSX2Wrapper_h

#include <string>

namespace PCSX2Wrapper
{
    /// Initialize PCSX2 core
    /// @param biosPath Path to PS2 BIOS file
    /// @param dataPath Path to data directory for saves, etc.
    /// @return true if successful
    bool Initialize(const std::string& biosPath, const std::string& dataPath);

    /// Shutdown PCSX2 core
    void Shutdown();

    /// Load a game
    /// @param gamePath Path to ISO/BIN/CHD file
    /// @return true if successful
    bool LoadGame(const std::string& gamePath);

    /// Start emulation
    void Start();

    /// Pause emulation
    void Pause();

    /// Resume emulation
    void Resume();

    /// Stop emulation and unload game
    void Stop();

    /// Reset emulation
    void Reset();

    /// Run one frame of emulation
    void RunFrame();

    /// Save state
    /// @param slot Save state slot (0-9)
    /// @return true if successful
    bool SaveState(int slot);

    /// Load state
    /// @param slot Save state slot (0-9)
    /// @return true if successful
    bool LoadState(int slot);

    /// Get current FPS
    /// @return FPS value
    double GetFPS();

    /// Get current frame count
    /// @return Frame count
    uint64_t GetFrameCount();

    /// Check if emulator is running
    /// @return true if running
    bool IsRunning();

    /// Check if emulator is paused
    /// @return true if paused
    bool IsPaused();

    /// Set emulation speed (1.0 = 100%)
    void SetSpeed(double speed);

    /// Get emulation speed
    double GetSpeed();

    /// Set renderer (Metal, OpenGL, etc.)
    void SetRenderer(const std::string& renderer);

    /// Get current renderer
    std::string GetRenderer();

    /// Take screenshot
    /// @param path Output path
    /// @return true if successful
    bool TakeScreenshot(const std::string& path);

} // namespace PCSX2Wrapper

#endif /* PCSX2Wrapper_h */
