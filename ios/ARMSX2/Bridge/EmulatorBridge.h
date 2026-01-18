//
//  EmulatorBridge.h
//  ARMSX2
//
//  Bridge between Swift UI and C++ PCSX2 emulator core
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, EmulatorState) {
    EmulatorStateStopped = 0,
    EmulatorStateRunning = 1,
    EmulatorStatePaused = 2,
    EmulatorStateLoading = 3,
    EmulatorStateError = 4
};

@protocol EmulatorBridgeDelegate <NSObject>
@optional
/// Called when emulator state changes
- (void)emulatorStateDidChange:(EmulatorState)state;

/// Called when an error occurs
- (void)emulatorDidEncounterError:(NSString *)error;

/// Called with frame updates (for FPS counter, etc.)
- (void)emulatorDidUpdateFrame:(double)fps;
@end

@interface EmulatorBridge : NSObject

/// Shared singleton instance
@property (class, readonly, nonatomic) EmulatorBridge *sharedBridge;

/// Current emulator state
@property (readonly, nonatomic) EmulatorState state;

/// Delegate for emulator events
@property (weak, nonatomic, nullable) id<EmulatorBridgeDelegate> delegate;

/// Current FPS
@property (readonly, nonatomic) double currentFPS;

/// Whether emulator is initialized
@property (readonly, nonatomic) BOOL isInitialized;

/// Initialize the emulator core
/// @param biosPath Path to PS2 BIOS file
/// @return YES if successful, NO otherwise
- (BOOL)initializeWithBIOSPath:(NSString *)biosPath error:(NSError **)error;

/// Load and start a game
/// @param gamePath Path to game ISO/CHD file
/// @return YES if successful, NO otherwise
- (BOOL)loadGame:(NSString *)gamePath error:(NSError **)error;

/// Start/resume emulation
- (void)start;

/// Pause emulation
- (void)pause;

/// Stop emulation and unload game
- (void)stop;

/// Reset emulation
- (void)reset;

/// Save state to file
/// @param slot Save state slot (0-9)
/// @return YES if successful
- (BOOL)saveState:(NSInteger)slot;

/// Load state from file
/// @param slot Save state slot (0-9)
/// @return YES if successful
- (BOOL)loadState:(NSInteger)slot;

/// Update emulator settings
- (void)updateSettings:(NSDictionary *)settings;

/// Get current emulator info
- (NSDictionary *)getEmulatorInfo;

/// Setup rendering surface
/// @param view The UIView to render to
- (void)setupRenderSurface:(UIView *)view;

/// Handle touch input
- (void)touchBegan:(UITouch *)touch;
- (void)touchMoved:(UITouch *)touch;
- (void)touchEnded:(UITouch *)touch;

/// Virtual controller button press
- (void)pressButton:(NSString *)button;
- (void)releaseButton:(NSString *)button;

@end

NS_ASSUME_NONNULL_END
