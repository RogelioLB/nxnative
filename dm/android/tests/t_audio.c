// The audio output on AAudio (coreaudio_aaudio.c), as hle_audio.c drives it: an output AudioUnit with a PCM16 stream format and a render callback,
// started for two seconds of a quiet 440 Hz tone. Checks that the stream runs at about real time (the frames rendered), that the callback's
// buffers reach the device (no underruns past the start), and that Stop and a second Start work (hle_audio.c's watchdog restarts the unit so).
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dm.h"
int dm_aaudio_xruns(AudioUnit au);   // coreaudio_aaudio.c
static int fails, checks;
#define EXPECT(what, ok) do { checks++; int ok_ = (ok); printf("%-60s %s\n", what, ok_ ? "✓" : "✗"); if (!ok_) fails++; } while (0)
static _Atomic long g_frames, g_calls; static double g_phase;
static OSStatus render(void *ud, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *bl) {
    (void)ud; (void)fl; (void)ts; (void)bus;
    int16_t *s = bl->mBuffers[0].mData;
    for (UInt32 i = 0; i < n; i++) { int16_t v = (int16_t)(2000 * sin(g_phase)); g_phase += 2 * M_PI * 440 / 48000; s[2 * i] = s[2 * i + 1] = v; }
    g_frames += n; g_calls++;
    return noErr;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output};
    AudioComponent c = AudioComponentFindNext(NULL, &d); AudioUnit au = NULL;
    EXPECT("output component found and instanced", c && !AudioComponentInstanceNew(c, &au));
    AudioStreamBasicDescription f = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {render, NULL};
    EXPECT("stream format and render callback set, unit initialized", !AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) &&
           !AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) && !AudioUnitInitialize(au));
    double t0 = now_s(); EXPECT("started", !AudioOutputUnitStart(au));
    usleep(500000); double t1 = now_s(); long fr1 = g_frames, c1 = g_calls;   // after the start-up: the steady rate
    sleep(2); double dt = now_s() - t1; long fr = g_frames - fr1;
    printf("first %.2f s: %ld frames; then %ld frames in %ld callbacks over %.2f s (%.0f frames/s), %d underruns\n", t1 - t0, fr1, fr, (long)(g_calls - c1), dt, fr / dt, dm_aaudio_xruns(au));
    EXPECT("about real time (48000 frames/s within 10 %)", fabs(fr / dt - 48000) < 4800);
    EXPECT("no more than 2 underruns (the start may have one)", dm_aaudio_xruns(au) <= 2);
    EXPECT("stopped", !AudioOutputUnitStop(au));
    usleep(200000); long f1 = g_frames; usleep(300000);
    EXPECT("no callbacks while stopped", g_frames == f1);
    EXPECT("started again", !AudioOutputUnitStart(au));
    usleep(500000); EXPECT("callbacks again", g_frames > f1 + 10000);
    AudioOutputUnitStop(au); AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
