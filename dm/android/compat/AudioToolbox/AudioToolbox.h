// Android stand-in for the part of AudioToolbox the host uses (dm/android/coreaudio_aaudio.c):
//   the output AudioUnit (hle_audio.c): PCM16 interleaved, pulled through a render callback, played by an AAudio stream in data-callback mode;
//   AudioConverter (hle_movie.c's AAC movie sound): not available yet, AudioConverterNew fails and the movie plays silent.
#pragma once
#include "dm_android.h"
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef double Float64;
#define noErr 0
typedef uint32_t AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, AudioUnitRenderActionFlags, AudioFormatID, AudioFormatFlags, AudioConverterPropertyID;
typedef uint32_t AudioDeviceID;
#define kAudioObjectUnknown 0
typedef struct { Float64 mSampleRate; AudioFormatID mFormatID; AudioFormatFlags mFormatFlags; UInt32 mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket, mDataByteSize; } AudioStreamPacketDescription;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef struct { Float64 mMinimum, mMaximum; } AudioValueRange;
typedef struct { uint32_t componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct dm_au_component *AudioComponent;
typedef struct dm_au *AudioUnit, *AudioComponentInstance;
typedef OSStatus (*AURenderCallback)(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *data);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef void (*AudioUnitPropertyListenerProc)(void *ctx, AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el);
enum { kAudioUnitType_Output = 'auou', kAudioUnitSubType_RemoteIO = 'rioc', kAudioUnitSubType_DefaultOutput = 'def ', kAudioUnitManufacturer_Apple = 'appl' };
enum { kAudioFormatLinearPCM = 'lpcm', kAudioFormatMPEG4AAC = 'aac ' };
enum { kLinearPCMFormatFlagIsFloat = 1, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8 };
enum { kAudioUnitScope_Global = 0, kAudioUnitScope_Input = 1, kAudioUnitScope_Output = 2 };
enum { kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioOutputUnitProperty_CurrentDevice = 2000,
       kAudioDevicePropertyBufferFrameSize = 'fsiz', kAudioDevicePropertyBufferFrameSizeRange = 'fsz#' };
enum { kAudioConverterDecompressionMagicCookie = 'dmgc' };
AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance au);
OSStatus AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size);
OSStatus AudioUnitGetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size);
OSStatus AudioUnitAddPropertyListener(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx);
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx);
OSStatus AudioUnitInitialize(AudioUnit au);
OSStatus AudioUnitUninitialize(AudioUnit au);
OSStatus AudioOutputUnitStart(AudioUnit au);
OSStatus AudioOutputUnitStop(AudioUnit au);

typedef struct dm_audio_converter *AudioConverterRef;
typedef OSStatus (*AudioConverterComplexInputDataProc)(AudioConverterRef c, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **desc, void *ctx);
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c);
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data);
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc fn, void *ctx, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc);
OSStatus AudioConverterDispose(AudioConverterRef c);
