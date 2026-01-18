//
//  EmulatorBridge.mm
//  ARMSX2
//
//  Bridge implementation between Swift and C++ emulator
//

#import "EmulatorBridge.h"
#import "JITManager.h"
#import "../Platform/PCSX2Wrapper.h"
#import "../Platform/AudioIOS.h"
#import "../Platform/InputIOS.h"
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

@interface EmulatorBridge ()
@property (readwrite, nonatomic) EmulatorState state;
@property (readwrite, nonatomic) BOOL isInitialized;
@property (readwrite, nonatomic) double currentFPS;
@property (nonatomic) dispatch_queue_t emulatorQueue;
@property (nonatomic) NSTimer *frameTimer;
@property (nonatomic, strong) JITManager *jitManager;
@property (nonatomic) id<MTLDevice> metalDevice;
@property (nonatomic) id<MTLCommandQueue> metalCommandQueue;
@end

@implementation EmulatorBridge

+ (instancetype)sharedBridge {
    static EmulatorBridge *bridge = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        bridge = [[EmulatorBridge alloc] init];
    });
    return bridge;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _state = EmulatorStateStopped;
        _isInitialized = NO;
        _currentFPS = 0.0;
        _emulatorQueue = dispatch_queue_create("net.armsx2.emulator", DISPATCH_QUEUE_SERIAL);
        _jitManager = [JITManager sharedManager];

        // Initialize Metal
        _metalDevice = MTLCreateSystemDefaultDevice();
        if (_metalDevice) {
            _metalCommandQueue = [_metalDevice newCommandQueue];
            NSLog(@"[ARMSX2-Bridge] Metal initialized successfully");
        } else {
            NSLog(@"[ARMSX2-Bridge] Warning: Metal device not available");
        }

        NSLog(@"[ARMSX2-Bridge] Emulator Bridge initialized");
    }
    return self;
}

- (BOOL)initializeWithBIOSPath:(NSString *)biosPath error:(NSError **)error {
    NSLog(@"[ARMSX2-Bridge] Initializing emulator with BIOS: %@", biosPath);

    // Check if BIOS file exists
    if (![[NSFileManager defaultManager] fileExistsAtPath:biosPath]) {
        if (error) {
            *error = [NSError errorWithDomain:@"ARMSX2"
                                         code:1001
                                     userInfo:@{NSLocalizedDescriptionKey: @"BIOS file not found"}];
        }
        return NO;
    }

    // Initialize JIT first
    if (![self.jitManager initializeJIT]) {
        NSLog(@"[ARMSX2-Bridge] Warning: JIT initialization failed, performance may be affected");
    }

    // Initialize PCSX2 core
    dispatch_sync(self.emulatorQueue, ^{
        @try {
            NSString *documentsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
            NSString *dataPath = [documentsPath stringByAppendingPathComponent:@"ARMSX2"];

            // Create data directory if needed
            [[NSFileManager defaultManager] createDirectoryAtPath:dataPath
                                        withIntermediateDirectories:YES
                                        attributes:nil
                                        error:nil];

            NSLog(@"[ARMSX2-Bridge] Initializing PCSX2 wrapper...");
            self.isInitialized = PCSX2Wrapper::Initialize([biosPath UTF8String], [dataPath UTF8String]);

            if (self.isInitialized) {
                NSLog(@"[ARMSX2-Bridge] PCSX2 initialized successfully");
            } else {
                NSLog(@"[ARMSX2-Bridge] PCSX2 initialization failed");
            }
        } @catch (NSException *exception) {
            NSLog(@"[ARMSX2-Bridge] Exception during initialization: %@", exception);
            self.isInitialized = NO;
        }
    });

    if (self.isInitialized && self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }

    return self.isInitialized;
}

- (BOOL)loadGame:(NSString *)gamePath error:(NSError **)error {
    if (!self.isInitialized) {
        if (error) {
            *error = [NSError errorWithDomain:@"ARMSX2"
                                         code:1002
                                     userInfo:@{NSLocalizedDescriptionKey: @"Emulator not initialized"}];
        }
        return NO;
    }

    // Check if game file exists
    if (![[NSFileManager defaultManager] fileExistsAtPath:gamePath]) {
        if (error) {
            *error = [NSError errorWithDomain:@"ARMSX2"
                                         code:1003
                                     userInfo:@{NSLocalizedDescriptionKey: @"Game file not found"}];
        }
        return NO;
    }

    NSLog(@"[ARMSX2-Bridge] Loading game: %@", gamePath);
    self.state = EmulatorStateLoading;

    __block BOOL success = NO;
    dispatch_sync(self.emulatorQueue, ^{
        @try {
            success = PCSX2Wrapper::LoadGame([gamePath UTF8String]);

            if (success) {
                self.state = EmulatorStatePaused;
                NSLog(@"[ARMSX2-Bridge] Game loaded successfully");
            } else {
                self.state = EmulatorStateError;
                NSLog(@"[ARMSX2-Bridge] Failed to load game");
            }
        } @catch (NSException *exception) {
            NSLog(@"[ARMSX2-Bridge] Exception while loading game: %@", exception);
            success = NO;
            self.state = EmulatorStateError;
        }
    });

    if (self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }

    if (!success && error) {
        *error = [NSError errorWithDomain:@"ARMSX2"
                                     code:1004
                                 userInfo:@{NSLocalizedDescriptionKey: @"Failed to load game"}];
    }

    return success;
}

- (void)start {
    if (self.state != EmulatorStatePaused && self.state != EmulatorStateStopped) {
        NSLog(@"[ARMSX2-Bridge] Cannot start - invalid state: %ld", (long)self.state);
        return;
    }

    NSLog(@"[ARMSX2-Bridge] Starting emulation");
    self.state = EmulatorStateRunning;

    dispatch_async(self.emulatorQueue, ^{
        PCSX2Wrapper::Start();

        // Start frame update timer
        dispatch_async(dispatch_get_main_queue(), ^{
            self.frameTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                                              target:self
                                                            selector:@selector(updateFrame)
                                                            userInfo:nil
                                                             repeats:YES];
        });
    });

    if (self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }
}

- (void)pause {
    if (self.state != EmulatorStateRunning) {
        return;
    }

    NSLog(@"[ARMSX2-Bridge] Pausing emulation");
    self.state = EmulatorStatePaused;

    dispatch_async(self.emulatorQueue, ^{
        PCSX2Wrapper::Pause();

        // Stop frame timer
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.frameTimer invalidate];
            self.frameTimer = nil;
        });
    });

    if (self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }
}

- (void)stop {
    NSLog(@"[ARMSX2-Bridge] Stopping emulation");

    [self.frameTimer invalidate];
    self.frameTimer = nil;

    self.state = EmulatorStateStopped;

    dispatch_async(self.emulatorQueue, ^{
        PCSX2Wrapper::Stop();
    });

    if (self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }
}

- (void)reset {
    NSLog(@"[ARMSX2-Bridge] Resetting emulation");

    dispatch_async(self.emulatorQueue, ^{
        PCSX2Wrapper::Reset();
    });
}

- (BOOL)saveState:(NSInteger)slot {
    NSLog(@"[ARMSX2-Bridge] Saving state to slot %ld", (long)slot);

    __block BOOL success = NO;
    dispatch_sync(self.emulatorQueue, ^{
        success = PCSX2Wrapper::SaveState((int)slot);
    });

    return success;
}

- (BOOL)loadState:(NSInteger)slot {
    NSLog(@"[ARMSX2-Bridge] Loading state from slot %ld", (long)slot);

    __block BOOL success = NO;
    dispatch_sync(self.emulatorQueue, ^{
        success = PCSX2Wrapper::LoadState((int)slot);
    });

    return success;
}

- (void)updateFrame {
    dispatch_async(self.emulatorQueue, ^{
        // Run one frame
        PCSX2Wrapper::RunFrame();

        // Update FPS
        self.currentFPS = PCSX2Wrapper::GetFPS();

        if (self.delegate) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.delegate emulatorDidUpdateFrame:self.currentFPS];
            });
        }
    });
}

- (void)updateSettings:(NSDictionary *)settings {
    NSLog(@"[ARMSX2-Bridge] Updating settings: %@", settings);

    dispatch_async(self.emulatorQueue, ^{
        // Apply settings to PCSX2 core
        // This would involve calling various PCSX2 configuration functions
    });
}

- (NSDictionary *)getEmulatorInfo {
    return @{
        @"version": @"1.0.0",
        @"core": @"PCSX2",
        @"platform": @"iOS",
        @"jit_enabled": @(self.jitManager.isJITEnabled),
        @"jit_status": [self.jitManager statusDescription],
        @"metal_available": @(self.metalDevice != nil)
    };
}

- (void)setupRenderSurface:(UIView *)view {
    NSLog(@"[ARMSX2-Bridge] Setting up render surface");

    // Setup Metal rendering
    if (self.metalDevice && [view isKindOfClass:[MTKView class]]) {
        MTKView *metalView = (MTKView *)view;
        metalView.device = self.metalDevice;
        metalView.preferredFramesPerSecond = 60;
        NSLog(@"[ARMSX2-Bridge] Metal render surface configured");
    }
}

- (void)touchBegan:(UITouch *)touch {
    // Handle touch input for virtual controls
}

- (void)touchMoved:(UITouch *)touch {
    // Handle touch movement
}

- (void)touchEnded:(UITouch *)touch {
    // Handle touch release
}

- (void)pressButton:(NSString *)button {
    NSLog(@"[ARMSX2-Bridge] Button pressed: %@", button);

    // Map button name to PS2Button enum
    PS2Button ps2Button = PS2Button_Cross; // Default

    if ([button isEqualToString:@"cross"] || [button isEqualToString:@"X"]) {
        ps2Button = PS2Button_Cross;
    } else if ([button isEqualToString:@"circle"] || [button isEqualToString:@"O"]) {
        ps2Button = PS2Button_Circle;
    } else if ([button isEqualToString:@"square"]) {
        ps2Button = PS2Button_Square;
    } else if ([button isEqualToString:@"triangle"]) {
        ps2Button = PS2Button_Triangle;
    } else if ([button isEqualToString:@"up"]) {
        ps2Button = PS2Button_DPad_Up;
    } else if ([button isEqualToString:@"down"]) {
        ps2Button = PS2Button_DPad_Down;
    } else if ([button isEqualToString:@"left"]) {
        ps2Button = PS2Button_DPad_Left;
    } else if ([button isEqualToString:@"right"]) {
        ps2Button = PS2Button_DPad_Right;
    } else if ([button isEqualToString:@"L1"]) {
        ps2Button = PS2Button_L1;
    } else if ([button isEqualToString:@"R1"]) {
        ps2Button = PS2Button_R1;
    } else if ([button isEqualToString:@"L2"]) {
        ps2Button = PS2Button_L2;
    } else if ([button isEqualToString:@"R2"]) {
        ps2Button = PS2Button_R2;
    } else if ([button isEqualToString:@"start"]) {
        ps2Button = PS2Button_Start;
    } else if ([button isEqualToString:@"select"]) {
        ps2Button = PS2Button_Select;
    }

    iOS_Input_SetButton(0, ps2Button, true);
}

- (void)releaseButton:(NSString *)button {
    NSLog(@"[ARMSX2-Bridge] Button released: %@", button);

    // Map button name to PS2Button enum (same logic as press)
    PS2Button ps2Button = PS2Button_Cross;

    if ([button isEqualToString:@"cross"] || [button isEqualToString:@"X"]) {
        ps2Button = PS2Button_Cross;
    } else if ([button isEqualToString:@"circle"] || [button isEqualToString:@"O"]) {
        ps2Button = PS2Button_Circle;
    } else if ([button isEqualToString:@"square"]) {
        ps2Button = PS2Button_Square;
    } else if ([button isEqualToString:@"triangle"]) {
        ps2Button = PS2Button_Triangle;
    } else if ([button isEqualToString:@"up"]) {
        ps2Button = PS2Button_DPad_Up;
    } else if ([button isEqualToString:@"down"]) {
        ps2Button = PS2Button_DPad_Down;
    } else if ([button isEqualToString:@"left"]) {
        ps2Button = PS2Button_DPad_Left;
    } else if ([button isEqualToString:@"right"]) {
        ps2Button = PS2Button_DPad_Right;
    } else if ([button isEqualToString:@"L1"]) {
        ps2Button = PS2Button_L1;
    } else if ([button isEqualToString:@"R1"]) {
        ps2Button = PS2Button_R1;
    } else if ([button isEqualToString:@"L2"]) {
        ps2Button = PS2Button_L2;
    } else if ([button isEqualToString:@"R2"]) {
        ps2Button = PS2Button_R2;
    } else if ([button isEqualToString:@"start"]) {
        ps2Button = PS2Button_Start;
    } else if ([button isEqualToString:@"select"]) {
        ps2Button = PS2Button_Select;
    }

    iOS_Input_SetButton(0, ps2Button, false);
}

- (void)dealloc {
    [self stop];
    PCSX2Wrapper::Shutdown();
}

@end
