//
//  EmulatorBridge.mm
//  ARMSX2
//
//  Bridge implementation between Swift and C++ emulator
//

#import "EmulatorBridge.h"
#import "JITManager.h"
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

// Forward declarations for PCSX2 C++ code
// These will link to the actual PCSX2 implementation
extern "C" {
    // These are placeholder declarations - actual implementation will link from PCSX2 core
    void PCSX2_Init(const char* biosPath);
    bool PCSX2_LoadGame(const char* gamePath);
    void PCSX2_Start();
    void PCSX2_Pause();
    void PCSX2_Stop();
    void PCSX2_Reset();
    bool PCSX2_SaveState(int slot);
    bool PCSX2_LoadState(int slot);
    void PCSX2_UpdateFrame();
    double PCSX2_GetFPS();
}

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
            const char *biosPathCStr = [biosPath UTF8String];
            NSLog(@"[ARMSX2-Bridge] Calling PCSX2_Init...");

            // In a real implementation, this would call the actual PCSX2 init
            // PCSX2_Init(biosPathCStr);

            self.isInitialized = YES;
            NSLog(@"[ARMSX2-Bridge] Emulator core initialized successfully");
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
            const char *gamePathCStr = [gamePath UTF8String];

            // In a real implementation, this would call the actual PCSX2 load
            // success = PCSX2_LoadGame(gamePathCStr);
            success = YES;

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
        // PCSX2_Start();

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
        // PCSX2_Pause();

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
        // PCSX2_Stop();
    });

    if (self.delegate) {
        [self.delegate emulatorStateDidChange:self.state];
    }
}

- (void)reset {
    NSLog(@"[ARMSX2-Bridge] Resetting emulation");

    dispatch_async(self.emulatorQueue, ^{
        // PCSX2_Reset();
    });
}

- (BOOL)saveState:(NSInteger)slot {
    NSLog(@"[ARMSX2-Bridge] Saving state to slot %ld", (long)slot);

    __block BOOL success = NO;
    dispatch_sync(self.emulatorQueue, ^{
        // success = PCSX2_SaveState((int)slot);
        success = YES;
    });

    return success;
}

- (BOOL)loadState:(NSInteger)slot {
    NSLog(@"[ARMSX2-Bridge] Loading state from slot %ld", (long)slot);

    __block BOOL success = NO;
    dispatch_sync(self.emulatorQueue, ^{
        // success = PCSX2_LoadState((int)slot);
        success = YES;
    });

    return success;
}

- (void)updateFrame {
    dispatch_async(self.emulatorQueue, ^{
        // PCSX2_UpdateFrame();

        // Update FPS
        // self.currentFPS = PCSX2_GetFPS();
        self.currentFPS = 60.0; // Placeholder

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
    // Send button press to PCSX2 input system
}

- (void)releaseButton:(NSString *)button {
    NSLog(@"[ARMSX2-Bridge] Button released: %@", button);
    // Send button release to PCSX2 input system
}

- (void)dealloc {
    [self stop];
}

@end
