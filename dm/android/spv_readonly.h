// Vulkan: without the vertexPipelineStoresAndAtomics feature, every storage buffer and storage image a vertex, tessellation or geometry shader declares must be
// decorated NonWritable (VUID-RuntimeSpirv-NonWritable-06340). The translator (Ryujinx's) never adds it: the Adreno has the feature, the Mali-G76 (POCO M5s) has not,
// and there the shader breaks the rule whether or not it writes. spv_readonly adds NonWritable to each such variable the shader never writes through
// (a store, an atomic, a memory copy, an image write, or a pointer passed to a function, which counts as a write): the rule then holds and the shader is unchanged.
// Self-contained (no libc) so that dm/android/tests/test_spv_readonly.py builds it alone and runs it over a game's shaders on the PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

// w[n]: the module. scratch: 2 × bound words (w[3]). o[cap]: the result, at least n + 3 × (the variables) words.
// → the result's word count, or 0 when the module needs nothing (not a vertex-pipeline stage, nothing to decorate, or o too small); *written: the variables that are written
static size_t spv_readonly(const uint32_t *w, size_t n, uint32_t *scratch, uint32_t *o, size_t cap, int *written) {
    enum { STORAGE = 1, IMAGE = 2, DECORATED = 8, WRITTEN = 32 };
    *written = 0;
    if (n < 5) return 0;
    uint32_t bound = w[3], *root = scratch, *kind = scratch + bound;
    for (uint32_t i = 0; i < 2 * bound; i++) scratch[i] = 0;
    int vtx = 0, nvar = 0;
#define ID(x) ((x) < bound)
    for (size_t i = 5; i < n; ) {   // the stage, the decorations, the types and the variables
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) return 0;
        if (op == 15 && len > 1 && w[i + 1] <= 3) vtx = 1;   // OpEntryPoint Vertex, TessellationControl, TessellationEvaluation, Geometry
        if (op == 71 && len > 2 && ID(w[i + 1])) {   // OpDecorate
            if (w[i + 2] == 3) kind[w[i + 1]] |= STORAGE;   // BufferBlock
            if (w[i + 2] == 24) kind[w[i + 1]] |= DECORATED;   // NonWritable
        }
        if (op == 25 && len > 7 && ID(w[i + 1]) && w[i + 7] == 2) kind[w[i + 1]] |= IMAGE;   // OpTypeImage, Sampled 2: a storage image
        if ((op == 28 || op == 29) && len > 2 && ID(w[i + 1]) && ID(w[i + 2])) kind[w[i + 1]] |= kind[w[i + 2]] & (STORAGE | IMAGE);   // OpTypeArray / OpTypeRuntimeArray
        if (op == 32 && len > 3 && ID(w[i + 1]) && ID(w[i + 3])) {   // OpTypePointer: what it points to, in a class that makes it a storage resource
            uint32_t c = w[i + 2], k = kind[w[i + 3]];
            kind[w[i + 1]] |= c == 12 ? STORAGE : c == 2 ? k & STORAGE : c == 0 ? k & IMAGE : 0;   // StorageBuffer; Uniform with BufferBlock; UniformConstant image
        }
        if (op == 59 && len > 3 && ID(w[i + 1]) && ID(w[i + 2]) && (kind[w[i + 1]] & (STORAGE | IMAGE)) && !(kind[w[i + 2]] & DECORATED)) { root[w[i + 2]] = w[i + 2]; nvar++; }   // OpVariable
        i += len;
    }
    if (!vtx || !nvar) return 0;
    for (int changed = 1, pass = 0; changed && pass < 16; pass++) {   // the pointers and image values derived from them, until no more; the writes through them
        changed = 0;
        for (size_t i = 5; i < n; ) {
            uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
            uint32_t from = 0, to = 0, wr = 0;
            if ((op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 61 || op == 60) && len > 3) { to = w[i + 2]; from = w[i + 3]; }   // access chains, OpCopyObject, OpLoad, OpImageTexelPointer
            if (to && ID(to) && ID(from) && root[from] && !root[to]) { root[to] = root[from]; changed = 1; }
            if ((op == 62 || op == 63 || op == 64 || op == 228 || op == 99) && len > 1) wr = w[i + 1];   // OpStore, OpCopyMemory(Sized), OpAtomicStore, OpImageWrite
            if (((op >= 229 && op <= 242) || op == 6035) && len > 3) wr = w[i + 3];   // the read-modify-write atomics (and OpAtomicFAddEXT)
            if (wr && ID(wr) && root[wr]) kind[root[wr]] |= WRITTEN;
            if (op == 57) for (uint32_t a = 4; a < len; a++) if (ID(w[i + a]) && root[w[i + a]]) kind[root[w[i + a]]] |= WRITTEN;   // OpFunctionCall
            i += len;
        }
    }
    int add = 0;
    for (uint32_t v = 0; v < bound; v++) if (root[v] == v && v) { if (kind[v] & WRITTEN) (*written)++; else add++; }
    if (!add || n + 3 * (size_t)add > cap) return 0;
    size_t k = 0; int done = 0;
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!done && (op == 71 || op == 72 || (op >= 19 && op <= 39))) {   // before the first decoration (or the first type, in a module without one): the new ones
            for (uint32_t v = 0; v < bound; v++) if (root[v] == v && v && !(kind[v] & WRITTEN)) { o[k++] = 3u << 16 | 71; o[k++] = v; o[k++] = 24; }
            done = 1;
        }
        for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];
        i += len;
    }
#undef ID
    return done ? k : 0;
}
