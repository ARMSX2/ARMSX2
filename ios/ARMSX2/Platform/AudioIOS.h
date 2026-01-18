//
//  AudioIOS.h
//  ARMSX2
//
//  iOS Audio backend header
//

#ifndef AudioIOS_h
#define AudioIOS_h

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize iOS audio system
void iOS_Audio_Initialize(void);

/// Shutdown iOS audio system
void iOS_Audio_Shutdown(void);

/// Start audio playback
void iOS_Audio_Start(void);

/// Stop audio playback
void iOS_Audio_Stop(void);

/// Submit audio samples for playback
/// @param data Pointer to interleaved stereo PCM data (16-bit)
/// @param frameCount Number of frames (samples per channel)
void iOS_Audio_SubmitSamples(const short* data, int frameCount);

/// Get the audio sample rate
int iOS_Audio_GetSampleRate(void);

/// Get number of audio channels
int iOS_Audio_GetChannels(void);

/// Get audio buffer size in frames
int iOS_Audio_GetBufferSize(void);

#ifdef __cplusplus
}
#endif

#endif /* AudioIOS_h */
