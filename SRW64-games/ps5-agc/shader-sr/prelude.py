import os
# prelude.py - GLSL runtime of gcn2glsl: guest memory through a range table, V# buffer access with format decoding,
# float helpers with GCN semantics, LDS atomics, wave operations of a 64-thread wave made of two 32-thread warps

import re

BUFFER_FORMATS = [
    'Invalid', '8UNorm', '8SNorm', '8UScaled', '8SScaled', '8UInt', '8SInt', '16UNorm', '16SNorm', '16UScaled',
    '16SScaled', '16UInt', '16SInt', '16Float', '8_8UNorm', '8_8SNorm', '8_8UScaled', '8_8SScaled', '8_8UInt',
    '8_8SInt', '32UInt', '32SInt', '32Float', '16_16UNorm', '16_16SNorm', '16_16UScaled', '16_16SScaled',
    '16_16UInt', '16_16SInt', '16_16Float', '11_11_10UNorm', '11_11_10SNorm', '11_11_10UScaled', '11_11_10SScaled',
    '11_11_10UInt', '11_11_10SInt', '11_11_10Float', '10_11_11UNorm', '10_11_11SNorm', '10_11_11UScaled',
    '10_11_11SScaled', '10_11_11UInt', '10_11_11SInt', '10_11_11Float', '2_10_10_10UNorm', '2_10_10_10SNorm',
    '2_10_10_10UScaled', '2_10_10_10SScaled', '2_10_10_10UInt', '2_10_10_10SInt', '10_10_10_2UNorm',
    '10_10_10_2SNorm', '10_10_10_2UScaled', '10_10_10_2SScaled', '10_10_10_2UInt', '10_10_10_2SInt', '8_8_8_8UNorm',
    '8_8_8_8SNorm', '8_8_8_8UScaled', '8_8_8_8SScaled', '8_8_8_8UInt', '8_8_8_8SInt', '32_32UInt', '32_32SInt',
    '32_32Float', '16_16_16_16UNorm', '16_16_16_16SNorm', '16_16_16_16UScaled', '16_16_16_16SScaled',
    '16_16_16_16UInt', '16_16_16_16SInt', '16_16_16_16Float', '32_32_32UInt', '32_32_32SInt', '32_32_32Float',
    '32_32_32_32UInt', '32_32_32_32SInt', '32_32_32_32Float',
]


def format_table():
    """per gfx10 buffer format: component count | bits << 3 | type << 9 (type: 0 float, 1 unorm, 2 snorm, 3 uscaled,
    4 sscaled, 5 uint, 6 sint); 0 for formats this decoder does not handle (packed or mixed component sizes)"""
    types = {'Float': 0, 'UNorm': 1, 'SNorm': 2, 'UScaled': 3, 'SScaled': 4, 'UInt': 5, 'SInt': 6}
    table = []
    for name in BUFFER_FORMATS:
        m = re.match(r'^([\d_]+)(UNorm|SNorm|UScaled|SScaled|UInt|SInt|Float)$', name)
        entry = 0
        if m:
            bits = [int(x) for x in m.group(1).split('_')]
            if all(b in (8, 16, 32) for b in bits) and len(set(bits)) == 1:
                entry = len(bits) | (bits[0] << 3) | (types[m.group(2)] << 9)
        table.append(entry)
    return table


COMPUTE_PUSH = r"""// Kyty's guest memory model (BufferCache): one device address per 16 KB guest page in the BDA page table (0 = the
// page is not cached on the GPU); a miss sets the page's bit in the fault bitmap (fault buffer offset 0), which Kyty
// turns into a cached buffer after the submit. Guest addresses from kExtendedMemoryBase (2^43) are packed after the
// first 2^40 bytes.
layout(push_constant) uniform Push {
    uint64_t pages;         // BDA page table
    uint64_t faults;        // fault buffer
    uint64_t user_table;    // 0: user data in user[]; else batched guest dispatches, 20 dwords each: user data[16],
                            // first workgroup, workgroup counts x, y, z
    uint64_t counters;      // debug counters (0 = none)
    uint64_t wg_map;        // batched: the guest dispatch of each workgroup
    uint64_t scalars;       // ScalarTable of the dispatch (0 = none; batched: per guest dispatch in the user table)
    uint     use_words;     // dword offset of the use bitmap in the fault buffer
    uint     user_count;    // user SGPRs (s0..)
    uint     tgid_mask;     // bit 0/1/2: TGID_X/Y/Z_EN (SGPRs after the user data)
    uint     debug;         // 1: probe this dispatch (counters [3..5] and [16..], see probe_*)
    uint     user[16];
} pc;"""

# Kyty's graphics plug-in (pipeline/graphicsTranslated.h): push-constant bytes 128..255 of a draw with a translated
# stage; Kyty's own push data stays in bytes 0..127 (not used by the translations)
GRAPHICS_PUSH = r"""// Kyty's guest memory model (BufferCache) as in the compute translations: the BDA page table, the fault buffer
layout(push_constant) uniform Push {
    layout(offset = 128) uint64_t pages;   // BDA page table
    uint64_t faults;                        // fault buffer
    uint64_t user_slot;                     // dwords [0, 32): geometry-stage user SGPRs, [32, 64): pixel-stage ones
    uint     use_words;                     // dword offset of the use bitmap in the fault buffer
    uint     geometry_user_count;
    uint     pixel_user_count;
    uint     flags;                         // bit 0: geometry stage translated, bit 1: pixel stage translated
    uint     geometry_user[10];             // the first user SGPRs of each stage, inline (bytes 168..207)
    uint     pixel_user[8];                 // (bytes 208..239)
    uint64_t pixel_scalars;                 // ScalarTable of the pixel stage's s_load chains (.reach; 0 = none)
    uint64_t geometry_scalars;              // ScalarTable of the geometry stage
} pc;
uint64_t GFX_COUNTERS = 0ul;   // no debug counters or probes in graphics stages
uint GFX_DEBUG = 0u;"""


def prelude(threads, lds_dwords, nvgpr, probes=False, stage='cs'):
    text = r'''#version 460
// generated by gcn2glsl.py (SR64 shader-sr): PS5 gfx1013 wave64 compute shader, one guest lane per thread
#define PROBES PROBES_ON
#define GRAPHICS GRAPHICS_ON
// lane bits of the thread: compute runs lane pairs (bit 0 lane j, bit 1 lane j + 32), graphics one lane per invocation
#define LMASK LMASK_BITS
#define SOFT_SIN SOFT_SIN_ON
#define NO_HANDSHAKE NO_HANDSHAKE_ON
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_shader_image_load_formatted : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_shuffle_relative : require
#extension GL_KHR_shader_subgroup_vote : require
QUAD_EXTENSION


// lane pairs: a host thread runs guest lanes j and j + 32 of its warp's guest wave (one guest wave = one warp)
LOCAL_SIZE_DECL

layout(buffer_reference, std430, buffer_reference_align = 4) buffer W32 { uint v; };
layout(buffer_reference, std430, buffer_reference_align = 2) buffer W16 { uint16_t v; };
layout(buffer_reference, std430, buffer_reference_align = 1) buffer W8 { uint8_t v; };
// scalar loads (s_load, s_buffer_load): read-only for the dispatch, as on the GPU, where the scalar cache does not see
// the dispatch's own vector stores; lets the compiler reuse and hoist descriptor/table loads
layout(buffer_reference, std430, buffer_reference_align = 4) readonly restrict buffer RO32 { uint v; };
// glc / dlc loads (the guest bypasses the non-coherent caches: spin and compare-swap loops read what other waves'
// atomics wrote): coherent, and volatile so the compiler neither hoists nor merges them
layout(buffer_reference, std430, buffer_reference_align = 4) coherent volatile buffer C32 { uint v; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer Counters { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly restrict buffer BdaTable { uint64_t e[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) coherent buffer Bitmap { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly restrict buffer UserTable { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly restrict buffer WgMap { uint v[]; };
// CPU snapshot of the tables the scalar loads read (the emulator resolves the s_load chains of the dispatch on the
// CPU, as its own shader translation does): guest range [lo, lo + size) at device address dev; entry k is reach node k
// (gcn2glsl.scalar_reach), size 0 when the node's memory is read through the page table (GPU-written or large)
struct ScalarRange { uint64_t lo; uint64_t dev; uint size; uint pad0; uint64_t pad1; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly restrict buffer ScalarTable {
    uint count; uint pad0; uint64_t pad1; ScalarRange e[];
};

PUSH_BLOCK

const uint64_t EXTENDED_BASE = 0x080000000000ul;
const uint64_t LOWER_SIZE    = 0x010000000000ul;
const uint     NUM_PAGES     = 0x6000000u;      // (2^40 + 512 GB) >> 14

#if !GRAPHICS
shared uint LDS[LDS_DWORDS];
#endif
uint EXM;           // exec of the thread's two lanes: bit 0 lane j, bit 1 lane j + 32
#define EXB0 ((EXM & 1u) != 0u)
#define EXB1 ((EXM & 2u) != 0u)
bool SCC;
uint LANE0, LANE1;  // the thread's guest lanes in the wave (j, j + 32)
uint FAULT_PAGE;    // one missing page per thread, recorded at the end (NUM_PAGES = none)
uint PROBE_N;       // probed scalar loads so far (workgroup 0, thread 0)
uint64_t SCALARS;   // ScalarTable of this workgroup's guest dispatch

// ---------------------------------------------------------------- guest memory
uint guest_page(uint64_t a) {
    uint64_t packed = a >= EXTENDED_BASE ? a - (EXTENDED_BASE - LOWER_SIZE) : a;
    uint64_t page = packed >> 14;
    return page < uint64_t(NUM_PAGES) ? uint(page) : NUM_PAGES;
}
// guest address -> device address (0 when the page is not cached); free of side effects so the compiler can reuse
// translations; the missing page is kept in FAULT_PAGE
uint64_t xlate(uint64_t a) {
    uint page = guest_page(a);
    uint64_t base = page < NUM_PAGES ? BdaTable(pc.pages).e[page] : 0ul;
    FAULT_PAGE = (base == 0ul && page < NUM_PAGES) ? page : FAULT_PAGE;
    return base == 0ul ? 0ul : base + (a & 0x3ffful);
}
// as xlate, for device address (DMA) accesses: also marks the page as used, as Kyty's own DMA loads do, so the
// buffer cache keeps it
uint64_t xlate_dma(uint64_t a) {
    uint64_t d = xlate(a);
    if (d != 0ul) {
        uint page = guest_page(a);
        Bitmap b = Bitmap(pc.faults);
        uint word = pc.use_words + (page >> 5), bit = 1u << (page & 31u);
        if ((b.w[word] & bit) == 0u) atomicOr(b.w[word], bit);
    }
    return d;
}
// probes (pc.debug): counters[3] buffer stores of active lanes, [4] of them dropped (bounds or no page), [5] threads
// with a missing page, [16 + 3 i] address lo/hi and first dword of the i-th scalar load of workgroup 0 thread 0
void probe_sload(uint64_t a, uint v) {
#if PROBES
    if (pc.debug != 0u && gl_WorkGroupID.x == 0u && gl_LocalInvocationIndex == 0u && pc.counters != 0ul) {
        uint i = PROBE_N++;
        if (i < 24u) {
            Counters c = Counters(pc.counters);
            c.v[16u + 3u * i] = uint(a);
            c.v[17u + 3u * i] = uint(a >> 32);
            c.v[18u + 3u * i] = v;
        }
    }
#endif
}
void record_faults() {
    if (pc.debug != 0u && pc.counters != 0ul && FAULT_PAGE < NUM_PAGES) atomicAdd(Counters(pc.counters).v[5], 1u);
    if (FAULT_PAGE < NUM_PAGES) atomicOr(Bitmap(pc.faults).w[FAULT_PAGE >> 5], 1u << (FAULT_PAGE & 31u));
}
uint sld32(uint64_t d) { return d == 0ul ? 0u : RO32(d).v; }
// scalar load address: the CPU snapshot when it holds [a, a + bytes), else the page table
uint64_t sxlate(uint64_t a, uint bytes) {
    if (SCALARS != 0ul) {
        ScalarTable t = ScalarTable(SCALARS);
        uint n = min(t.count, 32u);
        for (uint i = 0u; i < n; i++) {
            uint64_t lo = t.e[i].lo;
            if (a >= lo && a - lo + uint64_t(bytes) <= uint64_t(t.e[i].size)) return t.e[i].dev + (a - lo);
        }
    }
    return xlate(a);
}
// an s_load through reach node k: the node's snapshot entry when it holds [a, a + bytes) (no search: the hot loops
// issue dozens of scalar loads per iteration), the page table for a node without a snapshot; an address outside the
// node's range (a chain the analysis followed in program order, changed in a loop) searches every snapshot
uint64_t sxlate_k(uint k, uint64_t a, uint bytes) {
    if (SCALARS != 0ul) {
        ScalarTable t = ScalarTable(SCALARS);
        if (k < t.count) {
            uint size = t.e[k].size;
            if (size == 0u) return xlate(a);
            uint64_t lo = t.e[k].lo;
            if (a >= lo && a - lo + uint64_t(bytes) <= uint64_t(size)) return t.e[k].dev + (a - lo);
        }
    }
    return sxlate(a, bytes);
}
// the device address of an s_buffer_load V#'s range, one translation per V# shared by its loads: the snapshot of the
// V#'s reach node k (k < 0: none) when it is this V#'s range, else the page table (one device buffer holds the range)
uint64_t sbuf_xlate(int k, uint w0, uint w1) {
    uint64_t base = packUint2x32(uvec2(w0, w1 & 0xffffu));
    if (k >= 0 && SCALARS != 0ul) {
        ScalarTable t = ScalarTable(SCALARS);
        if (uint(k) < t.count && t.e[k].size != 0u && t.e[k].lo == base) return t.e[k].dev;
    }
    return xlate(base);
}
uint ldd32(uint64_t d) { return d == 0ul ? 0u : W32(d).v; }
uint ldd16(uint64_t d) { return d == 0ul ? 0u : uint(W16(d).v); }
uint ldd8(uint64_t d) { return d == 0ul ? 0u : uint(W8(d).v); }
void std32(uint64_t d, uint v) { if (d != 0ul) W32(d).v = v; }
void std16(uint64_t d, uint v) { if (d != 0ul) W16(d).v = uint16_t(v); }

// V# (buffer descriptor): base 48 bits, stride 14 bits, num_records, word 3 (dst_sel, format); one translation per
// instruction (a buffer access stays inside one device buffer)
struct BufAddr { uint64_t dev; bool ok; uint w1; uint w3; };
void probe_store(BufAddr b) {
#if PROBES
    if (pc.debug != 0u && pc.counters != 0ul) {
        Counters c = Counters(pc.counters);
        atomicAdd(c.v[3], 1u);
        if (!b.ok) atomicAdd(c.v[4], 1u);
        if ((b.w1 >> 30) != 0u || ((b.w3 >> 23) & 1u) != 0u) atomicAdd(c.v[6], 1u);   // swizzled / ADD_TID
        c.v[8] = b.w1;
        c.v[9] = b.w3;
    }
#endif
}
BufAddr buf_addr(uint w0, uint w1, uint w2, uint w3, uint index, uint offset) {
    uint64_t base = packUint2x32(uvec2(w0, w1 & 0xffffu));
    uint stride = (w1 >> 16) & 0x3fffu;
    BufAddr r;
    // the descriptor's base is translated (one device buffer holds the whole guest buffer): one translation per
    // descriptor, shared by all accesses through it
    uint64_t dbase = xlate(base);
    r.ok = (stride != 0u ? index < w2 : offset < w2) && dbase != 0ul;
    r.dev = dbase + uint64_t(stride * index + offset);
    r.w1 = w1;
    r.w3 = w3;
    return r;
}
uint buf_load32(BufAddr b, int extra) { return b.ok ? W32(b.dev + uint64_t(extra)).v : 0u; }
const uint LOOP_LIMIT = 65536u;     // translated loops end after this many iterations (no GPU hang)
uint buf_load32_glc(BufAddr b, int extra) { return b.ok ? C32(b.dev + uint64_t(extra)).v : 0u; }
uint buf_load16(BufAddr b) { return b.ok ? uint(W16(b.dev).v) : 0u; }
uint buf_load16_at(BufAddr b, int extra) { return b.ok ? uint(W16(b.dev + uint64_t(extra)).v) : 0u; }
uint buf_load8_at(BufAddr b, int extra) { return b.ok ? uint(W8(b.dev + uint64_t(extra)).v) : 0u; }
void buf_store32(BufAddr b, int extra, uint v) { if (b.ok) W32(b.dev + uint64_t(extra)).v = v; }
void buf_store16(BufAddr b, uint v) { if (b.ok) W16(b.dev).v = uint16_t(v); }
void buf_store8(BufAddr b, uint v) { if (b.ok) W8(b.dev).v = uint8_t(v); }
const uint FORMATS[78] = uint[78](FORMAT_TABLE);
// UNORM / SNORM conversion, correctly rounded like the hardware's, from integer arithmetic only: the driver would
// compile x / (2^n - 1) differently from shader to shader (reciprocal or division), and results would differ
float div_exact(uint num, uint den) {
    uint64_t n = uint64_t(num) << 40;
    uint64_t q = n / uint64_t(den);
    if (n - q * uint64_t(den) != 0ul) q |= 1ul;            // sticky bit: round-to-nearest of the true quotient
    return float(q) * 9.094947017729282e-13;                // 2^-40, exact
}
float unorm_exact(uint raw, uint bits) {
    if (bits == 32u) return float(raw) / 4294967295.0;
    return div_exact(raw, (1u << bits) - 1u);
}
float snorm_exact(int sraw, uint bits) {
    if (bits == 32u) return float(sraw) / 2147483647.0;
    float m = div_exact(uint(abs(sraw)), (1u << (bits - 1u)) - 1u);
    return sraw < 0 ? -m : m;
}
// buffer_store_format_*: the V#'s data / number format applied to the store data (the VGPRs' bits: floats for the
// float and normalized formats, integers for the integer ones), components beyond the format's are dropped
void buf_store_format(BufAddr ba, uvec4 data, uint ncomp) {
    uint fmt = (ba.w3 >> 12) & 0x7fu;
    uint info = fmt < 78u ? FORMATS[fmt] : 0u;
    uint n = info & 7u, bits = (info >> 3) & 63u, type = info >> 9;
    if (n == 0u && pc.counters != 0ul) atomicAdd(Counters(pc.counters).v[1], 1u);
    for (uint k = 0u; k < min(n, ncomp) && ba.ok; k++) {
        uint64_t a = ba.dev + uint64_t(k * (bits >> 3));
        float f = uintBitsToFloat(data[k]);
        uint mask = bits == 32u ? 0xffffffffu : (1u << bits) - 1u;
        uint raw;
        if (type == 0u) raw = bits == 32u ? data[k] : (packHalf2x16(vec2(f, 0.0)) & 0xffffu);
        else if (type == 1u) raw = uint(roundEven(clamp(isnan(f) ? 0.0 : f, 0.0, 1.0) * float(mask)));
        else if (type == 2u) {
            float maxs = bits == 32u ? 2147483647.0 : float((1u << (bits - 1u)) - 1u);
            raw = uint(int(roundEven(clamp(isnan(f) ? 0.0 : f, -1.0, 1.0) * maxs))) & mask;
        } else raw = data[k] & mask;
        if (bits == 32u) W32(a).v = raw;
        else if (bits == 16u) W16(a).v = uint16_t(raw);
        else W8(a).v = uint8_t(raw);
    }
}
vec4 buf_load_format(BufAddr ba) {
    uint fmt = (ba.w3 >> 12) & 0x7fu;
    uint info = fmt < 78u ? FORMATS[fmt] : 0u;
    uint n = info & 7u, bits = (info >> 3) & 63u, type = info >> 9;
    if (n == 0u && pc.counters != 0ul) atomicAdd(Counters(pc.counters).v[1], 1u);
    float c[4] = float[4](0.0, 0.0, 0.0, 0.0);
    for (uint k = 0u; k < n && ba.ok; k++) {
        uint64_t a = ba.dev + uint64_t(k * (bits >> 3));
        uint raw = bits == 32u ? W32(a).v : (bits == 16u ? uint(W16(a).v) : uint(W8(a).v));
        int sraw = bits == 32u ? int(raw) : bitfieldExtract(int(raw), 0, int(bits));
        float maxu = bits == 32u ? 4294967295.0 : float((1u << bits) - 1u);
        float maxs = bits == 32u ? 2147483647.0 : float((1u << (bits - 1u)) - 1u);
        float v;
        if (type == 0u) v = bits == 32u ? uintBitsToFloat(raw) : unpackHalf2x16(raw).x;
        else if (type == 1u) v = unorm_exact(raw, bits);
        else if (type == 2u) v = max(snorm_exact(sraw, bits), -1.0);
        else if (type == 3u) v = float(raw);
        else if (type == 4u) v = float(sraw);
        else if (type == 5u) v = uintBitsToFloat(raw);
        else v = uintBitsToFloat(uint(sraw));
        c[k] = v;
    }
    float one = (type == 5u || type == 6u) ? uintBitsToFloat(1u) : 1.0;
    vec4 r;
    for (uint k = 0u; k < 4u; k++) {
        // dst_sel: 0 -> 0, 1 -> 1, 4..7 -> x..w; a component the format lacks reads 0 (w: 1)
        uint sel = (ba.w3 >> (3u * k)) & 7u;
        float v = sel == 1u ? one : 0.0;
        if (sel >= 4u) v = sel - 4u < n ? c[sel - 4u] : (sel == 7u ? one : 0.0);
        r[k] = v;
    }
    return r;
}

// ---------------------------------------------------------------- float helpers with GCN semantics
float fma_unfused(float a, float b, float c) { precise float t = a * b; precise float r = t + c; return r; }
float med3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
uint med3_u32(uint a, uint b, uint c) { return max(min(a, b), min(max(a, b), c)); }
int med3_i32(int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); }
float fmin_ieee(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }
float fmax_ieee(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }
float rcp_f32(float x) { return 1.0 / x; }
float rsq_f32(float x) { return inversesqrt(x); }
#if SOFT_SIN
// debugging (SHADER_SR_SOFT_SIN=1): a software sin of 2*pi*x from multiplies and adds only, so two compiled shaders
// give bit-identical results (the driver's sin depends on how it optimises the surrounding code)
float sin_rev(float x) {
    float r = x - floor(x + 0.5);                       // [-0.5, 0.5] revolutions
    float a = r * 6.28318530717958647692;
    float a2 = a * a;
    float p = -2.39e-08;
    p = p * a2 + 2.7526e-06;
    p = p * a2 - 1.98409e-04;
    p = p * a2 + 8.3333315e-03;
    p = p * a2 - 1.6666666e-01;
    return a + a * (a2 * p);
}
float cos_rev(float x) { return sin_rev(x + 0.25); }
#else
float sin_rev(float x) { return sin(x * 6.28318530717958647692); }
float cos_rev(float x) { return cos(x * 6.28318530717958647692); }
#endif
float fract_f32(float x) { return isinf(x) ? 0.0 : min(x - floor(x), uintBitsToFloat(0x3f7fffffu)); }
uint cvt_u32_f32(float x) { return isnan(x) ? 0u : (x >= 4294967296.0 ? 0xffffffffu : (x <= 0.0 ? 0u : uint(x))); }
uint mulhi_u32(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }
// float -> int with GCN's saturation (NaN -> 0)
int cvt_i32_f32(float x) { return isnan(x) ? 0 : (x >= 2147483648.0 ? 0x7fffffff : (x <= -2147483648.0 ? int(0x80000000u) : int(x))); }
int cvt_flr_i32_f32(float x) { return cvt_i32_f32(floor(x)); }
// bitfield extract with GCN's operands (offset and width masked to 5 bits; width 0 -> 0)
uint bfe_u32(uint a, uint o, uint w) {
    o &= 31u; w &= 31u;
    return w == 0u ? 0u : (o + w < 32u ? bitfieldExtract(a, int(o), int(w)) : a >> o);
}
// s_bfe_u64: src1 [5:0] offset, [22:16] width
uint64_t bfe_u64(uint64_t a, uint c) {
    uint o = c & 63u, w = (c >> 16) & 127u;
    uint64_t x = a >> o;
    return w == 0u ? 0ul : (w >= 64u ? x : x & ((1ul << w) - 1ul));
}
uint bfe_i32(uint a, uint o, uint w) {
    o &= 31u; w &= 31u;
    return w == 0u ? 0u : uint(o + w < 32u ? bitfieldExtract(int(a), int(o), int(w)) : int(a) >> o);
}
// cube map helpers (v_cubeid / sc / tc / ma): the major axis is z, then y, then x
float cube_id(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    return (az >= ax && az >= ay) ? (z < 0.0 ? 5.0 : 4.0) : ((ay >= ax) ? (y < 0.0 ? 3.0 : 2.0) : (x < 0.0 ? 1.0 : 0.0));
}
float cube_sc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    return (az >= ax && az >= ay) ? (z < 0.0 ? -x : x) : ((ay >= ax) ? x : (x < 0.0 ? z : -z));
}
float cube_tc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    return (az >= ax && az >= ay) ? -y : ((ay >= ax) ? (y < 0.0 ? -z : z) : -y);
}
float cube_ma(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    return (az >= ax && az >= ay) ? 2.0 * z : ((ay >= ax) ? 2.0 * y : 2.0 * x);
}
// a cube image as Kyty binds it: a 2D array of faces; the guest's layer coordinate is slice * 8 + face
uint cube_layer(uint guest) { return guest - ((guest >> 3) << 1); }
// f32 -> f16 bits rounding toward zero (v_cvt_pkrtz_f16_f32)
uint f2h_rtz(float f) {
    uint x = floatBitsToUint(f);
    uint sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (e == 0xffu) return sign | 0x7c00u | (m != 0u ? 0x200u | (m >> 13) : 0u);
    int ue = int(e) - 112;
    if (ue >= 31) return sign | 0x7bffu;
    if (ue <= 0) return ue < -10 ? sign : sign | ((m | 0x800000u) >> uint(14 - ue));
    return sign | (uint(ue) << 10) | (m >> 13);
}
bool cmp_class(float x, uint mask) {
    uint bits = floatBitsToUint(x);
    uint neg = bits >> 31, e = (bits >> 23) & 0xffu, m = bits & 0x7fffffu;
    uint nan = (m & 0x400000u) != 0u ? 1u : 0u;
    uint special = m != 0u ? nan : (neg != 0u ? 2u : 9u);
    uint zero = m == 0u ? (neg != 0u ? 5u : 6u) : (neg != 0u ? 4u : 7u);
    uint cls = e == 0xffu ? special : (e == 0u ? zero : (neg != 0u ? 3u : 8u));
    return ((mask >> cls) & 1u) != 0u;
}
bool lane_bit(uint lo, uint hi, uint lane) { return ((lane < 32u ? lo >> lane : hi >> (lane - 32u)) & 1u) != 0u; }
// a 64-bit lane mask held as a value -> the thread's two bits
uint lane_bits2(uint lo, uint hi) {
    return ((lane_bit(lo, hi, LANE0) ? 1u : 0u) | (lane_bit(lo, hi, LANE1) ? 2u : 0u)) & LMASK;
}

#if !GRAPHICS
// ---------------------------------------------------------------- LDS atomics
void lds_atomic_min_f32(uint a, uint v) {
    uint old = LDS[a];
    for (;;) {
        float o = uintBitsToFloat(old), n = min(o, uintBitsToFloat(v));
        if (floatBitsToUint(n) == old) break;
        uint prev = atomicCompSwap(LDS[a], old, floatBitsToUint(n));
        if (prev == old) break;
        old = prev;
    }
}
void lds_atomic_max_f32(uint a, uint v) {
    uint old = LDS[a];
    for (;;) {
        float o = uintBitsToFloat(old), n = max(o, uintBitsToFloat(v));
        if (floatBitsToUint(n) == old) break;
        uint prev = atomicCompSwap(LDS[a], old, floatBitsToUint(n));
        if (prev == old) break;
        old = prev;
    }
}

// ---------------------------------------------------------------- wave operations (one guest wave = one warp)
// LDS written by other lanes of the wave: the warp's shared memory accesses are ordered by a subgroup barrier
void wave_sync() {
    subgroupMemoryBarrierShared();
    subgroupBarrier();
}
#endif
bool wave_any(bool b) { return subgroupAny(b); }
// the wave's 64-bit mask of a per-lane condition (lanes j: c0, lanes j + 32: c1)
uvec2 wave_ballot2(bool c0, bool c1) { return uvec2(subgroupBallot(c0).x, subgroupBallot(c1).x); }
uint wave_readlane2(uint x0, uint x1, uint lane) {
    uint a = subgroupShuffle(x0, lane & 31u), b = subgroupShuffle(x1, lane & 31u);
    return (lane & 63u) < 32u ? a : b;
}
uint own_first(uint x0, uint x1) { return x0; }   // readfirstlane of a wave-uniform value, whole wave active
// v_readfirstlane_b32 where lanes may be inactive: the value of the first lane with exec set
uint wave_readfirst2(uint x0, uint x1) {
    uvec2 b = wave_ballot2(EXB0, EXB1);
    uint l = b.x != 0u ? uint(findLSB(b.x)) : (b.y != 0u ? 32u + uint(findLSB(b.y)) : 0u);
    return wave_readlane2(x0, x1, l);
}
// v_permlanex16_b32 with both lane selects 0xffffffff: every lane reads lane 15 of the other 16-lane row
uint permlanex16_ff(uint x) { return subgroupShuffle(x, gl_SubgroupInvocationID < 16u ? 31u : 15u); }
'''
    graphics = stage != 'cs'
    text = text.replace('PUSH_BLOCK', GRAPHICS_PUSH if graphics else COMPUTE_PUSH)
    text = text.replace('LOCAL_SIZE_DECL', '' if graphics else 'layout(local_size_x = HOST_THREADS) in;')
    text = text.replace('QUAD_EXTENSION', '#extension GL_KHR_shader_subgroup_quad : require\n'
                        '#extension GL_EXT_fragment_shader_barycentric : enable' if stage == 'ps' else '')
    text = text.replace('GRAPHICS_ON', '1' if graphics else '0').replace('LMASK_BITS', '1u' if graphics else '3u')
    if graphics:
        # no debug counters or probes in the graphics push block
        # (variables, not constants: glslang's constant folding of a buffer reference made from 0ul asserts)
        text = text.replace('pc.counters', 'GFX_COUNTERS').replace('pc.debug', 'GFX_DEBUG')
        probes = False
    return text.replace('PROBES_ON', '1' if probes else '0').replace('SOFT_SIN_ON', '1' if os.environ.get('SHADER_SR_SOFT_SIN') else '0').replace('NO_HANDSHAKE_ON', '1' if os.environ.get('SHADER_SR_NO_HANDSHAKE') else '0').replace('HOST_THREADS', str(threads // 2 if threads >= 64 else 32)).replace('THREADS', str(threads)).replace('LDS_DWORDS', str(lds_dwords)).replace(
        'NVGPR', str(nvgpr)).replace('FORMAT_TABLE', ', '.join('%du' % v for v in format_table()))
