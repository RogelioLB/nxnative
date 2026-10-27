// AudioToolbox's output AudioUnit on AAudio (compat/AudioToolbox/AudioToolbox.h): the unit's render callback becomes the stream's data callback.
// The stream is opened at Initialize with the format set on the input scope (PCM16, the host's rate and channel count), in low-latency
// shared mode; AAudio resamples to the device when the rate differs. A disconnected stream (headphones unplugged, route change) is reopened
// on the next Start; hle_audio.c's watchdog notices the missing callbacks and calls Stop / Start.
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include "dm.h"

struct dm_au_component { int unused; };
struct dm_au {
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb;
    AAudioStream *st;
    pthread_mutex_t mu;
};
static struct dm_au_component g_output;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return -1;
    struct dm_au *au = calloc(1, sizeof *au); if (!au) return -1;
    pthread_mutex_init(&au->mu, NULL);
    *out = au; return noErr;
}
static aaudio_data_callback_result_t data_cb(AAudioStream *st, void *ud, void *audio, int32_t frames) {
    (void)st; struct dm_au *au = ud;
    UInt32 bytes = (UInt32)frames * au->fmt.mBytesPerFrame;
    AudioBufferList bl = {1, {{au->fmt.mChannelsPerFrame, bytes, audio}}};
    AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {0};
    if (!au->cb.inputProc || au->cb.inputProc(au->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl)) memset(audio, 0, bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void close_stream(struct dm_au *au) { if (au->st) { AAudioStream_close(au->st); au->st = NULL; } }
static OSStatus open_stream(struct dm_au *au) {
    AAudioStreamBuilder *b; aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)au->fmt.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)au->fmt.mChannelsPerFrame);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, data_cb, au);
    r = AAudioStreamBuilder_openStream(b, &au->st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { au->st = NULL; LOG("[RED] audio: AAudio stream open failed: %s", AAudio_convertResultToText(r)); return r; }
    LOG("audio: AAudio stream %d Hz %d channels, burst %d frames, buffer %d frames", AAudioStream_getSampleRate(au->st), AAudioStream_getChannelCount(au->st),
        AAudioStream_getFramesPerBurst(au->st), AAudioStream_getBufferSizeInFrames(au->st));
    // Two bursts of buffering: low latency, and the host's queue (hle_audio.c) absorbs the game's own block sizes
    AAudioStream_setBufferSizeInFrames(au->st, 2 * AAudioStream_getFramesPerBurst(au->st));
    return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance au) {
    if (!au) return noErr;
    pthread_mutex_lock(&au->mu); close_stream(au); pthread_mutex_unlock(&au->mu);
    pthread_mutex_destroy(&au->mu); free(au); return noErr;
}
OSStatus AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && size >= sizeof au->fmt) {
        const AudioStreamBasicDescription *f = data;
        if (f->mFormatID != kAudioFormatLinearPCM || f->mBitsPerChannel != 16 || !(f->mFormatFlags & kLinearPCMFormatFlagIsSignedInteger)) return -50;   // paramErr: only PCM16
        au->fmt = *f; return noErr;
    }
    if (id == kAudioUnitProperty_SetRenderCallback && size >= sizeof au->cb) { au->cb = *(const AURenderCallbackStruct *)data; return noErr; }
    return -10879;   // kAudioUnitErr_InvalidProperty
}
OSStatus AudioUnitGetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && *size >= sizeof au->fmt) { *(AudioStreamBasicDescription *)data = au->fmt; *size = sizeof au->fmt; return noErr; }
    return -10879;
}
OSStatus AudioUnitAddPropertyListener(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitInitialize(AudioUnit au) {
    if (!au->fmt.mChannelsPerFrame || !au->fmt.mSampleRate) return -50;
    pthread_mutex_lock(&au->mu); OSStatus st = au->st ? noErr : open_stream(au); pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioUnitUninitialize(AudioUnit au) { pthread_mutex_lock(&au->mu); close_stream(au); pthread_mutex_unlock(&au->mu); return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit au) {
    pthread_mutex_lock(&au->mu);
    if (au->st && AAudioStream_getState(au->st) == AAUDIO_STREAM_STATE_DISCONNECTED) close_stream(au);
    OSStatus st = au->st ? noErr : open_stream(au);
    if (!st) st = AAudioStream_requestStart(au->st);
    pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioOutputUnitStop(AudioUnit au) {
    pthread_mutex_lock(&au->mu); OSStatus st = au->st ? AAudioStream_requestStop(au->st) : noErr; pthread_mutex_unlock(&au->mu);
    return st;
}

int dm_aaudio_xruns(AudioUnit au) { pthread_mutex_lock(&au->mu); int n = au->st ? AAudioStream_getXRunCount(au->st) : -1; pthread_mutex_unlock(&au->mu); return n; }   // tests/t_audio.c
// AAC decoding for movies (AudioConverter): media_android.c
