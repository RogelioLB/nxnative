// The movie decoders on MediaCodec (media_android.c): t_media <h264 file> <aac file>, made by ffmpeg (the README's Checks):
//   an Annex B H.264 stream of 30 frames of solid 0x2060c0 at 320×240 (High profile with B-frames, access unit delimiters), decoded as hle_movie.c does it (one access
//   unit per packet, pictures taken as they come and the rest at the end), as NV12 (Y 91, U 180, V 93: ffmpeg's BT.601 video range) and as RGBA (about 32 96 192);
//   an ADTS AAC stream of one second of a 1 kHz sine at 48 kHz, stereo, decoded through AudioConverter as md_aac does (the configuration in an esds cookie).
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm.h"
#include "hle_movie_avc.h"
static int fails, checks;
#define EXPECT(what, ok) do { checks++; int ok_ = (ok); printf("%-70s %s\n", what, ok_ ? "✓" : "✗"); if (!ok_) fails++; } while (0)
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { printf("cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static int video(const uint8_t *s, size_t n, int rgba, int *pics, uint8_t px[4]) {
    mavc *a = mavc_new(); if (rgba) mavc_set_rgba(a);
    int w = 320, h = 240; uint8_t *out = malloc((size_t)w * h * 4); int64_t pts; int frame = 0;
    size_t i = next_sc(s, n, 0), start = i;
    for (;;) {   // access units: from one delimiter (NAL type 9) to the next
        size_t j = next_sc(s, n, i + 3);
        int last = j >= n, aud = !last && (s[j + 3] & 31) == 9;
        if (last || aud) {
            size_t e = last ? n : (j > 0 && !s[j - 1] ? j - 1 : j);
            mavc_decode(a, s + start, (int)(e - start), (int64_t)frame * 33333); frame++;
            while (mavc_take(a, 0, out, w, h, &pts)) { (*pics)++; memcpy(px, rgba ? out + ((size_t)120 * w + 160) * 4 : out + (size_t)120 * w + 160, rgba ? 4 : 1); if (!rgba) { px[1] = out[(size_t)w * h + 60 * w + 160]; px[2] = out[(size_t)w * h + 60 * w + 161]; } }
            start = e;
        }
        if (last) break;
        i = j;
    }
    while (mavc_take(a, 1, out, w, h, &pts)) { (*pics)++; memcpy(px, rgba ? out + ((size_t)120 * w + 160) * 4 : out + (size_t)120 * w + 160, rgba ? 4 : 1); if (!rgba) { px[1] = out[(size_t)w * h + 60 * w + 160]; px[2] = out[(size_t)w * h + 60 * w + 161]; } }
    mavc_free(a); free(out);
    return frame;
}
static const uint8_t *g_pkt; static UInt32 g_pkt_n;
static OSStatus feed(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **pd, void *ctx) {
    (void)c; (void)ctx; (void)pd;
    if (!g_pkt) { *np = 0; return 1; }
    bl->mNumberBuffers = 1; bl->mBuffers[0].mData = (void *)g_pkt; bl->mBuffers[0].mDataByteSize = g_pkt_n; *np = 1; g_pkt = NULL; return 0;
}
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: t_media <h264> <aac>\n"); return 2; }
    size_t n; uint8_t *s = slurp(argv[1], &n);
    int pics = 0; uint8_t px[4] = {0};
    int frames = video(s, n, 0, &pics, px);
    printf("H.264 NV12: %d access units, %d pictures, centre Y %d, U %d, V %d\n", frames, pics, px[0], px[1], px[2]);
    EXPECT("NV12: every access unit gives a picture", frames == 30 && pics == 30);
    EXPECT("NV12: the colour (Y 91 U 180 V 93, within 3)", abs(px[0] - 91) <= 3 && abs(px[1] - 180) <= 3 && abs(px[2] - 93) <= 3);
    pics = 0; frames = video(s, n, 1, &pics, px);
    printf("H.264 RGBA: %d pictures, centre %d %d %d %d\n", pics, px[0], px[1], px[2], px[3]);
    EXPECT("RGBA: every access unit gives a picture", pics == 30);
    EXPECT("RGBA: the colour (32 96 192, within 8, opaque)", abs(px[0] - 32) <= 8 && abs(px[1] - 96) <= 8 && abs(px[2] - 192) <= 8 && px[3] == 255);
    free(s);
    // AAC: ADTS frames → raw packets; the AudioSpecificConfig from the first header, in an esds cookie as hle_movie.c makes it
    s = slurp(argv[2], &n);
    int prof = (s[2] >> 6) + 1, fi = s[2] >> 2 & 15, ch = (s[2] & 1) << 2 | s[3] >> 6;
    uint8_t asc[2] = {(uint8_t)(prof << 3 | fi >> 1), (uint8_t)((fi & 1) << 7 | ch << 3)};
    uint8_t ck[64], *p = ck;
    *p++ = 3; *p++ = 3 + 2 + 13 + 2 + 2; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 4; *p++ = 13 + 2 + 2; *p++ = 0x40; *p++ = 0x15; memset(p, 0, 11); p += 11;
    *p++ = 5; *p++ = 2; memcpy(p, asc, 2); p += 2;
    AudioStreamBasicDescription in = {.mSampleRate = 48000, .mFormatID = kAudioFormatMPEG4AAC, .mFramesPerPacket = 1024, .mChannelsPerFrame = (UInt32)ch};
    AudioStreamBasicDescription out = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                       .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch, .mChannelsPerFrame = (UInt32)ch, .mBitsPerChannel = 16};
    AudioConverterRef c = NULL;
    EXPECT("AAC converter made and configured", !AudioConverterNew(&in, &out, &c) && !AudioConverterSetProperty(c, kAudioConverterDecompressionMagicCookie, (UInt32)(p - ck), ck));
    long total = 0, cross = 0; double sq = 0; int16_t prev = 0, buf[8192 * 2]; int packets = 0;
    for (size_t i = 0; i + 7 <= n;) {
        size_t len = (size_t)(s[i + 3] & 3) << 11 | (size_t)s[i + 4] << 3 | s[i + 5] >> 5, hl = s[i + 1] & 1 ? 7 : 9;
        if (len < hl || i + len > n) break;
        g_pkt = s + i + hl; g_pkt_n = (UInt32)(len - hl); packets++;
        UInt32 fr = 8192; AudioBufferList ob = {1, {{(UInt32)ch, sizeof buf, buf}}};
        AudioConverterFillComplexBuffer(c, feed, NULL, &fr, &ob, NULL);
        for (UInt32 k = 0; k < fr; k++) { int16_t v = buf[k * ch]; sq += (double)v * v; if ((v >= 0) != (prev >= 0)) cross++; prev = v; }
        total += fr; i += len;
    }
    double rms = total ? sqrt(sq / total) : 0;
    printf("AAC: %d packets, %ld frames out, RMS %.0f, %ld zero crossings (%.0f Hz)\n", packets, total, rms, cross, total ? cross / 2.0 / (total / 48000.0) : 0);
    EXPECT("AAC: about the stream's length (all but the decoder's last packets)", total > 40000 && total <= 49152);
    EXPECT("AAC: a 1 kHz tone (within 5 %, not silent)", rms > 1000 && total && fabs(cross / 2.0 / (total / 48000.0) - 1000) < 50);
    AudioConverterDispose(c); free(s);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
