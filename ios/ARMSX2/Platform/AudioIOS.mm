//
//  AudioIOS.mm
//  ARMSX2
//
//  iOS Audio backend for PCSX2 using AVAudioEngine
//

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <cstring>
#include <mutex>

// Audio buffer size
static constexpr int AUDIO_BUFFER_SIZE = 2048;
static constexpr int AUDIO_SAMPLE_RATE = 48000;
static constexpr int AUDIO_CHANNELS = 2;

@interface AudioEngineIOS : NSObject
{
    AVAudioEngine* _audioEngine;
    AVAudioPlayerNode* _playerNode;
    AVAudioPCMBuffer* _audioBuffer;
    AVAudioFormat* _audioFormat;
    BOOL _isRunning;
    std::mutex _mutex;
}

+ (instancetype)sharedInstance;
- (BOOL)initialize;
- (void)shutdown;
- (void)start;
- (void)stop;
- (void)submitSamples:(const int16_t*)data frameCount:(int)frameCount;

@end

@implementation AudioEngineIOS

+ (instancetype)sharedInstance
{
    static AudioEngineIOS* instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[AudioEngineIOS alloc] init];
    });
    return instance;
}

- (instancetype)init
{
    self = [super init];
    if (self) {
        _isRunning = NO;
        NSLog(@"[ARMSX2-Audio] Audio Engine created");
    }
    return self;
}

- (BOOL)initialize
{
    NSLog(@"[ARMSX2-Audio] Initializing iOS audio engine...");

    @try {
        // Create audio format (stereo, 48kHz, 16-bit)
        _audioFormat = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:AUDIO_SAMPLE_RATE
                                                                       channels:AUDIO_CHANNELS];

        // Create audio engine
        _audioEngine = [[AVAudioEngine alloc] init];

        // Create player node
        _playerNode = [[AVAudioPlayerNode alloc] init];

        // Attach player node to engine
        [_audioEngine attachNode:_playerNode];

        // Connect player node to main mixer
        [_audioEngine connect:_playerNode
                           to:_audioEngine.mainMixerNode
                       format:_audioFormat];

        // Create audio buffer
        _audioBuffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:_audioFormat
                                                      frameCapacity:AUDIO_BUFFER_SIZE];

        // Request audio session
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // Set category for playback
        [session setCategory:AVAudioSessionCategoryPlayback
                 withOptions:AVAudioSessionCategoryOptionMixWithOthers
                       error:&error];

        if (error) {
            NSLog(@"[ARMSX2-Audio] Error setting audio category: %@", error);
            return NO;
        }

        // Set preferred sample rate
        [session setPreferredSampleRate:AUDIO_SAMPLE_RATE error:&error];
        if (error) {
            NSLog(@"[ARMSX2-Audio] Warning: Could not set sample rate: %@", error);
        }

        // Activate audio session
        [session setActive:YES error:&error];
        if (error) {
            NSLog(@"[ARMSX2-Audio] Error activating audio session: %@", error);
            return NO;
        }

        // Start audio engine
        [_audioEngine prepare];
        [_audioEngine startAndReturnError:&error];

        if (error) {
            NSLog(@"[ARMSX2-Audio] Error starting audio engine: %@", error);
            return NO;
        }

        NSLog(@"[ARMSX2-Audio] Audio engine initialized successfully");
        NSLog(@"[ARMSX2-Audio]   Sample Rate: %d Hz", AUDIO_SAMPLE_RATE);
        NSLog(@"[ARMSX2-Audio]   Channels: %d", AUDIO_CHANNELS);
        NSLog(@"[ARMSX2-Audio]   Buffer Size: %d frames", AUDIO_BUFFER_SIZE);

        return YES;

    } @catch (NSException* exception) {
        NSLog(@"[ARMSX2-Audio] Exception initializing audio: %@", exception);
        return NO;
    }
}

- (void)shutdown
{
    NSLog(@"[ARMSX2-Audio] Shutting down audio engine...");

    std::lock_guard<std::mutex> lock(_mutex);

    if (_isRunning) {
        [self stop];
    }

    if (_audioEngine) {
        [_audioEngine stop];
        _audioEngine = nil;
    }

    _playerNode = nil;
    _audioBuffer = nil;
    _audioFormat = nil;

    // Deactivate audio session
    AVAudioSession* session = [AVAudioSession sharedInstance];
    [session setActive:NO error:nil];

    NSLog(@"[ARMSX2-Audio] Audio engine shut down");
}

- (void)start
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_isRunning) return;

    NSLog(@"[ARMSX2-Audio] Starting audio playback...");

    if (_playerNode) {
        [_playerNode play];
        _isRunning = YES;
        NSLog(@"[ARMSX2-Audio] Audio playback started");
    }
}

- (void)stop
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_isRunning) return;

    NSLog(@"[ARMSX2-Audio] Stopping audio playback...");

    if (_playerNode) {
        [_playerNode stop];
        _isRunning = NO;
        NSLog(@"[ARMSX2-Audio] Audio playback stopped");
    }
}

- (void)submitSamples:(const int16_t*)data frameCount:(int)frameCount
{
    if (!_isRunning || !_playerNode || !_audioBuffer) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    // Limit frame count to buffer size
    if (frameCount > AUDIO_BUFFER_SIZE) {
        frameCount = AUDIO_BUFFER_SIZE;
    }

    // Set frame length
    _audioBuffer.frameLength = frameCount;

    // Copy audio data to buffer
    int16_t* bufferData = (int16_t*)_audioBuffer.int16ChannelData[0];

    if (bufferData && data) {
        // Interleaved stereo data
        memcpy(bufferData, data, frameCount * AUDIO_CHANNELS * sizeof(int16_t));

        // Schedule buffer for playback
        [_playerNode scheduleBuffer:_audioBuffer
                 completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
                  completionHandler:nil];
    }
}

- (void)dealloc
{
    [self shutdown];
}

@end

// C++ interface for PCSX2

extern "C" {

void iOS_Audio_Initialize()
{
    [[AudioEngineIOS sharedInstance] initialize];
}

void iOS_Audio_Shutdown()
{
    [[AudioEngineIOS sharedInstance] shutdown];
}

void iOS_Audio_Start()
{
    [[AudioEngineIOS sharedInstance] start];
}

void iOS_Audio_Stop()
{
    [[AudioEngineIOS sharedInstance] stop];
}

void iOS_Audio_SubmitSamples(const int16_t* data, int frameCount)
{
    [[AudioEngineIOS sharedInstance] submitSamples:data frameCount:frameCount];
}

int iOS_Audio_GetSampleRate()
{
    return AUDIO_SAMPLE_RATE;
}

int iOS_Audio_GetChannels()
{
    return AUDIO_CHANNELS;
}

int iOS_Audio_GetBufferSize()
{
    return AUDIO_BUFFER_SIZE;
}

} // extern "C"
