// Movie decoding on Android's MediaCodec (NDK AMediaCodec): H.264 for the movie decoder (hle_movie_avc.h, VideoToolbox's place on Apple, hle_movie_avc.c) and
// AAC behind AudioToolbox's AudioConverter (compat/AudioToolbox/AudioToolbox.h; hle_movie.c md_aac).
// H.264: the access units (Annex B, as the SDK's demuxer hands them over) go to an "video/avc" decoder made from SPS / PPS (csd-0 / csd-1). Its pictures go to an
// AImageReader (YUV_420_888 with explicit plane strides: the byte-buffer output of Qualcomm's decoders is a tiled vendor format) and are copied out as they arrive, to
// NV12 or, in the native texture mode, RGBA (the stream's colour matrix: BT.709 from 720 lines up, else BT.601; video range). MediaCodec hands pictures out in presentation
// order already, so none is held back for reordering; at the end of the stream (take with flush) the decoder is drained.
// AAC: an "audio/mp4a-latm" decoder configured from the AudioSpecificConfig in the magic cookie (the esds descriptor hle_movie.c builds); FillComplexBuffer feeds it
// the packets the input procedure gives and returns the PCM16 it has, the decoder's start-up delay giving none for the first packets.
#include <AudioToolbox/AudioToolbox.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include "dm.h"
#include "hle_movie_avc.h"

// ---------------- H.264 ----------------
#define QMAX 16
typedef struct { uint8_t *buf; int w, h; int64_t pts; } pic_t;   // a picture as it came out: NV12 (Y then interleaved UV, stride w) of w × h
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty, red, rgba;
    AMediaCodec *mc; AImageReader *rd; ANativeWindow *win; int eos;
    pthread_mutex_t mu; pthread_cond_t cv; pic_t q[QMAX]; int nq, w, h; long rendered, arrived;   // pictures sent to the reader / taken from it
    uint8_t *au; size_t aucap;
};
mavc *mavc_new(void) {
    mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed");
    pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
    return a;
}
static void close_codec(mavc *a) {
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; }
    if (a->rd) { AImageReader_delete(a->rd); a->rd = NULL; a->win = NULL; }   // the window belongs to the reader
    a->eos = 0;
}
void mavc_drop(mavc *a) {
    pthread_mutex_lock(&a->mu); for (int i = 0; i < a->nq; i++) free(a->q[i].buf); a->nq = 0; pthread_mutex_unlock(&a->mu);
    if (a->mc) AMediaCodec_flush(a->mc);
}
void mavc_free(mavc *a) { if (!a) return; if (a->arrived != a->rendered) LOG("[RED] movie: AVC decoder rendered %ld pictures, %ld reached the reader", a->rendered, a->arrived); close_codec(a); mavc_drop(a); free(a->au); pthread_mutex_destroy(&a->mu); pthread_cond_destroy(&a->cv); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {   // as hle_movie_avc.c: Annex B, or else 4-byte lengths
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) { size_t b = s + 3, e = next_sc(p, n, b), end = e; while (end > b && !p[end - 1]) end--; if (end > b) fn(ctx, p + b, end - b); s = e; }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
// a picture from the reader → the queue, as NV12 (the conversion to RGBA is done at take, for the size asked)
static void on_image(void *ctx, AImageReader *rd) {
    mavc *a = ctx; AImage *img = NULL;
    while (AImageReader_acquireNextImage(rd, &img) == AMEDIA_OK && img) {
        int32_t w = 0, h = 0; int64_t ts = 0; AImageCropRect cr;
        AImage_getWidth(img, &w); AImage_getHeight(img, &h); AImage_getTimestamp(img, &ts);
        if (AImage_getCropRect(img, &cr) == AMEDIA_OK && cr.right > cr.left && cr.bottom > cr.top) { w = cr.right - cr.left; h = cr.bottom - cr.top; } else cr.left = cr.top = 0;
        uint8_t *yp, *up, *vp; int yl, ul, vl, ys, us, vs, ups, vps;
        AImage_getPlaneData(img, 0, &yp, &yl); AImage_getPlaneData(img, 1, &up, &ul); AImage_getPlaneData(img, 2, &vp, &vl);
        AImage_getPlaneRowStride(img, 0, &ys); AImage_getPlaneRowStride(img, 1, &us); AImage_getPlaneRowStride(img, 2, &vs);
        AImage_getPlanePixelStride(img, 1, &ups); AImage_getPlanePixelStride(img, 2, &vps);
        w &= ~1; h &= ~1;
        uint8_t *buf = w > 0 && h > 0 ? malloc((size_t)w * h * 3 / 2) : NULL;
        if (buf) {
            for (int r = 0; r < h; r++) memcpy(buf + (size_t)r * w, yp + (size_t)(r + cr.top) * ys + cr.left, (size_t)w);
            uint8_t *uv = buf + (size_t)w * h;
            for (int r = 0; r < h / 2; r++) {
                const uint8_t *u = up + (size_t)(r + cr.top / 2) * us + (size_t)(cr.left / 2) * ups, *v = vp + (size_t)(r + cr.top / 2) * vs + (size_t)(cr.left / 2) * vps;
                uint8_t *o = uv + (size_t)r * w;
                for (int x = 0; x < w / 2; x++) { o[2 * x] = u[x * ups]; o[2 * x + 1] = v[x * vps]; }
            }
            pthread_mutex_lock(&a->mu); a->arrived++;
            if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].buf); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
            a->q[a->nq++] = (pic_t){buf, w, h, ts / 1000};
            pthread_cond_broadcast(&a->cv); pthread_mutex_unlock(&a->mu);
        }
        AImage_delete(img); img = NULL;
    }
}
static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    if (AImageReader_new(1920, 1088, AIMAGE_FORMAT_YUV_420_888, 6, &a->rd) != AMEDIA_OK || AImageReader_getWindow(a->rd, &a->win) != AMEDIA_OK) {
        if (!a->red++) LOG("[RED] movie: no image reader for the AVC decoder, outputting black frames"); close_codec(a); return 0; }
    AImageReader_ImageListener l = {a, on_image}; AImageReader_setImageListener(a->rd, &l);
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: no H.264 decoder on this device, outputting black frames"); close_codec(a); return 0; }
    uint8_t c0[520], c1[520]; static const uint8_t sc[4] = {0, 0, 0, 1};
    memcpy(c0, sc, 4); memcpy(c0 + 4, a->sps, (size_t)a->nsps); memcpy(c1, sc, 4); memcpy(c1 + 4, a->pps, (size_t)a->npps);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // the stream's own size comes from its SPS
    AMediaFormat_setBuffer(f, "csd-0", c0, (size_t)a->nsps + 4); AMediaFormat_setBuffer(f, "csd-1", c1, (size_t)a->npps + 4);
    media_status_t st = AMediaCodec_configure(a->mc, f, a->win, NULL, 0); AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: the H.264 decoder refused its configuration (%d), outputting black frames", st); close_codec(a); return 0; }
    LOG("[movie] AVC: MediaCodec decoding, profile %d", a->nsps > 1 ? a->sps[1] : 0);
    a->dirty = 0; return 1;
}
// the decoder's finished pictures go to the reader (rendered to its window); end: the end of stream was reached
static int drain(mavc *a, int64_t timeout_us) {
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, timeout_us);
        if (i >= 0) {
            if (bi.size > 0) { pthread_mutex_lock(&a->mu); a->rendered++; pthread_mutex_unlock(&a->mu); }
            AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, bi.size > 0);
            // the reader's queue keeps one queued picture and replaces it with the next: each one is taken before the next is sent (up to 100 ms)
            pthread_mutex_lock(&a->mu);
            for (int k = 0; k < 20 && a->arrived < a->rendered; k++) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_cond_timedwait(&a->cv, &a->mu, &ts); }
            pthread_mutex_unlock(&a->mu); if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return 1; continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        return 0;   // try again later
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc) && !open_codec(a)) return -1;
    for (int tries = 0; tries < 200; tries++) {   // up to 2 s for an input buffer: the decoder frees them as it finishes pictures
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
        if (i < 0) { drain(a, 0); continue; }
        size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
        if (!b || cap < c.len) { if (!a->red++) LOG("[RED] movie: AVC access unit of %zu bytes over the decoder's %zu", c.len, cap); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts_us, 0); return -1; }
        memcpy(b, a->au, c.len);
        AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)pts_us, 0);
        drain(a, 0);
        return 0;
    }
    if (!a->red++) LOG("[RED] movie: the AVC decoder took no input for 2 s");
    return -1;
}
// YUV (video range) → RGBA: BT.709 from 720 lines up, BT.601 below (as the streams' colour description usually says; VideoToolbox reads it from the stream)
static void nv12_rgba(const uint8_t *src, int sw, int sh, uint8_t *dst, int w, int h) {
    int hd = sh >= 720, cw = sw < w ? sw : w, chh = sh < h ? sh : h;
    int kr = hd ? 459 : 409, kgu = hd ? 55 : 100, kgv = hd ? 136 : 208, kb = hd ? 541 : 516;   // ×256, video range: 1.164 (Y), the matrix's coefficients
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    const uint8_t *uvp = src + (size_t)sw * sh;
    for (int r = 0; r < chh; r++) {
        const uint8_t *y = src + (size_t)r * sw, *uv = uvp + (size_t)(r / 2) * sw; uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int Y = (y[x] - 16) * 298, U = uv[x & ~1] - 128, V = uv[(x & ~1) + 1] - 128;
            int R = (Y + kr * V + 128) >> 8, G = (Y - kgu * U - kgv * V + 128) >> 8, B = (Y + kb * U + 128) >> 8;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc && !a->eos) {
        if (flush) {   // the end of the stream: every picture comes out
            ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 100000);
            if (i >= 0) AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            for (int k = 0; k < 100 && !drain(a, 20000); k++) {}
            a->eos = 1;
            pthread_mutex_lock(&a->mu);   // the last pictures reach the reader's listener (up to 1 s)
            for (int k = 0; k < 100 && a->arrived < a->rendered; k++) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 10000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_cond_timedwait(&a->cv, &a->mu, &ts); }
            pthread_mutex_unlock(&a->mu);
        } else drain(a, 0);
    }
    pthread_mutex_lock(&a->mu);
    if (!a->nq && a->mc && !flush) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&a->cv, &a->mu, &ts); }   // a picture decoded just now is on its way to the reader
    if (!a->nq) { pthread_mutex_unlock(&a->mu); return 0; }
    pic_t p = a->q[0]; memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    pthread_mutex_unlock(&a->mu);
    if (pts_us) *pts_us = p.pts;
    if (a->rgba) nv12_rgba(p.buf, p.w, p.h, dst, w, h);
    else {
        int cw = p.w < w ? p.w : w, chh = p.h < h ? p.h : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, p.buf + (size_t)r * p.w, (size_t)cw);
        for (int r = 0; r < chh / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, p.buf + (size_t)p.w * p.h + (size_t)r * p.w, (size_t)cw);
    }
    free(p.buf);
    return 1;
}

// ---------------- AAC (AudioConverter) ----------------
struct dm_audio_converter {
    AMediaCodec *mc; int rate, ch, out_ch; uint8_t asc[64]; int nasc, started, red;
    int16_t *pend; size_t npend, cappend;   // decoded frames not handed out yet (interleaved, ch channels)
};
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    *c = NULL;
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16) return -50;
    struct dm_audio_converter *a = calloc(1, sizeof *a); if (!a) return -108;
    a->rate = (int)in->mSampleRate; a->ch = (int)out->mChannelsPerFrame; a->out_ch = a->ch;
    *c = a; return noErr;
}
// the magic cookie: an esds descriptor chain (ES 3 → decoder configuration 4 → decoder specific 5 = the AudioSpecificConfig)
static int descr(const uint8_t *p, size_t n, size_t *i, int *tag, size_t *len) {
    if (*i >= n) return 0; *tag = p[(*i)++]; *len = 0;
    for (int k = 0; k < 4 && *i < n; k++) { uint8_t b = p[(*i)++]; *len = *len << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    return 1;
}
OSStatus AudioConverterSetProperty(AudioConverterRef a, AudioConverterPropertyID id, UInt32 size, const void *data) {
    if (id != kAudioConverterDecompressionMagicCookie) return -50;
    const uint8_t *p = data; size_t i = 0; int tag; size_t len;
    while (descr(p, size, &i, &tag, &len)) {
        if (tag == 3) { i += 3; continue; }          // ES ID, flags: the decoder configuration follows inside
        if (tag == 4) { i += 13; continue; }         // object type, stream type, buffer size, bit rates
        if (tag == 5) { if (len > sizeof a->asc || i + len > size) return -50; memcpy(a->asc, p + i, len); a->nasc = (int)len; return noErr; }
        i += len;
    }
    return -50;
}
static int aac_open(struct dm_audio_converter *a) {
    a->started = -1;
    a->mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!a->mc) { LOG("[RED] movie: no AAC decoder on this device"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, a->rate); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, a->ch);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    if (a->nasc) AMediaFormat_setBuffer(f, "csd-0", a->asc, (size_t)a->nasc);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0); AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { LOG("[RED] movie: the AAC decoder refused its configuration (%d)", st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    a->started = 1; return 1;
}
static void aac_out(struct dm_audio_converter *a, int64_t timeout_us) {   // what the decoder has ready → pend
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, timeout_us); timeout_us = 0;
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t ch = 0;
            if (f && AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch) && ch > 0) a->out_ch = ch;
            if (f) AMediaFormat_delete(f); continue;
        }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        size_t cap; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &cap);
        size_t frames = b ? (size_t)bi.size / (2u * (size_t)a->out_ch) : 0;
        if (a->npend + frames > a->cappend) { a->cappend = (a->npend + frames) * 2; a->pend = realloc(a->pend, a->cappend * (size_t)a->ch * 2); }
        const int16_t *s = (const int16_t *)(b + bi.offset);
        for (size_t f = 0; f < frames && a->pend; f++) for (int c = 0; c < a->ch; c++)   // the decoder's channel count to the one asked (mono duplicated, extra channels dropped)
            a->pend[(a->npend + f) * (size_t)a->ch + (size_t)c] = s[f * (size_t)a->out_ch + (size_t)(c < a->out_ch ? c : a->out_ch - 1)];
        if (a->pend) a->npend += frames;
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
    }
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef a, AudioConverterComplexInputDataProc fn, void *ctx, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)desc;
    UInt32 want = *packets; *packets = 0;
    if (!a->started && !aac_open(a)) return -1;
    if (a->started < 0) return -1;
    for (;;) {   // the packets the input procedure has
        UInt32 np = 1; AudioBufferList bl = {1, {{0, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        OSStatus st = fn(a, &np, &bl, &pd, ctx);
        if (st || !np || !bl.mBuffers[0].mData) break;
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 20000);
        while (i < 0) { aac_out(a, 0); i = AMediaCodec_dequeueInputBuffer(a->mc, 20000); if (i < 0 && !a->red++) LOG("[RED] movie: the AAC decoder is slow to take input"); }
        size_t cap; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap); size_t n = bl.mBuffers[0].mDataByteSize < cap ? bl.mBuffers[0].mDataByteSize : cap;
        memcpy(b, bl.mBuffers[0].mData, n);
        AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, n, 0, 0);
    }
    aac_out(a, a->npend ? 0 : 5000);   // a packet decoded now: its samples
    UInt32 n = (UInt32)(a->npend < want ? a->npend : want);
    size_t room = out->mBuffers[0].mDataByteSize / (2u * (size_t)a->ch); if (n > room) n = (UInt32)room;
    if (n) { memcpy(out->mBuffers[0].mData, a->pend, (size_t)n * 2 * (size_t)a->ch); memmove(a->pend, a->pend + (size_t)n * a->ch, (a->npend - n) * 2 * (size_t)a->ch); a->npend -= n; }
    out->mBuffers[0].mDataByteSize = n * 2u * (UInt32)a->ch; *packets = n;
    return noErr;
}
OSStatus AudioConverterDispose(AudioConverterRef a) {
    if (!a) return noErr;
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); }
    free(a->pend); free(a); return noErr;
}
