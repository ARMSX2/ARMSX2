// ARMSX2Bridge.h — ObjC bridge for C++ emulator control
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, ARMSX2EmulatorState) {
    ARMSX2EmulatorStateStopped = 0,
    ARMSX2EmulatorStateRunning,
    ARMSX2EmulatorStatePaused,
    ARMSX2EmulatorStateSaving,
    ARMSX2EmulatorStateSuspended,
};

typedef NS_ENUM(NSInteger, ARMSX2CoreType) {
    ARMSX2CoreTypeLegacyRecompiler = 0,
    ARMSX2CoreTypeInterpreter = 1,
    ARMSX2CoreTypeARM64JIT = 2,
    ARMSX2CoreTypeJIT = ARMSX2CoreTypeARM64JIT,
};

typedef NS_ENUM(NSInteger, ARMSX2PadButton) {
    ARMSX2PadButtonUp = 0,
    ARMSX2PadButtonDown,
    ARMSX2PadButtonLeft,
    ARMSX2PadButtonRight,
    ARMSX2PadButtonCross,
    ARMSX2PadButtonCircle,
    ARMSX2PadButtonSquare,
    ARMSX2PadButtonTriangle,
    ARMSX2PadButtonL1,
    ARMSX2PadButtonR1,
    ARMSX2PadButtonL2,
    ARMSX2PadButtonR2,
    ARMSX2PadButtonStart,
    ARMSX2PadButtonSelect,
    ARMSX2PadButtonL3,
    ARMSX2PadButtonR3,
};

@interface ARMSX2SaveStateSlotInfo : NSObject
@property (nonatomic, assign) NSInteger slot;
@property (nonatomic, assign) BOOL occupied;
@property (nonatomic, copy, nonnull) NSString *filePath;
@property (nonatomic, copy, nonnull) NSString *fileName;
@property (nonatomic, strong, nullable) NSDate *modifiedDate;
@property (nonatomic, strong, nullable) NSData *previewPNGData;
@end

@interface ARMSX2BIOSInfo : NSObject
@property (nonatomic, copy, nonnull) NSString *fileName;
@property (nonatomic, copy, nonnull) NSString *filePath;
@property (nonatomic, copy, nonnull) NSString *regionName;
@property (nonatomic, copy, nonnull) NSString *countryCode;
@property (nonatomic, copy, nonnull) NSString *descriptionText;
@property (nonatomic, assign) NSInteger regionCode;
@property (nonatomic, assign) BOOL valid;
@end

@interface ARMSX2RetroAchievementsToastInfo : NSObject
@property (nonatomic, copy, nonnull) NSString *title;
@property (nonatomic, copy, nonnull) NSString *message;
@property (nonatomic, copy, nonnull) NSString *badgePath;
@property (nonatomic, assign) NSTimeInterval duration;
@end

typedef void (^ARMSX2SaveStateCompletion)(BOOL success);
typedef void (^ARMSX2RetroAchievementsCompletion)(BOOL success, NSString * _Nonnull message);

@interface ARMSX2Bridge : NSObject

// Game render view (for UIViewRepresentable)
+ (nonnull UIView *)gameRenderView;
+ (void)prepareGameRenderViewForCurrentRenderer;

// Lifecycle
+ (void)saveAllState;  // NVM + MC

// Pad input
+ (void)setPadButton:(ARMSX2PadButton)button pressed:(BOOL)pressed;
+ (void)setLeftStickX:(float)x Y:(float)y;
+ (void)setRightStickX:(float)x Y:(float)y;

// VM control
+ (void)requestVMStop;
+ (void)setVMPaused:(BOOL)paused;
+ (void)setFullScreen:(BOOL)enabled;
+ (BOOL)isSDLFullscreen;

// Info
+ (nonnull NSString *)biosName;
+ (nonnull NSString *)buildVersion;
+ (BOOL)isJITAvailable;
+ (BOOL)isNoJITFallbackActive;
+ (BOOL)isIdleVMPrewarmResolved;
+ (nonnull NSArray<NSURL *> *)extractControllerSkinArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory
    NS_SWIFT_NAME(extractControllerSkinArchive(at:to:));

+ (nullable NSData *)peekSkinManifestDataAtURL:(nonnull NSURL *)archiveURL
    NS_SWIFT_NAME(peekSkinManifestData(at:));

+ (nonnull NSArray<NSURL *> *)extractSkinPackageArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory
    NS_SWIFT_NAME(extractSkinPackageArchive(at:to:));

// Preserves the archive tree, so each .slangp still resolves its stages by relative path.
// Empty with `error` set on failure; count the returned .slangp URLs to tell a real pack.
+ (nonnull NSArray<NSURL *> *)extractShaderPackArchiveAtURL:(nonnull NSURL *)archiveURL
                                                toDirectory:(nonnull NSURL *)destinationDirectory
                                                      error:(NSError * _Nullable * _Nullable)error
    NS_SWIFT_NAME(extractShaderPackArchive(at:to:error:));

// Extracts the first .ps2 file from a ZIP into the memory-card directory.
+ (nullable NSString *)extractMemoryCardArchiveAtURL:(nonnull NSURL *)archiveURL
    NS_SWIFT_NAME(extractMemoryCardArchive(at:));

// OSD overlay
+ (void)setPerformanceOverlayVisible:(BOOL)visible;
+ (BOOL)isPerformanceOverlayVisible;
+ (void)applyOsdPreset:(int)preset;  // 0=off, 1=simple, 2=detail, 3=full

// Permanently releases the selected optional runtime resources for the current VM session.
+ (void)releaseNonEmulationResources:(NSUInteger)releaseFlags;
+ (BOOL)isEmulationOnlyModeActive;

// Accessibility: structured device stats for the VoiceOver HUD mirror.
+ (nonnull NSDictionary<NSString *, id> *)deviceStatsForAccessibility;

// Device haptic fallback for game rumble when no rumble-capable controller is connected.
+ (void)triggerDeviceHapticLarge:(NSUInteger)large small:(NSUInteger)small;

// Audio
+ (int)emulatorVolumePercent;
+ (void)setEmulatorVolumePercent:(int)value;

// ISO management
+ (nullable NSString *)currentISOPath;
+ (nullable NSString *)currentGameISOName;
+ (nonnull NSString *)isoDirectory;
+ (nonnull NSString *)documentsDirectory;
+ (nonnull NSArray<NSString *> *)availableISOs;
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)availableISOEntries;
+ (nonnull NSDictionary<NSString *, NSString *> *)gameMetadataForISO:(nonnull NSString *)isoName;
+ (nonnull NSDictionary<NSString *, id> *)gameSettingsForISO:(nullable NSString *)isoName NS_SWIFT_NAME(gameSettings(forISO:));
+ (nullable NSDictionary<NSString *, id> *)gameSettingsForCurrentGame;
+ (void)setGameSettings:(nonnull NSDictionary<NSString *, id> *)settings forISO:(nullable NSString *)isoName NS_SWIFT_NAME(setGameSettings(_:forISO:));
+ (nullable NSString *)linkedDiscPathForELF:(nonnull NSString *)elfName NS_SWIFT_NAME(linkedDiscPath(forELF:));
+ (void)setLinkedDiscPath:(nullable NSString *)discPath forELF:(nonnull NSString *)elfName NS_SWIFT_NAME(setLinkedDiscPath(_:forELF:));
+ (nonnull NSString *)clearCacheForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(clearCache(forISO:));
+ (nonnull NSString *)deleteGameDataForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(deleteGameData(forISO:));
+ (BOOL)deleteISO:(nonnull NSString *)isoName deleteGameData:(BOOL)deleteGameData NS_SWIFT_NAME(deleteISO(_:deleteGameData:));
+ (void)changeDiscToISO:(nonnull NSString *)isoName completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(changeDisc(toISO:completion:));
+ (void)ejectDiscWithCompletion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(ejectDisc(completion:));

// ISO boot
+ (BOOL)canResolveISO:(nonnull NSString *)isoName NS_SWIFT_NAME(canResolveISO(_:));
+ (void)bootISO:(nonnull NSString *)isoName;

// BIOS management
+ (nonnull NSString *)biosDirectory;
+ (nonnull NSArray<ARMSX2BIOSInfo *> *)availableBIOSInfos;
+ (nonnull NSString *)defaultBIOSName;
+ (void)setDefaultBIOS:(nonnull NSString *)biosName;

// Favorites
+ (BOOL)isFavorite:(nonnull NSString *)isoName;
+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite;

// INI generic getter/setter
+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def;
+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def;
+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def;
+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value;
+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value;
+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value;
+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value;
+ (void)clearINISection:(nonnull NSString *)section;
+ (void)applyGraphicsSettingsNow;
+ (void)flushINISettings;

// What the running game is actually using for each upscaling hack, keyed by INI key.
// Each entry has "effective", "reason" (ARMSX2GraphicsHackReason) and "pinned". The
// masks and the GameDB sit between the INI and the renderer, so this is the only way
// the settings screen can tell whether a toggle did anything. Refreshed on the
// ARMSX2GraphicsHackStateChanged notification.
+ (nonnull NSDictionary<NSString *, id> *)graphicsHackState;
+ (void)setGraphicsHackPinned:(nonnull NSString *)iniKey pinned:(BOOL)pinned NS_SWIFT_NAME(setGraphicsHackPinned(_:pinned:));

// Frame-time history (read-only). frameTimeHistory wraps
// PerformanceMetrics::GetFrameTimeHistory() (a thread-safe read of the
// 150-sample ring buffer) and frameTimeHistoryPos returns its current write
// cursor. Used by the adaptive-resolution controller to read the freshest
// samples before the cursor.
+ (nonnull NSData *)frameTimeHistory;
+ (NSUInteger)frameTimeHistoryPos;

// MetalFX Spatial upscaler availability probe. Returns YES only on iOS 16+ with
// a device GPU that reports MetalFX support (NO on the simulator and unsupported
// hardware). Used by the settings UI to hide the Upscaler option where unusable.
+ (BOOL)isMetalFXSupported;

// Whether this build includes librashader. Without cargo it is compiled out and Settings hides Shaders.
+ (BOOL)isShaderChainSupported;

// A .slangp preset's parameters as a JSON array of {name, description, initial, minimum,
// maximum, step} in declaration order, or "[]" for none. nil when this build has no
// librashader or the preset won't load, with librashader's message in error. Reads files,
// so call it off the main thread.
+ (nullable NSString *)shaderPresetParametersAtPath:(nonnull NSString *)path error:(NSError * _Nullable * _Nullable)error NS_SWIFT_NAME(shaderPresetParameters(atPath:));

// Queues parameter values for the chain built from preset; the GS thread applies them before its next frame.
+ (void)setShaderChainParameters:(nonnull NSDictionary<NSString *, NSNumber *> *)params forPreset:(nonnull NSString *)preset NS_SWIFT_NAME(setShaderChainParameters(_:forPreset:));

// Lets the renderer load a preset that failed before once more, on its next frame.
+ (void)retryShaderChain;

// librashader's message when the last chain built for path failed, or nil.
+ (nullable NSError *)shaderChainErrorForPreset:(nonnull NSString *)path NS_SWIFT_NAME(shaderChainError(forPreset:));

// Per-game INI access — reads/writes the per-game INI file
// (EmuFolders::GameSettings/<serial>_<crc>.ini) used by the game-settings and
// patch-enable-list helpers. Pass nil for isoName to target the running game (live-applies).
+ (BOOL)hasPerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName NS_SWIFT_NAME(hasPerGameINIValue(_:key:forISO:));
// Presence and value in one read; nil when the per-game INI has no entry.
+ (nullable NSNumber *)perGameINIBoolIfPresent:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName NS_SWIFT_NAME(perGameINIBoolIfPresent(_:key:forISO:));
+ (int)getPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def forISO:(nullable NSString *)isoName NS_SWIFT_NAME(getPerGameINIInt(_:key:defaultValue:forISO:));
+ (BOOL)getPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def forISO:(nullable NSString *)isoName NS_SWIFT_NAME(getPerGameINIBool(_:key:defaultValue:forISO:));
+ (float)getPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def forISO:(nullable NSString *)isoName NS_SWIFT_NAME(getPerGameINIFloat(_:key:defaultValue:forISO:));
+ (nonnull NSString *)getPerGameINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def forISO:(nullable NSString *)isoName NS_SWIFT_NAME(getPerGameINIString(_:key:defaultValue:forISO:));
+ (void)setPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value forISO:(nullable NSString *)isoName NS_SWIFT_NAME(setPerGameINIInt(_:key:value:forISO:));
+ (void)setPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value forISO:(nullable NSString *)isoName NS_SWIFT_NAME(setPerGameINIBool(_:key:value:forISO:));
+ (void)setPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value forISO:(nullable NSString *)isoName NS_SWIFT_NAME(setPerGameINIFloat(_:key:value:forISO:));
+ (void)setPerGameINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value forISO:(nullable NSString *)isoName NS_SWIFT_NAME(setPerGameINIString(_:key:value:forISO:));
+ (void)deletePerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName NS_SWIFT_NAME(deletePerGameINIValue(_:key:forISO:));
+ (BOOL)hasPerGameINIValueForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key NS_SWIFT_NAME(hasPerGameINIValueForCurrentGame(_:key:));
+ (BOOL)getPerGameINIBoolForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def NS_SWIFT_NAME(getPerGameINIBoolForCurrentGame(_:key:defaultValue:));

// Identity the accessors above key on, or "" when there isn't one. The current-game
// variant only reads VM state; the ISO variant opens the disc image, so keep it
// off render paths. Pass nil for isoName to target the running game.
+ (nonnull NSString *)perGameIdentityKeyForCurrentGame;
// Serial and crc from VM state, without the INI parse gameSettingsForCurrentGame does.
+ (nullable NSDictionary<NSString *, NSString *> *)perGameIdentityForCurrentGame;
+ (nonnull NSString *)perGameIdentityKeyForISO:(nullable NSString *)isoName NS_SWIFT_NAME(perGameIdentityKey(forISO:));

// Runtime speed control
+ (int)limiterMode;
+ (void)setLimiterMode:(int)mode;
+ (void)setPresentFPSCap:(float)fps NS_SWIFT_NAME(setPresentFPSCap(_:));

// Runtime disc identity
+ (nonnull NSString *)currentDiscIdentity;
// The folder name the texture loader reads, unlike currentDiscIdentity which normalizes it.
+ (nonnull NSString *)currentTextureSerial;
+ (void)reloadTextureReplacements;
// Unpacks a zip or tar.zst of replacement textures into textures/<serial>/replacements and returns the
// serial: `serial` if set, else a serial folder in the archive, else its file name, else fallbackSerial.
+ (nullable NSString *)installTexturePackAtURL:(nonnull NSURL *)archiveURL serial:(nonnull NSString *)forcedSerial fallbackSerial:(nonnull NSString *)fallbackSerial error:(NSError * _Nullable * _Nullable)error;

// VM lifecycle for menu flow
+ (BOOL)isVMRunning;
+ (BOOL)hasBIOS;
+ (void)requestVMBoot;
+ (void)testControllerRumble;

// Save states
+ (BOOL)hasValidSaveStateGame;
+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots;
+ (void)saveStateToSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(saveState(toSlot:completion:));
+ (void)loadStateFromSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(loadState(fromSlot:completion:));

// PNACH cheats/patches (pass nil for isoName to target the running game)
+ (nullable NSString *)pnachPathForCurrentGameAsCheat:(BOOL)asCheat NS_SWIFT_NAME(pnachPathForCurrentGame(asCheat:));
+ (nullable NSString *)pnachPathForISO:(nullable NSString *)isoName asCheat:(BOOL)asCheat NS_SWIFT_NAME(pnachPath(forISO:asCheat:));
+ (void)reloadPatches;

// Per-game patch/cheat enable lists (stored in the per-game INI under [Cheats]/Enable
// and [Patches]/Enable, matching the PCSX2 patch loader). Used by the Cheats & Patches
// manager to toggle named patch entries without rewriting .pnach files. Pass nil for isoName to target the running game.
+ (nonnull NSArray<NSString *> *)patchEnableListForISO:(nullable NSString *)isoName
                                               section:(nonnull NSString *)section
                                                   key:(nonnull NSString *)key
        NS_SWIFT_NAME(patchEnableList(forISO:section:key:));
+ (void)setPatchEnableList:(nonnull NSArray<NSString *> *)values
                    forISO:(nullable NSString *)isoName
                   section:(nonnull NSString *)section
                       key:(nonnull NSString *)key
        NS_SWIFT_NAME(setPatchEnableList(_:forISO:section:key:));

// Memory card management
+ (nonnull NSString *)memoryCardDirectory;
+ (nonnull NSArray<NSString *> *)availableMemoryCards;
+ (nullable NSString *)memoryCardNameForSlot:(NSInteger)slot NS_SWIFT_NAME(memoryCardName(forSlot:));
+ (void)setMemoryCardName:(nonnull NSString *)name forSlot:(NSInteger)slot enabled:(BOOL)enabled NS_SWIFT_NAME(setMemoryCard(name:forSlot:enabled:));
+ (BOOL)createMemoryCardNamed:(nonnull NSString *)name sizeMB:(NSInteger)sizeMB folder:(BOOL)folder NS_SWIFT_NAME(createMemoryCard(named:sizeMB:folder:));
+ (BOOL)deleteMemoryCardNamed:(nonnull NSString *)name NS_SWIFT_NAME(deleteMemoryCard(named:));

// RetroAchievements
+ (nonnull NSDictionary<NSString *, id> *)retroAchievementsState;
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)retroAchievementsForCurrentGame;
+ (nullable ARMSX2RetroAchievementsToastInfo *)consumePendingRetroAchievementsNotification;
+ (BOOL)isRetroAchievementsHardcoreActive;
+ (void)setRetroAchievementsEnabled:(BOOL)enabled;
+ (void)setRetroAchievementsHardcore:(BOOL)enabled;
+ (void)setRetroAchievementsNotifications:(BOOL)enabled;
+ (void)setRetroAchievementsLeaderboards:(BOOL)enabled;
+ (void)setRetroAchievementsOverlays:(BOOL)enabled;
+ (void)loginRetroAchievementsWithUsername:(nonnull NSString *)username password:(nonnull NSString *)password completion:(nullable ARMSX2RetroAchievementsCompletion)completion NS_SWIFT_NAME(loginRetroAchievements(username:password:completion:));
+ (void)logoutRetroAchievements;

// DEV9 / Network
+ (nonnull NSArray<NSString *> *)dev9NetworkAdapters;

// Gamepad button mapping
+ (void)startButtonCapture;
+ (void)stopButtonCapture;
+ (void)pollGamepadForCapture;  // call from main thread when VM is not running
+ (int)capturedButton;  // returns SDL_GamepadButton or -1
+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton;
+ (int)getButtonMapping:(int)ps2Index;
+ (void)resetButtonMappings;

@end
