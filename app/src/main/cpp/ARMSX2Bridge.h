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

typedef void (^ARMSX2SaveStateCompletion)(BOOL success);
typedef void (^ARMSX2RetroAchievementsCompletion)(BOOL success, NSString * _Nonnull message);

@interface ARMSX2Bridge : NSObject

// Game render view (for UIViewRepresentable)
+ (nonnull UIView *)gameRenderView;
+ (void)prepareGameRenderViewForCurrentRenderer;

// Lifecycle
+ (void)saveNVRAM;
+ (void)saveMemoryCards;
+ (void)saveAllState;  // NVM + MC
+ (BOOL)isRunning;

// NVM status
+ (nullable NSDate *)lastNVMSaveDate;
+ (nullable NSString *)nvmFilePath;
+ (BOOL)nvmFileExists;

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
+ (nonnull NSArray<NSURL *> *)extractControllerSkinArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory
    NS_SWIFT_NAME(extractControllerSkinArchive(at:to:));

// OSD overlay
+ (void)setPerformanceOverlayVisible:(BOOL)visible;
+ (BOOL)isPerformanceOverlayVisible;
+ (void)applyOsdPreset:(int)preset;  // 0=off, 1=simple, 2=detail, 3=full

// ISO management
+ (nullable NSString *)currentISOPath;
+ (nullable NSString *)currentGameISOName;
+ (nonnull NSString *)isoDirectory;
+ (nonnull NSString *)documentsDirectory;
+ (nonnull NSArray<NSString *> *)availableISOs;
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)availableISOEntries;
+ (nonnull NSDictionary<NSString *, NSString *> *)gameMetadataForISO:(nonnull NSString *)isoName;
+ (nonnull NSDictionary<NSString *, id> *)gameSettingsForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(gameSettings(forISO:));
+ (nullable NSDictionary<NSString *, id> *)gameSettingsForCurrentGame;
+ (void)setGameSettingsForISO:(nonnull NSString *)isoName
                       enabled:(BOOL)enabled
             upscaleMultiplier:(float)upscaleMultiplier
                   aspectRatio:(nonnull NSString *)aspectRatio
              textureFiltering:(int)textureFiltering
            hardwareMipmapping:(BOOL)hardwareMipmapping
              blendingAccuracy:(int)blendingAccuracy
               interlaceMode:(int)interlaceMode
        trilinearFiltering:(int)trilinearFiltering
          halfPixelOffset:(int)halfPixelOffset
              roundSprite:(int)roundSprite
      alignSpriteOverride:(BOOL)alignSpriteOverride
              alignSprite:(BOOL)alignSprite
      mergeSpriteOverride:(BOOL)mergeSpriteOverride
              mergeSprite:(BOOL)mergeSprite
    wildArmsOffsetOverride:(BOOL)wildArmsOffsetOverride
           wildArmsOffset:(BOOL)wildArmsOffset
    textureOffsetXOverride:(BOOL)textureOffsetXOverride
           textureOffsetX:(int)textureOffsetX
    textureOffsetYOverride:(BOOL)textureOffsetYOverride
           textureOffsetY:(int)textureOffsetY
     skipDrawStartOverride:(BOOL)skipDrawStartOverride
            skipDrawStart:(int)skipDrawStart
       skipDrawEndOverride:(BOOL)skipDrawEndOverride
              skipDrawEnd:(int)skipDrawEnd
                    eeCoreType:(int)eeCoreType
                          mtvu:(BOOL)mtvu
                  enableCheats:(BOOL)enableCheats
                 enablePatches:(BOOL)enablePatches
              enableGameFixes:(BOOL)enableGameFixes
    enableGameDBHardwareFixes:(BOOL)enableGameDBHardwareFixes
    NS_SWIFT_NAME(setGameSettings(forISO:enabled:upscaleMultiplier:aspectRatio:textureFiltering:hardwareMipmapping:blendingAccuracy:interlaceMode:trilinearFiltering:halfPixelOffset:roundSprite:alignSpriteOverride:alignSprite:mergeSpriteOverride:mergeSprite:wildArmsOffsetOverride:wildArmsOffset:textureOffsetXOverride:textureOffsetX:textureOffsetYOverride:textureOffsetY:skipDrawStartOverride:skipDrawStart:skipDrawEndOverride:skipDrawEnd:eeCoreType:mtvu:enableCheats:enablePatches:enableGameFixes:enableGameDBHardwareFixes:));
+ (void)setGameSettingsForCurrentGameWithEnabled:(BOOL)enabled
                               upscaleMultiplier:(float)upscaleMultiplier
                                     aspectRatio:(nonnull NSString *)aspectRatio
                                textureFiltering:(int)textureFiltering
                              hardwareMipmapping:(BOOL)hardwareMipmapping
                                blendingAccuracy:(int)blendingAccuracy
                                   interlaceMode:(int)interlaceMode
                              trilinearFiltering:(int)trilinearFiltering
                                 halfPixelOffset:(int)halfPixelOffset
                                     roundSprite:(int)roundSprite
                             alignSpriteOverride:(BOOL)alignSpriteOverride
                                     alignSprite:(BOOL)alignSprite
                             mergeSpriteOverride:(BOOL)mergeSpriteOverride
                                     mergeSprite:(BOOL)mergeSprite
                           wildArmsOffsetOverride:(BOOL)wildArmsOffsetOverride
                                  wildArmsOffset:(BOOL)wildArmsOffset
                           textureOffsetXOverride:(BOOL)textureOffsetXOverride
                                  textureOffsetX:(int)textureOffsetX
                           textureOffsetYOverride:(BOOL)textureOffsetYOverride
                                  textureOffsetY:(int)textureOffsetY
                            skipDrawStartOverride:(BOOL)skipDrawStartOverride
                                   skipDrawStart:(int)skipDrawStart
                              skipDrawEndOverride:(BOOL)skipDrawEndOverride
                                     skipDrawEnd:(int)skipDrawEnd
                                      eeCoreType:(int)eeCoreType
                                            mtvu:(BOOL)mtvu
                                    enableCheats:(BOOL)enableCheats
                                   enablePatches:(BOOL)enablePatches
                                 enableGameFixes:(BOOL)enableGameFixes
                      enableGameDBHardwareFixes:(BOOL)enableGameDBHardwareFixes
    NS_SWIFT_NAME(setGameSettingsForCurrentGame(enabled:upscaleMultiplier:aspectRatio:textureFiltering:hardwareMipmapping:blendingAccuracy:interlaceMode:trilinearFiltering:halfPixelOffset:roundSprite:alignSpriteOverride:alignSprite:mergeSpriteOverride:mergeSprite:wildArmsOffsetOverride:wildArmsOffset:textureOffsetXOverride:textureOffsetX:textureOffsetYOverride:textureOffsetY:skipDrawStartOverride:skipDrawStart:skipDrawEndOverride:skipDrawEnd:eeCoreType:mtvu:enableCheats:enablePatches:enableGameFixes:enableGameDBHardwareFixes:));
+ (nonnull NSString *)clearCacheForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(clearCache(forISO:));
+ (nonnull NSString *)deleteGameDataForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(deleteGameData(forISO:));
+ (BOOL)deleteISO:(nonnull NSString *)isoName deleteGameData:(BOOL)deleteGameData NS_SWIFT_NAME(deleteISO(_:deleteGameData:));
+ (void)changeDiscToISO:(nonnull NSString *)isoName completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(changeDisc(toISO:completion:));
+ (void)ejectDiscWithCompletion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(ejectDisc(completion:));

// [P44] ISO boot
+ (BOOL)canResolveISO:(nonnull NSString *)isoName NS_SWIFT_NAME(canResolveISO(_:));
+ (void)bootISO:(nonnull NSString *)isoName;

// [P44] BIOS management
+ (nonnull NSString *)biosDirectory;
+ (nonnull NSArray<NSString *> *)availableBIOSes;
+ (nonnull NSArray<ARMSX2BIOSInfo *> *)availableBIOSInfos;
+ (nonnull ARMSX2BIOSInfo *)biosInfoForName:(nonnull NSString *)biosName;
+ (nonnull NSString *)defaultBIOSName;
+ (void)setDefaultBIOS:(nonnull NSString *)biosName;

// [P44] Favorites
+ (BOOL)isFavorite:(nonnull NSString *)isoName;
+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite;

// [P44] INI generic getter/setter
+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def;
+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def;
+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def;
+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value;
+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value;
+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value;
+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value;

// Compatibility Lab
+ (BOOL)getJITBisectFlag:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (void)setJITBisectFlag:(nonnull NSString *)key value:(BOOL)value;
+ (nonnull NSString *)compatibilityPresetForCurrentGame;
+ (nonnull NSString *)compatibilityIdentityForCurrentGame;
+ (nonnull NSString *)compatibilityPresetForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(compatibilityPreset(forISO:));
+ (nonnull NSString *)compatibilityIdentityForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(compatibilityIdentity(forISO:));
+ (BOOL)isCompatibilityAutoGamePresetsEnabled;
+ (void)setCompatibilityAutoGamePresetsEnabled:(BOOL)enabled;
+ (void)setCompatibilityPreset:(nonnull NSString *)preset rememberForCurrentGame:(BOOL)rememberForCurrentGame;
+ (void)setCompatibilityPreset:(nonnull NSString *)preset forISO:(nonnull NSString *)isoName NS_SWIFT_NAME(setCompatibilityPreset(_:forISO:));
+ (BOOL)compatibilityFlag:(nonnull NSString *)flag forISO:(nonnull NSString *)isoName NS_SWIFT_NAME(compatibilityFlag(_:forISO:));
+ (void)setCompatibilityFlag:(nonnull NSString *)flag enabled:(BOOL)enabled forISO:(nonnull NSString *)isoName NS_SWIFT_NAME(setCompatibilityFlag(_:enabled:forISO:));
+ (void)forgetCompatibilityPresetForCurrentGame;
+ (void)forgetCompatibilityPresetForISO:(nonnull NSString *)isoName NS_SWIFT_NAME(forgetCompatibilityPreset(forISO:));

// [P44] VM lifecycle for menu flow
+ (BOOL)isVMRunning;
+ (BOOL)hasBIOS;
+ (void)requestVMBoot;
+ (void)requestVMShutdown;
+ (void)resetVM;
+ (void)testControllerRumble;

// Save states
+ (BOOL)hasValidSaveStateGame;
+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots;
+ (void)saveStateToSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(saveState(toSlot:completion:));
+ (void)loadStateFromSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(loadState(fromSlot:completion:));

// PNACH cheats/patches
+ (nullable NSString *)pnachPathForCurrentGameAsCheat:(BOOL)asCheat NS_SWIFT_NAME(pnachPathForCurrentGame(asCheat:));
+ (nullable NSString *)pnachPathForISO:(nonnull NSString *)isoName asCheat:(BOOL)asCheat NS_SWIFT_NAME(pnachPath(forISO:asCheat:));
+ (void)reloadPatches;

// Memory card management
+ (nonnull NSString *)memoryCardDirectory;
+ (nonnull NSArray<NSString *> *)availableMemoryCards;
+ (nullable NSString *)memoryCardNameForSlot:(NSInteger)slot NS_SWIFT_NAME(memoryCardName(forSlot:));
+ (void)setMemoryCardName:(nonnull NSString *)name forSlot:(NSInteger)slot enabled:(BOOL)enabled NS_SWIFT_NAME(setMemoryCard(name:forSlot:enabled:));
+ (BOOL)createMemoryCardNamed:(nonnull NSString *)name sizeMB:(NSInteger)sizeMB folder:(BOOL)folder NS_SWIFT_NAME(createMemoryCard(named:sizeMB:folder:));

// RetroAchievements
+ (nonnull NSDictionary<NSString *, id> *)retroAchievementsState;
+ (void)setRetroAchievementsEnabled:(BOOL)enabled;
+ (void)setRetroAchievementsHardcore:(BOOL)enabled;
+ (void)setRetroAchievementsNotifications:(BOOL)enabled;
+ (void)setRetroAchievementsLeaderboards:(BOOL)enabled;
+ (void)setRetroAchievementsOverlays:(BOOL)enabled;
+ (void)loginRetroAchievementsWithUsername:(nonnull NSString *)username password:(nonnull NSString *)password completion:(nullable ARMSX2RetroAchievementsCompletion)completion NS_SWIFT_NAME(loginRetroAchievements(username:password:completion:));
+ (void)logoutRetroAchievements;

// DEV9 / Network
+ (nonnull NSArray<NSString *> *)dev9NetworkAdapters;

// [P53] Gamepad button mapping
+ (void)startButtonCapture;
+ (void)stopButtonCapture;
+ (void)pollGamepadForCapture;  // call from main thread when VM is not running
+ (int)capturedButton;  // returns SDL_GamepadButton or -1
+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton;
+ (int)getButtonMapping:(int)ps2Index;
+ (void)resetButtonMappings;

@end
