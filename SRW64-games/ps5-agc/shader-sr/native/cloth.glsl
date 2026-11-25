// cloth.glsl - native NVIDIA rewrite of the guest's GPU cloth solver (one cloth instance per workgroup, NT threads,
// subgroups of 32, no guest-wave emulation). It reproduces the guest program operation for operation: every
// multiply and add rounds alone (native.py decorates them NoContraction), fma() is written where the guest fuses
// (v_fma_mix), and rcp / rsq / sqrt / exp2 / log2 / sin are the same expressions the translator uses. Built by
// native.py on top of the shader-sr runtime (prelude.py: guest memory through the emulator's BDA page table, V#
// format decoding, batched user data), so it drops in where the translation of the guest program would.
//
// Notation as in the reverse-engineering notes: T = the instance's 304-byte table (s[0:1]), P = *(T + 8) (the piece
// data), T.V#(x) / P.V#(x) = buffer descriptors at byte x, h(x) = f16 at byte x of T.

#ifndef NT
#define NT 1024
#endif
#ifndef SHARED_STORAGE
#define SHARED_STORAGE 0
#endif
#ifndef BEND_LIMIT
#define BEND_LIMIT 0xffffffffu      // the 64- and 512-thread programs process only the first 64 / 512 bending constraints of a group
#endif
#ifndef STOP
#define STOP 0                      // debugging: 1 after the prediction, 2 before the iterations, 3 after iteration 1
#endif
#define EPS4 uintBitsToFloat(0x38d1b717u)      // 1e-4
#define EPS6 uintBitsToFloat(0x358637bdu)      // 1e-6
#define TINY uintBitsToFloat(0x0da24260u)      // 1e-30
#define FLT_MAX uintBitsToFloat(0x7f7fffffu)

// ------------------------------------------------------------------------------------------- instance data
shared uint sT[76];                 // T, 304 bytes
shared uint sP[72];                 // P header, 0x120 bytes
shared uint64_t sTaddr;
uint Tu(uint byte) { return sT[byte >> 2]; }
float Tf(uint byte) { return uintBitsToFloat(sT[byte >> 2]); }
float Th(uint byte) { return unpackHalf2x16(sT[byte >> 2] >> ((byte & 2u) * 8u)).x; }
uint Tb(uint byte) { return (sT[byte >> 2] >> ((byte & 3u) * 8u)) & 0xffu; }
uint Pu(uint byte) { return sP[byte >> 2]; }

// a buffer descriptor with its base translated once (one device buffer holds the guest buffer)
struct VB { uint64_t dev; uint stride; uint num; uint w1; uint w3; };
VB vb_make(uint w0, uint w1, uint w2, uint w3) {
    VB b;
    uint64_t base = packUint2x32(uvec2(w0, w1 & 0xffffu));
    // empty descriptors (num_records 0, a dummy base) are never accessed: no translation (no fault for their page)
    b.dev = (base == 0ul || w2 == 0u) ? 0ul : xlate(base);
    b.stride = (w1 >> 16) & 0x3fffu;
    b.num = w2;
    b.w1 = w1;
    b.w3 = w3;
    return b;
}
VB vb_T(uint byte) { return vb_make(sT[byte >> 2], sT[(byte >> 2) + 1u], sT[(byte >> 2) + 2u], sT[(byte >> 2) + 3u]); }
VB vb_P(uint byte) { return vb_make(sP[byte >> 2], sP[(byte >> 2) + 1u], sP[(byte >> 2) + 2u], sP[(byte >> 2) + 3u]); }
bool vb_ok(VB b, uint index, uint offset) {
    return b.dev != 0ul && (b.stride != 0u ? index < b.num : offset < b.num);
}
uint64_t vb_at(VB b, uint index, uint offset) { return b.dev + uint64_t(b.stride * index + offset); }
uint vb_u32(VB b, uint index, uint offset) { return vb_ok(b, index, offset) ? W32(vb_at(b, index, offset)).v : 0u; }
float vb_f32(VB b, uint index, uint offset) { return uintBitsToFloat(vb_u32(b, index, offset)); }
vec3 vb_f3(VB b, uint index, uint offset) {
    if (!vb_ok(b, index, offset)) return vec3(0.0);
    uint64_t a = vb_at(b, index, offset);
    return vec3(uintBitsToFloat(W32(a).v), uintBitsToFloat(W32(a + 4ul).v), uintBitsToFloat(W32(a + 8ul).v));
}
vec4 vb_f4(VB b, uint index, uint offset) {
    if (!vb_ok(b, index, offset)) return vec4(0.0);
    uint64_t a = vb_at(b, index, offset);
    return vec4(uintBitsToFloat(W32(a).v), uintBitsToFloat(W32(a + 4ul).v), uintBitsToFloat(W32(a + 8ul).v),
                uintBitsToFloat(W32(a + 12ul).v));
}
int vb_i16(VB b, uint index, uint offset) {
    return vb_ok(b, index, offset) ? int(int16_t(W16(vb_at(b, index, offset)).v)) : 0;
}
uint vb_u16(VB b, uint index, uint offset) { return vb_ok(b, index, offset) ? uint(W16(vb_at(b, index, offset)).v) : 0u; }
float vb_h(VB b, uint index, uint offset) { return unpackHalf2x16(vb_u16(b, index, offset)).x; }
void vb_store3(VB b, uint index, vec3 v) {
    if (!vb_ok(b, index, 0u)) return;
    uint64_t a = vb_at(b, index, 0u);
    W32(a).v = floatBitsToUint(v.x);
    W32(a + 4ul).v = floatBitsToUint(v.y);
    W32(a + 8ul).v = floatBitsToUint(v.z);
}
void vb_store16(VB b, uint index, uint offset, uint v) { if (vb_ok(b, index, offset)) W16(vb_at(b, index, offset)).v = uint16_t(v); }
// buffer_load_format_*: the prelude's decoder (format and dst_sel from word 3)
vec4 vb_fmt(VB b, uint index) {
    BufAddr a;
    a.dev = vb_at(b, index, 0u);
    a.ok = vb_ok(b, index, 0u);
    a.w1 = b.w1;
    a.w3 = b.w3;
    return buf_load_format(a);
}
// guest memory at an address (scalar loads, global loads)
uint gld32(uint64_t a) { return ldd32(xlate(a)); }
float gldf(uint64_t a) { return uintBitsToFloat(gld32(a)); }
uint64_t gld64(uint64_t a) { return packUint2x32(uvec2(gld32(a), gld32(a + 4ul))); }

// ------------------------------------------------------------------------------------------- float helpers
float dot3(vec3 a, vec3 b) { float t = a.x * b.x; t = a.y * b.y + t; t = a.z * b.z + t; return t; }
vec3 cross3(vec3 a, vec3 b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
bool fin(float r) { return cmp_class(-r, 56u); }                 // r is +0, +denormal or +normal
vec3 snorm(vec3 v) { float r = rsq_f32(dot3(v, v)); return fin(r) ? vec3(v.x * r, v.y * r, v.z * r) : vec3(0.0); }
vec3 nrm(vec3 v) { float r = rsq_f32(dot3(v, v)); return vec3(v.x * r, v.y * r, v.z * r); }
float clamp01(float x) { return clamp(x, 0.0, 1.0); }
bool finite3(vec3 p) { return cmp_class(-abs(p.z), 56u) && cmp_class(-abs(p.y), 56u) && cmp_class(-abs(p.x), 56u); }

// ------------------------------------------------------------------------------------------- storage
// anchors A[i] and positions X[i]. The 1024-thread program keeps them in memory: anchors in the global table's second
// buffer at T[0xbc] + i, positions in T.V#(0x38). The smaller programs (SHARED_STORAGE, at most MAXV vertices) keep
// both in shared memory and write the positions to T.V#(0x38) at the end; with one substep (the game's case) the
// results are the same, the difference is where the intermediate values live.
VB vbA, vbX;                        // per thread copies (uniform)
uint offA, offX;                    // element offset of this instance's vertex 0
#if SHARED_STORAGE
shared float sAx[MAXV], sAy[MAXV], sAz[MAXV], sXx[MAXV], sXy[MAXV], sXz[MAXV];
// out-of-range indices read 0 (the guest's LDS reads beyond its allocation do)
vec3 A_get(int i) { return uint(i) < uint(MAXV) ? vec3(sAx[i], sAy[i], sAz[i]) : vec3(0.0); }
vec3 X_get(int i) { return uint(i) < uint(MAXV) ? vec3(sXx[i], sXy[i], sXz[i]) : vec3(0.0); }
void A_set(int i, vec3 p) { if (uint(i) < uint(MAXV)) { sAx[i] = p.x; sAy[i] = p.y; sAz[i] = p.z; } }
void X_set(int i, vec3 p) { if (uint(i) < uint(MAXV)) { sXx[i] = p.x; sXy[i] = p.y; sXz[i] = p.z; } }
#else
vec3 A_get(int i) { return vb_f3(vbA, offA + uint(i), 0u); }
vec3 X_get(int i) { return vb_f3(vbX, offX + uint(i), 0u); }
void A_set(int i, vec3 p) { vb_store3(vbA, offA + uint(i), p); }
void X_set(int i, vec3 p) { vb_store3(vbX, offX + uint(i), p); }
#endif

// ------------------------------------------------------------------------------------------- common descriptors
VB vbPal;                           // bone palette (3 rows of float4 per bone)
VB vbT18, vbT28, vbT38;             // collider set table, current / previous positions
uint64_t gObj;                      // object block *(T+0x10)
float objscale;
uint P4;                            // P[4] flags
int nverts;

ivec4 remap_bones(ivec4 bi) {
    uint e4 = Tu(0xe4u);
    for (int k = 0; k < 4; k++)
        if (bi[k] < -1) bi[k] = int(e4 + 0xfffffffeu - uint(bi[k]));   // T[0xe4] - 2 - i (u32)
    return bi;
}
// one bone row applied to a point, as the guest evaluates it: ((r.x*p.x + r.w) + r.y*p.y) + r.z*p.z
float row_xf(vec4 r, vec3 p) { float t = r.x * p.x + r.w; t = r.y * p.y + t; t = r.z * p.z + t; return t; }

// ------------------------------------------------------------------------------------------- section A: anchors
vec3 anchor_of(uint v) {
    VB b18 = vb_P(0x18u);
    vec3 bp = vb_fmt(b18, v).xyz;
    if ((P4 & 1u) != 0u) {
        VB b28 = vb_P(0x28u), b38 = vb_P(0x38u);
        int s12 = int(Pu(8u)), s13 = int(Pu(0xcu));
        vec3 p1 = bp;
        int g = 0;
        if (s13 > 0) {
            p1 = vec3(0.0);
            for (; g < s13; g++) {
                uint row = uint(g) + uint(s12) * v;
                ivec4 bi = remap_bones(floatBitsToInt(vb_fmt(b28, row)));
                vec4 wv = vb_fmt(b38, row);
                for (int k = 0; k < 4; k++) {
                    uint b = uint(bi[k]);
                    vec4 r0 = vb_f4(vbPal, b, 0u), r1 = vb_f4(vbPal, b, 16u), r2 = vb_f4(vbPal, b, 32u);
                    p1.x = row_xf(r0, bp) * wv[k] + p1.x;
                    p1.y = row_xf(r1, bp) * wv[k] + p1.y;
                    p1.z = row_xf(r2, bp) * wv[k] + p1.z;
                }
            }
        }
        vec3 p2 = vec3(0.0);
        for (; g < s12; g++) {
            uint row = uint(g) + uint(s12) * v;
            ivec4 bi = remap_bones(floatBitsToInt(vb_fmt(b28, row)));
            vec4 wv = vb_fmt(b38, row);
            for (int k = 0; k < 4; k++) {
                uint b = uint(bi[k]);
                vec4 r0 = vb_f4(vbPal, b, 0u), r1 = vb_f4(vbPal, b, 16u), r2 = vb_f4(vbPal, b, 32u);
                p2.x = row_xf(r0, p1) * wv[k] + p2.x;
                p2.y = row_xf(r1, p1) * wv[k] + p2.y;
                p2.z = row_xf(r2, p1) * wv[k] + p2.z;
            }
        }
        return p2;
    }
    // rigid: object matrix rows at +4, +0x14, +0x24
    vec4 m0 = vec4(gldf(gObj + 4ul), gldf(gObj + 8ul), gldf(gObj + 12ul), gldf(gObj + 16ul));
    vec4 m1 = vec4(gldf(gObj + 20ul), gldf(gObj + 24ul), gldf(gObj + 28ul), gldf(gObj + 32ul));
    vec4 m2 = vec4(gldf(gObj + 36ul), gldf(gObj + 40ul), gldf(gObj + 44ul), gldf(gObj + 48ul));
    return vec3(row_xf(m0, bp), row_xf(m1, bp), row_xf(m2, bp));
}

// ------------------------------------------------------------------------------------------- per-vertex prediction
VB vbP18, vbP28, vbP38, vbP58, vbP68, vbP78, vbP88, vbP98, vbPA8, vbPB8, vbP108, vbP48, vbPD8;
float dt, idt;                      // T.f[0xcc]; v6: T.f[0xc8] in the first substep, rcp(dt) later
uint F;                             // T[0xd4]

// skinned backstop normal (stage-2 bone blend; T[0x108] bit 10: weight-only bones and the anchor-mesh normal)
vec3 skinned_normal(uint v, vec3 n) {
    vec3 N = vec3(0.0);
    float wsum = 0.0;
    uint flag = vb_u16(vbP108, v, 0u);
    bool b10 = (Tu(0x108u) & 0x400u) != 0u;
    int s12 = int(Pu(8u));
    for (int gi = int(Pu(0xcu)); gi < s12; gi++) {
        uint row = v * uint(s12) + uint(gi);
        ivec4 bi = floatBitsToInt(vb_fmt(vbP28, row));
        vec4 wv = vb_fmt(vbP38, row);
        for (int k = 0; k < 4; k++) {
            int i = bi[k];
            bool use;
            if (i < -1) { i = int(Tu(0xe4u) + 0xfffffffeu - uint(i)); use = true; }
            else if (b10) {
                if (flag != 0u) use = true;
                else { use = false; wsum = wsum + wv[k]; }
            } else use = false;
            if (use) {
                vec3 r0 = vb_f3(vbPal, uint(i), 0u), r1 = vb_f3(vbPal, uint(i), 16u), r2 = vb_f3(vbPal, uint(i), 32u);
                vec3 nb = snorm(vec3(dot3(r0, n), dot3(r1, n), dot3(r2, n)));
                N = vec3(nb.x * wv[k] + N.x, nb.y * wv[k] + N.y, nb.z * wv[k] + N.z);
            }
        }
    }
    if (b10 && 0.0 < wsum) {
        int j2 = vb_i16(vbP108, v, 2u), j4 = vb_i16(vbP108, v, 4u), j6 = vb_i16(vbP108, v, 6u), j8 = vb_i16(vbP108, v, 8u);
        vec3 e1 = A_get(j4) - A_get(j2), e2 = A_get(j8) - A_get(j6);
        vec3 nm = snorm(cross3(e1, e2));
        N = vec3(wsum * nm.x + N.x, wsum * nm.y + N.y, wsum * nm.z + N.z);
    }
    return N;
}

vec3 rigid_normal(vec3 n) {
    if ((Tu(0x108u) & 0x400u) == 0u) return vec3(0.0);
    vec3 m0 = vec3(gldf(gObj + 4ul), gldf(gObj + 8ul), gldf(gObj + 12ul));
    vec3 m1 = vec3(gldf(gObj + 20ul), gldf(gObj + 24ul), gldf(gObj + 28ul));
    vec3 m2 = vec3(gldf(gObj + 36ul), gldf(gObj + 40ul), gldf(gObj + 44ul));
    return snorm(vec3(dot3(m0, n), dot3(m1, n), dot3(m2, n)));
}

// max distance around the anchor or the backstop center (the guest's 0x2bd8 and 0x8048 copies), when !(h64 < 0)
vec3 skin_constrain(uint v, vec3 p) {
    float h64 = Th(0x64u);
    if (h64 < 0.0) return p;
    float md = vb_fmt(vbP58, v).x;
    vec3 Av = A_get(int(v));
    float R = objscale * h64;
    R = R * md;
    vec3 c = Av;
    if ((P4 & 4u) != 0u) {
        float wb = (P4 & 8u) != 0u ? vb_fmt(vbP78, v).x : 1.0;
        if (0.0 < wb) {
            vec2 uv = vb_fmt(vbPB8, v).xy;
            float x0 = clamp01(max(0.0, uv.x)), y0 = clamp01(max(0.0, uv.y));
            bool sx = 0.5 <= uv.x, sy = 0.5 <= uv.y;
            float x = 2.0 * x0 + (-1.0), y = 2.0 * y0 + (-1.0);
            float z = 1.0 + ((-abs(x)) + (-abs(y)));
            float t = med3(1.0, -z, 0.0);
            x = (sx ? -t : t) + x;
            y = (sy ? -t : t) + y;
            vec3 xyz = vec3(x, y, z);
            float r = rsq_f32(dot3(xyz, xyz));
            vec3 n = vec3(r * x, r * y, r * z);
            vec3 N = (P4 & 1u) != 0u ? skinned_normal(v, n) : rigid_normal(n);
            float sR = wb * R;
            c = vec3(sR * N.x + Av.x, sR * N.y + Av.y, sR * N.z + Av.z);
        }
    }
    vec3 d = p - c;
    float R2 = R * R;
    float dd = max(EPS4, dot3(d, d));
    if (dd > R2) {
        float s = rsq_f32(dd) * R;
        p = vec3(s * d.x + c.x, s * d.y + c.y, s * d.z + c.z);
    }
    return p;
}

// tethers to the anchors (T[0x108] bit 11: segment tether, else two spheres); the guest's two copies are identical
vec3 tethers(uint v, vec3 p) {
    if ((Tu(0x108u) & 0x800u) != 0u) {
        float d0 = vb_f32(vbPA8, v, 4u), d1 = vb_f32(vbPA8, v, 8u);
        if (!(min(d0, d1) <= 0.0)) {
            uint ab = vb_u32(vbPA8, v, 0u);
            int ia = int(int16_t(uint16_t(ab & 0xffffu))), ib = int(int16_t(uint16_t(ab >> 16)));
            vec3 Av = A_get(int(v)), Aa = A_get(ia), Ab = A_get(ib);
            vec3 e = Ab - Aa;
            float ee = dot3(e, e);
            if (!(ee < EPS4)) {
                float dAv = dot3(Av, e), dAa = dot3(Aa, e);
                float r = rcp_f32(ee);
                float t = dAv - dAa;
                t = r * t;
                vec3 P0 = vec3(t * e.x + Aa.x, t * e.y + Aa.y, t * e.z + Aa.z);
                vec3 hv = Av - P0;
                float hh = dot3(hv, hv);
                if (!(hh < EPS4)) {
                    float dp = dot3(p, e);
                    vec3 u0 = p - P0;
                    float L = sqrt(hh);
                    float w = dAv - dp;
                    w = r * w;
                    vec3 u = vec3(w * e.x + u0.x, w * e.y + u0.y, w * e.z + u0.z);
                    vec3 n = snorm(u);
                    p = vec3(L * n.x + P0.x, L * n.y + P0.y, L * n.z + P0.z);
                }
            }
        }
        return p;
    }
    float d0 = vb_f32(vbPA8, v, 4u);
    float r0 = Th(0x54u) * d0;
    if (!(r0 <= 0.0)) {
        int i0 = vb_i16(vbPA8, v, 0u);
        float r0s = r0 * r0;
        vec3 C0 = A_get(i0);
        vec3 d = p - C0;
        float dd = dot3(d, d);
        if (dd > r0s) {
            float g = rsq_f32(max(EPS4, dd));
            vec3 n = vec3(g * d.x, g * d.y, g * d.z);
            p = vec3(n.x * r0 + C0.x, n.y * r0 + C0.y, n.z * r0 + C0.z);
        }
        float d1 = vb_f32(vbPA8, v, 8u);
        float r1 = Th(0x56u) * d1;
        if (!(r1 <= 0.0)) {
            int i1 = vb_i16(vbPA8, v, 2u);
            float r1s = r1 * r1;
            vec3 C1 = A_get(i1);
            vec3 dv = p - C1;
            float dd1 = dot3(dv, dv);
            if (!(dd1 <= r1s)) {
                float f = rsq_f32(max(EPS4, dd1)) * r1;
                p = vec3(f * dv.x + C1.x, f * dv.y + C1.y, f * dv.z + C1.z);
                vec3 q = p - C0;
                float t = dv.x * q.x;
                float nz = (-q.z) * dv.z;
                t = q.y * dv.y + t;
                if (!(t >= nz)) {
                    float qq = dot3(q, q);
                    if (qq > r0s) {
                        vec3 e = C1 - C0;
                        float rs = r0 + r1;
                        float L2 = dot3(e, e);
                        float L = sqrt(L2);
                        if (L >= rs) {
                            float f2 = rcp_f32(rs);
                            f2 = f2 * r0;
                            p = vec3(e.x * f2 + C0.x, e.y * f2 + C0.y, e.z * f2 + C0.z);
                        } else {
                            vec3 c = cross3(e, q);
                            float k = r0s - r1s;
                            float gL = rsq_f32(L2);
                            float hn = k + L2;
                            float hh = (gL * hn) * 0.5;
                            vec3 nE = fin(gL) ? vec3(gL * e.x, gL * e.y, gL * e.z) : vec3(0.0);
                            vec3 m = cross3(c, e);
                            float rr = hh * (-hh) + r0s;
                            rr = max(0.0, rr);
                            vec3 p0 = vec3(nE.x * hh + C0.x, nE.y * hh + C0.y, nE.z * hh + C0.z);
                            float rho = sqrt(rr);
                            float mm = dot3(m, m);
                            float gm = rsq_f32(mm);
                            vec3 nm = fin(gm) ? vec3(gm * m.x, gm * m.y, gm * m.z) : vec3(0.0);
                            p = vec3(nm.x * rho + p0.x, nm.y * rho + p0.y, nm.z * rho + p0.z);
                        }
                    }
                }
            }
        }
    }
    return p;
}

// ------------------------------------------------------------------------------------------- prediction (one vertex)
vec3 predict(uint v) {
    vec3 cr = vb_f3(vbT28, v, 0u), pr = vb_f3(vbT38, v, 0u);
    vec3 cur = vec3(Tf(0x48u) + cr.x, Tf(0x4cu) + cr.y, Tf(0x50u) + cr.z);
    vec3 prev = vec3(Tf(0x58u) + pr.x, Tf(0x5cu) + pr.y, Tf(0x60u) + pr.z);
    vec3 vel = cur - prev;
    if ((F & 16u) == 0u) {
        vec3 t = vec3(idt * (-vel.x), idt * (-vel.y), idt * (-vel.z));
        float dd = Tf(0xd0u) - dt;
        cur = vec3(dd * t.x + cur.x, dd * t.y + cur.y, dd * t.z + cur.z);
    }
    // gravity, attenuated by the up axis of the bones (or of the object)
    float g = Th(0x84u);
    if (Th(0x86u) > 0.0) {
        vec3 c;
        if ((P4 & 1u) != 0u) {
            c = vec3(0.0);
            int s12 = int(Pu(8u));
            for (int gi = int(Pu(0xcu)); gi < s12; gi++) {
                uint row = v * uint(s12) + uint(gi);
                ivec4 bi = remap_bones(floatBitsToInt(vb_fmt(vbP28, row)));
                vec4 wv = vb_fmt(vbP38, row);
                for (int k = 0; k < 4; k++) {
                    uint b = uint(bi[k]);
                    float m0 = vb_f32(vbPal, b, 8u), m1 = vb_f32(vbPal, b, 24u), m2 = vb_f32(vbPal, b, 40u);
                    c.x = m0 * wv[k] + c.x;
                    c.y = m1 * wv[k] + c.y;
                    c.z = m2 * wv[k] + c.z;
                }
            }
        } else c = vec3(gldf(gObj + 0xcul), gldf(gObj + 0x1cul), gldf(gObj + 0x2cul));
        float gh = g * Th(0x86u);
        float r = rsq_f32(dot3(c, c));
        float u = clamp01(r * (-c.z) + 1.0);
        u = u + (-1.0);
        g = u * gh + g;
    }
    float w = (P4 & 2u) != 0u ? vb_fmt(vbP68, v).x : 1.0;
    // drag and its limiter, external acceleration
    float h96 = Th(0x96u), h74 = Th(0x74u), h76 = Th(0x76u);
    float idw = idt * w;
    float dtp2 = Tf(0xd0u) * Tf(0xd0u);
    vec3 rel = vec3(vel.x - Tf(0x78u), vel.y - Tf(0x7cu), vel.z - Tf(0x80u));
    vec3 dr = vec3(idt * (-rel.x), idt * (-rel.y), idt * (-rel.z));
    float k = idw * h96;
    float lim = uintBitsToFloat(0x3f733333u) * h74;
    dr = vec3(dr.x * h76, dr.y * h76, dr.z * h76);
    k = k * h76;
    k = k * dtp2;
    if (k > lim) {
        float s = lim * rcp_f32(k);
        dr = vec3(dr.x * s, dr.y * s, dr.z * s);
    }
    vec3 a = vec3(w * (Tf(0x88u) + dr.x), w * (Tf(0x8cu) + dr.y), w * (Tf(0x90u) + dr.z));
    vec3 acc = vec3(0.0, 0.0, g);
    // wind, optionally projected on the edge or the normal of the stored positions
    if (h96 > 0.0) {
        vec3 ap = a;
        float h94 = Th(0x94u);
        if (h94 > 0.0) {
            uint flag = vb_u16(vbP108, v, 0u);
            int i2 = vb_i16(vbP108, v, 2u), i4 = vb_i16(vbP108, v, 4u);
            vec3 r;
            if (flag != 0u) {
                vec3 e = snorm(vb_f3(vbT28, uint(i4), 0u) - vb_f3(vbT28, uint(i2), 0u));
                vec3 cc = cross3(a, e);
                vec3 x = cross3(e, cc);
                r = vec3(x.x * 0.5, x.y * 0.5, x.z * 0.5);
            } else {
                int i6 = vb_i16(vbP108, v, 6u), i8 = vb_i16(vbP108, v, 8u);
                vec3 n = snorm(cross3(vb_f3(vbT28, uint(i4), 0u) - vb_f3(vbT28, uint(i2), 0u),
                                      vb_f3(vbT28, uint(i8), 0u) - vb_f3(vbT28, uint(i6), 0u)));
                float d = dot3(n, a);
                r = vec3(n.x * d, n.y * d, n.z * d);
            }
            ap = vec3(fma(h94, r.x - a.x, a.x), fma(h94, r.y - a.y, a.y), fma(h94, r.z - a.z, a.z));
        }
        acc = vec3(h96 * ap.x, h96 * ap.y, fma(h96, ap.z, g));
    }
    // turbulence
    float hb4 = Th(0xb4u);
    if (hb4 > 0.0) {
        float d0 = vb_f32(vbPA8, v, 4u);
        if (0.0 < d0) {
            int i = vb_i16(vbPA8, v, 0u);
            float hb6 = Th(0xb6u), hc0 = Th(0xc0u);
            vec3 q = cur - A_get(i);
            float t = uintBitsToFloat(0x3e22f983u) * float(i);
            t = sin_rev(t);
            t = 1000.0 * t;
            vec3 cc = cross3(a, q);
            float L = dot3(q, q);
            t = fract_f32(t);
            float ph = hc0 + t;
            float sq = sqrt(L);
            float arg = fma(hb6, sq, ph);
            vec3 n = snorm(cc);
            float sn = sin_rev(arg);
            float kk = sn * hb4;
            acc = vec3(n.x * kk + acc.x, n.y * kk + acc.y, n.z * kk + acc.z);
        }
    }
    // predicted position
    vec3 t = vec3(Tf(0x68u) + cur.x, Tf(0x6cu) + cur.y, Tf(0x70u) + cur.z);
    t = vec3(fma(h74, rel.x, t.x), fma(h74, rel.y, t.y), fma(h74, rel.z, t.z));
    vec3 p = vec3(dtp2 * acc.x + t.x, acc.y * dtp2 + t.y, acc.z * dtp2 + t.z);
    p = skin_constrain(v, p);
    // soft pin toward the anchor
    float hc2 = Th(0xc2u);
    float s = (P4 & 32u) != 0u ? vb_fmt(vbP98, v).x * hc2 : hc2;
    if (0.0 < s) {
        float hc4 = Th(0xc4u);
        if (hc4 > 0.0) {
            float gg = rcp_f32(hc4);
            float d0 = vb_f32(vbPA8, v, 4u);
            float e = gg * (-d0);
            e = exp2(e);
            s = s * e;
        }
        vec3 tt = A_get(int(v)) - p;
        p = vec3(tt.x * s + p.x, tt.y * s + p.y, tt.z * s + p.z);
    }
    return p;
}

// ------------------------------------------------------------------------------------------- distance constraint
VB vbPC8, vbPE8, vbPF8;
float inv_iters;

void distance_constraint(uint j, bool compress, float hdc) {
    int a = vb_i16(vbPD8, j, 0u), b = vb_i16(vbPD8, j, 2u);
    float hk = vb_h(vbPD8, j, 6u), hr = vb_h(vbPD8, j, 4u);
    float wa = vb_fmt(vbP48, uint(a)).x, wb = vb_fmt(vbP48, uint(b)).x;
    float om = 1.0 - hk;
    vec3 xa = X_get(a), xb = X_get(b);
    float lg = log2(abs(om));
    vec3 d = xb - xa;
    lg = lg * inv_iters;
    float dd = dot3(d, d);
    float kk = exp2(lg);
    float L = sqrt(dd);
    L = max(EPS4, L);
    float C = fma(objscale, -hr, L);
    if (compress) {
        float Cc = hdc * C;
        if (0.0 > C) C = Cc;
    }
    float W = wa + wb;
    C = C * (-kk) + C;
    float den = L * W;
    float r = rcp_f32(den);
    float sg = r * C;
    float sa = wa * sg;
    float sb = (-wb) * sg;
    xa = vec3(sa * d.x + xa.x, sa * d.y + xa.y, sa * d.z + xa.z);
    xb = vec3(sb * d.x + xb.x, sb * d.y + xb.y, sb * d.z + xb.z);
    X_set(a, xa);
    X_set(b, xb);
}

// ------------------------------------------------------------------------------------------- bending constraint
// formula A (T[0x108] bit 7) or B; k_once or k_it (every iteration)
void bending_constraint(uint j, bool formulaA, bool every) {
    int i0 = vb_i16(vbPF8, j, 0u), i1 = vb_i16(vbPF8, j, 2u), i2 = vb_i16(vbPF8, j, 4u), i3 = vb_i16(vbPF8, j, 6u);
    float h8 = vb_h(vbPF8, j, 8u), h10 = vb_h(vbPF8, j, 10u), h12 = vb_h(vbPF8, j, 12u);
    float h14 = vb_h(vbPF8, j, 14u), h16 = vb_h(vbPF8, j, 16u);
    float h106 = Th(0x106u);
    vec3 x0 = X_get(i0), x1 = X_get(i1), x2 = X_get(i2), x3 = X_get(i3);
    float om14 = 1.0 - h14;
    float k = every ? 1.0 - exp2(log2(abs(om14)) * inv_iters) : 1.0 - abs(om14);
    float om8 = 1.0 - h8;
    float h10m1 = h10 - 1.0;
    vec3 C;
    for (int c = 0; c < 3; c++) {
        float t = h8 * x0[c];
        t = fma(-h10, x1[c], t);
        t = t + om8 * x2[c];
        C[c] = h10m1 * x3[c] + t;
    }
    vec3 e02 = x2 - x0, e13 = x3 - x1;
    vec3 n = vec3(e02.y * e13.z - e13.y * e02.z, e02.z * e13.x - e13.z * e02.x, e02.x * e13.y - e13.x * e02.y);
    float nn2 = dot3(n, n);
    float r = rsq_f32(nn2);
    vec3 nh = fin(r) ? vec3(r * n.x, r * n.y, r * n.z) : vec3(0.0);
    float off = (-objscale) * fma(-h106, h16, h16);
    C = vec3(C.x + off * nh.x, C.y + off * nh.y, C.z + off * nh.z);
    float c0 = min(2.0, h8 * k), c1 = max(-2.0, (-h10) * k), c2 = min(2.0, k * om8);
    if (formulaA) {
        float w0 = vb_fmt(vbP48, uint(i0)).x, w1 = vb_fmt(vbP48, uint(i1)).x, w2 = vb_fmt(vbP48, uint(i2)).x;
        float s = w1 + h12;
        float f0 = ((-w0) * s) * c0;
        float f1 = (((-w1) * s) * c1) * 2.0;
        float f2 = ((-w2) * s) * c2;
        x0 = vec3(x0.x + C.x * f0, x0.y + C.y * f0, x0.z + C.z * f0);
        x1 = vec3(x1.x + C.x * f1, x1.y + C.y * f1, x1.z + C.z * f1);
        x2 = vec3(x2.x + C.x * f2, x2.y + C.y * f2, x2.z + C.z * f2);
        X_set(i0, x0);
        X_set(i1, x1);
        X_set(i2, x2);
    } else {
        float c3 = max(-2.0, k * h10m1);
        float w0 = vb_fmt(vbP48, uint(i0)).x, w1 = vb_fmt(vbP48, uint(i1)).x, w2 = vb_fmt(vbP48, uint(i2)).x;
        float w3 = vb_fmt(vbP48, uint(i3)).x;
        float f0 = ((-w0) * h12) * c0, f1 = ((-w1) * h12) * c1, f2 = ((-w2) * h12) * c2, f3 = ((-w3) * h12) * c3;
        x0 = vec3(x0.x + C.x * f0, x0.y + C.y * f0, x0.z + C.z * f0);
        x1 = vec3(x1.x + C.x * f1, x1.y + C.y * f1, x1.z + C.z * f1);
        x2 = vec3(x2.x + C.x * f2, x2.y + C.y * f2, x2.z + C.z * f2);
        x3 = vec3(x3.x + C.x * f3, x3.y + C.y * f3, x3.z + C.z * f3);
        X_set(i0, x0);
        X_set(i1, x1);
        X_set(i2, x2);
        X_set(i3, x3);
    }
}

// ------------------------------------------------------------------------------------------- colliders
// collider set s: the qword T.V#(0x18)[s]; its record array: the V# at set + 0x88 (168-byte records)
shared uint64_t sSetPtr[16];        // set pointers (byte offset 8*s, as the full paths read them)
shared uint64_t sRecBase[16];       // record array base (device address) per set, 0 when empty / not on the GPU
shared uint sRecNum[16];            // size in bytes of the set's record V# (num_records, times the stride if any)
shared uint64_t sMask[48];          // candidate masks: qword m = set m/3, records 64*(m%3) + bit
uint nsets;
// a record address is a DEVICE address (the set's record array translated once: one device buffer holds it), 0 when
// the array's page is not on the GPU (yet): its fields then read 0, as the guest's loads through the page table do
float rec_f(uint64_t rec, uint off) { return rec == 0ul ? 0.0 : uintBitsToFloat(W32(rec + uint64_t(off)).v); }
uint rec_u(uint64_t rec, uint off) { return rec == 0ul ? 0u : W32(rec + uint64_t(off)).v; }
uint64_t rec_dev(uint64_t guest) { return guest == 0ul ? 0ul : xlate(guest); }
uint64_t rec_at(uint64_t base, uint k) { return base == 0ul ? 0ul : base + uint64_t(168u * k); }
// a record of set s as the full paths read it (s_buffer_load: 0 beyond the V#'s num_records)
uint64_t rec_full(uint s, uint k) { return 168u * k + 168u <= sRecNum[s] ? rec_at(sRecBase[s], k) : 0ul; }
vec3 rec_f3(uint64_t rec, uint off) { return vec3(rec_f(rec, off), rec_f(rec, off + 4u), rec_f(rec, off + 8u)); }
vec4 rec_f4(uint64_t rec, uint off) { return vec4(rec_f(rec, off), rec_f(rec, off + 4u), rec_f(rec, off + 8u), rec_f(rec, off + 12u)); }

// edge (xa, xb) of thickness r against one record: 1 sphere, 2 tapered capsule (others: nothing). true on a hit
bool edge_vs_collider(uint64_t rec, uint type, float r, inout vec3 xa, inout vec3 xb) {
    vec3 emin = min(xa, xb), emax = max(xa, xb);
    if (type == 2u) {
        vec3 c0 = rec_f3(rec, 0x10u), c1 = rec_f3(rec, 0x20u);
        float R0 = rec_f(rec, 0x1cu) + r, R1 = rec_f(rec, 0x2cu) + r;
        vec3 cmax = vec3(max(c0.x + R0, c1.x + R1), max(c0.y + R0, c1.y + R1), max(c0.z + R0, c1.z + R1));
        vec3 cmin = vec3(min(c0.x - R0, c1.x - R1), min(c0.y - R0, c1.y - R1), min(c0.z - R0, c1.z - R1));
        if (emin.z > cmax.z || emin.y > cmax.y || emin.x > cmax.x || emax.z < cmin.z || emax.y < cmin.y || emax.x < cmin.x)
            return false;
        vec3 d1 = c1 - c0, d2 = xb - xa;
        vec3 n = cross3(d1, d2);
        float nn = dot3(n, n);
        if (nn < EPS4) return false;
        vec3 w = xa - c0;
        float inv = rcp_f32(nn);
        float t = inv * dot3(n, cross3(w, d1));
        float s = inv * dot3(n, cross3(w, d2));
        float tq;
        if (0.0 > s) tq = clamp01(rcp_f32(dot3(d2, d2)) * dot3(d2, c0 - xa));
        else if (1.0 < s) tq = clamp01(rcp_f32(dot3(d2, d2)) * dot3(d2, c1 - xa));
        else tq = clamp01(max(0.0, t));
        vec3 q = vec3(d2.x * tq + xa.x, d2.y * tq + xa.y, d2.z * tq + xa.z);
        float dR = R1 - R0;
        float den = dot3(d1, d1) + dR * (-dR);
        float num = dot3(d1, q - c0) + dR * R0;
        float u = clamp01(rcp_f32(den) * num);
        vec3 p = vec3(d1.x * u + c0.x, d1.y * u + c0.y, d1.z * u + c0.z);
        float rr = R0 + u * dR;
        vec3 dv = q - p;
        float dd = dot3(dv, dv);
        if (!(dd >= rr * rr)) {
            float ir = rsq_f32(dd);
            vec3 nv = fin(ir) ? vec3(dv.x * ir, dv.y * ir, dv.z * ir) : vec3(0.0);
            vec3 corr = vec3(nv.x * rr - dv.x, nv.y * rr - dv.y, nv.z * rr - dv.z);
            xa = xa + corr;
            xb = xb + corr;
            return true;
        }
        return false;
    }
    if (type == 1u) {
        vec3 c = rec_f3(rec, 0x10u);
        float R = rec_f(rec, 0x1cu) + r;
        if (emin.z > c.z + R || emin.y > c.y + R || emin.x > c.x + R || emax.z < c.z - R || emax.y < c.y - R || emax.x < c.x - R)
            return false;
        vec3 e = xb - xa;
        float t = clamp01(rcp_f32(dot3(e, e)) * dot3(e, c - xa));
        vec3 d = vec3((xa.x - c.x) + t * e.x, (xa.y - c.y) + t * e.y, (xa.z - c.z) + t * e.z);
        float dd = dot3(d, d);
        if (!(dd >= R * R)) {
            float ir = rsq_f32(dd);
            vec3 nv = fin(ir) ? vec3(d.x * ir, d.y * ir, d.z * ir) : vec3(0.0);
            vec3 corr = vec3(nv.x * R - d.x, nv.y * R - d.y, nv.z * R - d.z);
            xa = xa + corr;
            xb = xb + corr;
            return true;
        }
    }
    return false;
}

// G0: the collision cache (V# at *(T+0)), {i16 set, i16 k} per vertex (T[0xb8] + v) and per edge (P[0] + T[0xb8] + e)
VB vbG0;
float edge_thickness(int a, int b) {
    float r = Th(0xf6u);
    if ((P4 & 16u) != 0u) {
        float fa = vb_fmt(vbP88, uint(a)).x, fb = vb_fmt(vbP88, uint(b)).x;
        r = (r * (fa + fb)) * 0.5;
    }
    return r;
}

// full edge pass for edge e (group record index), as the guest: sets s < nsets, mask qwords 2s + j (j < 2), bare
// bit index as the record index
void edge_full(uint e, bool F4) {
    int a = vb_i16(vbPD8, e, 0u), b = vb_i16(vbPD8, e, 2u);
    float r = edge_thickness(a, b);
    vec3 xa = X_get(a), xb = X_get(b);
    uint hit = 0xffffffffu;
    for (uint s = 0u; s < nsets; s++) {
        for (uint j = 0u; j < 2u; j++) {
            uint64_t mask = sMask[2u * s + j];
            while (mask != 0ul) {
                uint k = uint(findLSB(mask));
                uint64_t rec = rec_full(s, k);
                uint ctype = rec_u(rec, 0u);
                if (edge_vs_collider(rec, ctype, r, xa, xb)) hit = (s << 16) | (k & 0xffffu);
                mask &= ~(1ul << k);
            }
        }
    }
    if (F4) {
        uint idx = uint(nverts) + Tu(0xb8u) + e;
        vb_store16(vbG0, idx, 0u, hit >> 16);
        vb_store16(vbG0, idx, 2u, hit & 0xffffu);
    }
    X_set(a, xa);
    X_set(b, xb);
}

// cached edge pass: only the collider remembered by the last full pass (no LDS masks, no flags)
void edge_cached(uint e) {
    uint idx = uint(nverts) + Tu(0xb8u) + e;
    int cs = int(int16_t(uint16_t(vb_u16(vbG0, idx, 0u)))), ck = int(int16_t(uint16_t(vb_u16(vbG0, idx, 2u))));
    if (cs < 0) return;
    int a = vb_i16(vbPD8, e, 0u), b = vb_i16(vbPD8, e, 2u);
    uint64_t setp = packUint2x32(uvec2(vb_u32(vbT18, uint(cs), 0u), vb_u32(vbT18, uint(cs), 4u)));
    uint64_t vd = gld64(setp + 0x88ul);
    uint64_t base = (vd & 0xfffffffful) | (uint64_t(int64_t(int16_t(uint16_t((vd >> 32) & 0xfffful)))) << 32);
    uint64_t rec = rec_at(rec_dev(base), uint(ck));
    vec3 xa = X_get(a), xb = X_get(b);
    uint ctype = rec_u(rec, 0u);
    if (ctype != 0u) {
        float r = edge_thickness(a, b);
        if (ctype == 2u || ctype == 1u) edge_vs_collider(rec, ctype, r, xa, xb);
    }
    X_set(a, xa);
    X_set(b, xb);
}

// ------------------------------------------------------------------------------------------- vertex collisions
#define EPS4N uintBitsToFloat(0xb8d1b717u)    // -1e-4
vec3 cur_of(uint v) { vec3 c = vb_f3(vbT28, v, 0u); return vec3(Tf(0x48u) + c.x, Tf(0x4cu) + c.y, Tf(0x50u) + c.z); }
// Mv(o) - p - base for the sphere / capsule exit direction: rows at 0x30, 0x40, 0x50 (translation in w)
vec3 motion_dir(uint64_t rec, vec3 start, vec3 o) {
    vec4 m0 = rec_f4(rec, 0x30u), m1 = rec_f4(rec, 0x40u), m2 = rec_f4(rec, 0x50u);
    float x = start.x + m0.x * o.x; x = x + m0.y * o.y; x = x + m0.z * o.z;
    float y = start.y + m1.x * o.x; y = y + m1.y * o.y; y = y + m1.z * o.z;
    float z = start.z + m2.x * o.x; z = z + m2.y * o.y; z = z + m2.z * o.z;
    return vec3(x, y, z);
}

// full pass, one collider; returns true on a hit (the anchor-side one-sided pushes do not count)
bool vertex_vs_collider(uint v, uint64_t rec, uint type, bool oneSided, bool collide, float f, float thick, vec3 o, inout vec3 p) {
    if (type == 4u) {
        vec3 n = rec_f3(rec, 0x10u), P0 = vec3(rec_f(rec, 0x1cu), rec_f(rec, 0x20u), rec_f(rec, 0x24u));
        vec3 Pt = vec3(thick * n.x + P0.x, thick * n.y + P0.y, thick * n.z + P0.z);
        vec3 q = p - Pt;
        float dist = n.x * q.x - thick; dist = dist + n.y * q.y; dist = dist + n.z * q.z;
        if (!(dist > EPS4N)) {
            vec3 qp = vec3(n.x * (-dist) + q.x, n.y * (-dist) + q.y, n.z * (-dist) + q.z);
            vec3 cr = vb_f3(vbT28, v, 0u);
            vec3 wo = vec3(((Tf(0x48u) + cr.x) - Pt.x) - q.x, (Tf(0x4cu) + (cr.y - Pt.y)) - q.y, (Tf(0x50u) + (cr.z - Pt.z)) - q.z);
            float dn = dot3(n, wo);
            if (EPS4 < dn) {
                vec3 g = q - qp;
                float s = clamp01((-dist) * rcp_f32(dn));
                g = vec3(s * wo.x + g.x, s * wo.y + g.y, s * wo.z + g.z);
                qp = vec3(f * g.x + qp.x, f * g.y + qp.y, f * g.z + qp.z);
            }
            p = qp + Pt;
            return true;
        }
        return false;
    }
    vec3 A = A_get(int(v));
    if (type == 2u) {
        vec3 c0 = rec_f3(rec, 0x10u), c1 = rec_f3(rec, 0x20u);
        float R0 = rec_f(rec, 0x1cu) + thick, R1 = rec_f(rec, 0x2cu) + thick;
        float sc = oneSided ? 1.5 : 1.0;
        vec3 hi = vec3(max(sc * R0 + c0.x, sc * R1 + c1.x), max(sc * R0 + c0.y, sc * R1 + c1.y), max(sc * R0 + c0.z, sc * R1 + c1.z));
        vec3 lo = vec3(min((-sc) * R0 + c0.x, (-sc) * R1 + c1.x), min((-sc) * R0 + c0.y, (-sc) * R1 + c1.y),
                       min((-sc) * R0 + c0.z, (-sc) * R1 + c1.z));
        if (p.x > hi.x || p.y > hi.y || p.z > hi.z || p.x < lo.x || p.y < lo.y || p.z < lo.z) return false;
        if (oneSided) {
            vec3 ax = c1 - c0, m = c0 + c1;
            float L2 = dot3(ax, ax);
            vec3 am = vec3((-0.5) * m.x + A.x, (-0.5) * m.y + A.y, (-0.5) * m.z + A.z);
            float Lb = (sqrt(L2) + R0) + R1;
            if (!(dot3(am, am) > Lb * Lb)) {
                vec3 a0 = A - c0;
                float dr = R1 - R0;
                float w = dot3(ax, a0) + dr * R0;
                float D = (-dr) * dr + L2;
                float t = clamp01(rcp_f32(D) * w);
                vec3 Q = vec3(ax.x * t + c0.x, ax.y * t + c0.y, ax.z * t + c0.z);
                float Rt = dr * t + R0;
                vec3 nA = nrm(A - Q);
                float lim = uintBitsToFloat(0x3f19999au) * Rt;
                float dist = dot3(nA, p - Q);
                if (dist < lim) { float k = lim - dist; p = vec3(k * nA.x + p.x, k * nA.y + p.y, k * nA.z + p.z); }
            }
        }
        if (!collide) return false;
        vec3 ax = c1 - c0, dp = p - c0;
        float L2 = dot3(ax, ax), w0 = dot3(ax, dp), dr = R1 - R0, R0dr = R0 * dr;
        float D = L2 + dr * (-dr);
        float w = w0 + dr * R0;
        float iD = rcp_f32(D);
        float t = clamp01(iD * w);
        vec3 d = vec3((-t) * ax.x + dp.x, (-t) * ax.y + dp.y, (-t) * ax.z + dp.z);
        float Rt = dr * t + R0;
        float dd = dot3(d, d);
        if (dd > Rt * Rt) return false;
        vec3 n = snorm(d);
        vec3 e = snorm(motion_dir(rec, vec3((rec_f(rec, 0x3cu) - c0.x) - dp.x, (rec_f(rec, 0x4cu) - c0.y) - dp.y,
                                            (rec_f(rec, 0x5cu) - c0.z) - dp.z), o));
        float ne = dot3(n, e);
        if (ne < 0.0) e = vec3(n.x * (-ne) + e.x, n.y * (-ne) + e.y, n.z * (-ne) + e.z);
        vec3 dir = vec3(f * (e.x - n.x) + n.x, f * (e.y - n.y) + n.y, f * (e.z - n.z) + n.z);
        vec3 H = dp;
        float pc2 = dot3(dp, dp) + R0 * (-R0);
        float b = dot3(dp, dir), axd = dot3(ax, dir), a = dot3(dir, dir);
        float C = D * pc2 + w * (-w);
        float B = D * b + axd * (-w);
        float AA = D * a + axd * (-axd);
        float disc = B * B + C * (-AA);
        if (!(disc < 0.0) && !(EPS6 > AA)) {
            float tr = rcp_f32(AA) * (sqrt(disc) - B);
            bool cap = false;
            vec3 capOff = vec3(0.0);
            float Rc = R0;
            if (0.0 < tr) {
                H = vec3(tr * dir.x + dp.x, tr * dir.y + dp.y, tr * dir.z + dp.z);
                float sn = R0dr + H.x * ax.x; sn = sn + H.y * ax.y; sn = sn + H.z * ax.z; sn = iD * sn;
                if (sn < 0.0) { cap = true; capOff = vec3(0.0); Rc = R0; }
                else if (sn > 1.0) { cap = true; capOff = ax; Rc = R1; }
            } else {
                cap = true;
                capOff = (R0 > R1) ? vec3(0.0) : ax;
                Rc = max(R0, R1);
            }
            if (cap) {
                H = dp;
                vec3 pc = dp - capOff;
                float bc = dot3(dir, pc);
                float cc = dot3(pc, pc) + (-Rc) * Rc;
                float dc = bc * bc + cc * (-a);
                if (!(dc < 0.0)) {
                    float tc = rcp_f32(max(TINY, a)) * (sqrt(dc) - bc);
                    H = vec3(dir.x * tc + dp.x, dir.y * tc + dp.y, dir.z * tc + dp.z);
                }
            }
        }
        p = c0 + H;
        return true;
    }
    if (type == 1u) {
        vec3 c = rec_f3(rec, 0x10u);
        float R = rec_f(rec, 0x1cu) + thick;
        if (oneSided) {
            vec3 pc = p - c;
            float lim = uintBitsToFloat(0x3f19999au) * R;
            vec3 nA = nrm(A - c);
            float dist = dot3(nA, pc);
            if (dist < lim) { float k = lim - dist; p = vec3(k * nA.x + p.x, k * nA.y + p.y, k * nA.z + p.z); }
        }
        if (!collide) return false;
        vec3 d = p - c;
        float RR = R * R, dd = dot3(d, d);
        if (dd > RR) return false;
        vec3 n = snorm(d);
        vec3 e = snorm(motion_dir(rec, vec3(rec_f(rec, 0x3cu) - p.x, rec_f(rec, 0x4cu) - p.y, rec_f(rec, 0x5cu) - p.z), o));
        float ne = dot3(n, e);
        if (ne < 0.0) e = vec3(n.x * (-ne) + e.x, n.y * (-ne) + e.y, n.z * (-ne) + e.z);
        vec3 dir = vec3(f * (e.x - n.x) + n.x, f * (e.y - n.y) + n.y, f * (e.z - n.z) + n.z);
        float cc = dd - RR;
        float b = dot3(d, dir), a = dot3(dir, dir);
        float disc = b * b + cc * (-a);
        if (!(disc < 0.0)) {
            float t = rcp_f32(max(TINY, a)) * (sqrt(disc) - b);
            p = vec3(t * dir.x + p.x, t * dir.y + p.y, t * dir.z + p.z);
        }
        return true;
    }
    if (type == 0u) {
        vec4 L0 = rec_f4(rec, 0x10u), L1 = rec_f4(rec, 0x20u), L2 = rec_f4(rec, 0x30u);
        vec4 W0 = rec_f4(rec, 0x40u), W1 = rec_f4(rec, 0x50u), W2 = rec_f4(rec, 0x60u);
        if (oneSided) {
            vec3 nl = snorm(vec3(row_xf(W0, A), row_xf(W1, A), row_xf(W2, A)));
            vec3 qv = vec3(uintBitsToFloat(0x3e99999au) * nl.x, uintBitsToFloat(0x3e99999au) * nl.y, uintBitsToFloat(0x3e99999au) * nl.z);
            vec3 S = vec3(row_xf(L0, qv), row_xf(L1, qv), row_xf(L2, qv));
            vec3 nA = nrm(A - S);
            float dist = dot3(nA, p - S);
            if (dist < 0.0) p = vec3(nA.x * (-dist) + p.x, nA.y * (-dist) + p.y, nA.z * (-dist) + p.z);
        }
        if (!collide) return false;
        vec3 pl = vec3(row_xf(W0, p), row_xf(W1, p), row_xf(W2, p));
        float pp = dot3(pl, pl);
        if (pp > 1.0) return false;
        vec3 nl = snorm(pl);
        vec4 V0 = rec_f4(rec, 0x70u), V1 = rec_f4(rec, 0x80u), V2 = rec_f4(rec, 0x90u);
        float ux = V0.w - pl.x; ux = ux + V0.x * o.x; ux = ux + V0.y * o.y; ux = ux + V0.z * o.z;
        float uy = V1.w - pl.y; uy = uy + V1.x * o.x; uy = uy + V1.y * o.y; uy = uy + V1.z * o.z;
        float uz = V2.w - pl.z; uz = uz + V2.x * o.x; uz = uz + V2.y * o.y; uz = uz + V2.z * o.z;
        vec3 ul = vec3(ux, uy, uz);
        float ru = rsq_f32(dot3(ul, ul));
        vec3 g = fin(ru) ? vec3(ru * ul.x - nl.x, ru * ul.y - nl.y, ru * ul.z - nl.z) : -nl;
        vec3 dir = vec3(f * g.x + nl.x, f * g.y + nl.y, f * g.z + nl.z);
        float b = dot3(pl, dir), a = dot3(dir, dir);
        float k = b * b + pp * (-a);
        if (!(k < -a)) {
            float t = rcp_f32(max(TINY, a)) * (sqrt(k + a) - b);
            pl = vec3(t * dir.x + pl.x, t * dir.y + pl.y, t * dir.z + pl.z);
        }
        p = vec3(row_xf(L0, pl), row_xf(L1, pl), row_xf(L2, pl));
        return true;
    }
    return false;
}

vec3 vertex_full(uint v, vec3 p, bool F4) {
    vec3 o = cur_of(v);
    float thick = Th(0xf6u);
    if ((P4 & 16u) != 0u) thick = vb_fmt(vbP88, v).x * thick;
    uint hitc = 0xffffffffu;
    uint T108 = Tu(0x108u);
    for (uint s = 0u; s < nsets; s++) {
        uint collideBit = s == 0u ? 1u : 2u;
        uint oneSideBits = (T108 & 16u) != 0u ? 12u : (s == 0u ? 4u : 8u);
        for (uint w = 0u; w < 3u; w++) {
            uint64_t mask = sMask[3u * s + w];
            while (mask != 0ul) {
                uint k = uint(findLSB(mask));
                uint64_t rec = rec_full(s, k);
                uint type = rec_u(rec, 0u), flags = rec_u(rec, 4u);
                bool oneSided = (flags & oneSideBits) != 0u && (T108 & 64u) == 0u;
                bool collide = (flags & collideBit) != 0u;
                float f = rec_f(rec, 0xcu);
                if (vertex_vs_collider(v, rec, type, oneSided, collide, f, thick, o, p)) hitc = (s << 16) | (k & 0xffffu);
                mask &= ~(1ul << k);
            }
        }
    }
    // ground plane of T
    vec3 n = vec3(Tf(0xe8u), Tf(0xecu), Tf(0xf0u)), P0 = vec3(Tf(0xf8u), Tf(0xfcu), Tf(0x100u));
    vec3 q = p - P0;
    float dist = n.x * q.x - thick; dist = dist + n.y * q.y; dist = dist + n.z * q.z;
    if (!(dist > EPS4N)) {
        vec3 qp = vec3(n.x * (-dist) + q.x, n.y * (-dist) + q.y, n.z * (-dist) + q.z);
        vec3 cr = vb_f3(vbT28, v, 0u);
        vec3 wo = vec3(((Tf(0x48u) + cr.x) - P0.x) - q.x, (Tf(0x4cu) + (cr.y - P0.y)) - q.y, (Tf(0x50u) + (cr.z - P0.z)) - q.z);
        float dn = dot3(n, wo);
        if (EPS4 < dn) {
            vec3 g = q - qp;
            float s = clamp01((-dist) * rcp_f32(dn));
            g = vec3(s * wo.x + g.x, s * wo.y + g.y, s * wo.z + g.z);
            float fT = Th(0xf4u);
            qp = vec3(fma(fT, g.x, qp.x), fma(fT, g.y, qp.y), fma(fT, g.z, qp.z));
        }
        p = P0 + qp;
    }
    if (F4) {
        uint idx = Tu(0xb8u) + v;
        vb_store16(vbG0, idx, 0u, hitc >> 16);
        vb_store16(vbG0, idx, 2u, hitc & 0xffffu);
    }
    return p;
}

// cached pass: only the collider the last full pass remembered for this vertex (no flags, no one-sided tests)
vec3 vertex_cached(uint v, vec3 p) {
    uint gi = Tu(0xb8u) + v;
    int set = int(int16_t(uint16_t(vb_u16(vbG0, gi, 0u)))), kk = int(int16_t(uint16_t(vb_u16(vbG0, gi, 2u))));
    if (set < 0) return p;
    uint64_t sp = packUint2x32(uvec2(vb_u32(vbT18, uint(set), 0u), vb_u32(vbT18, uint(set), 4u)));
    uint64_t vd = gld64(sp + 136ul);
    uint64_t base = (vd & 0xfffffffful) | (uint64_t(int64_t(int16_t(uint16_t((vd >> 32) & 0xfffful)))) << 32);
    uint64_t rec = rec_at(rec_dev(base), uint(kk));
    uint type = rec_u(rec, 0u);
    float fr = rec_f(rec, 0xcu);
    if (type != 0u) {
        float r = Th(0xf6u);
        if ((P4 & 16u) != 0u) r = vb_fmt(vbP88, v).x * r;
        if (type == 4u) {
            vec3 n = rec_f3(rec, 0x10u), c = vec3(rec_f(rec, 0x1cu), rec_f(rec, 0x20u), rec_f(rec, 0x24u));
            float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
            float d = n.x * dx - r; d = dy * n.y + d; d = dz * n.z + d;
            if (!(d > EPS4N)) {
                float ax = n.x * (-d) + dx, ay = n.y * (-d) + dy, az = n.z * (-d) + dz;
                vec3 X = vb_f3(vbT28, v, 0u);
                float wx = ((Tf(0x48u) + X.x) - c.x) - dx;
                float wy = ((X.y - c.y) + Tf(0x4cu)) - dy;
                float wz = ((X.z - c.z) + Tf(0x50u)) - dz;
                float dn = n.x * wx; dn = wy * n.y + dn; dn = wz * n.z + dn;
                if (EPS4 < dn) {
                    float t = clamp01((-d) * rcp_f32(dn));
                    float gx = dx - ax, gy = dy - ay, gz = dz - az;
                    gx = t * wx + gx; gy = t * wy + gy; gz = t * wz + gz;
                    ax = gx * fr + ax; ay = gy * fr + ay; az = gz * fr + az;
                }
                p = vec3(c.x + ax, c.y + ay, c.z + az);
            }
        } else if (type == 2u) {
            vec3 c0 = rec_f3(rec, 0x10u), c1 = rec_f3(rec, 0x20u);
            float r0 = rec_f(rec, 0x1cu), r1 = rec_f(rec, 0x2cu);
            float R1 = r + r1, R0 = r + r0;
            float hz = max(c0.z + R0, c1.z + R1), lz = min(c0.z - R0, c1.z - R1);
            float hy = max(c0.y + R0, c1.y + R1), ly = min(c0.y - R0, c1.y - R1);
            float hx = max(c0.x + R0, c1.x + R1), lx = min(c0.x - R0, c1.x - R1);
            if (p.z > hz || p.y > hy || p.z < lz || p.x > hx || p.y < ly || p.x < lx) return p;
            vec3 e = c1 - c0, q = p - c0;
            float dR = R1 - R0;
            float L2 = dot3(e, e); L2 = dR * (-dR) + L2;
            float Rd = R0 * dR;
            float iL = rcp_f32(L2);
            float B0 = dot3(e, q); B0 = dR * R0 + B0;
            float t = clamp01(iL * B0);
            float Rt = dR * t + R0;
            vec3 dv = vec3((-t) * e.x + q.x, (-t) * e.y + q.y, (-t) * e.z + q.z);
            if (dot3(dv, dv) > Rt * Rt) return p;
            vec3 n = snorm(dv);
            vec3 o = cur_of(v);
            vec3 m = snorm(motion_dir(rec, vec3((rec_f(rec, 0x3cu) - c0.x) - q.x, (rec_f(rec, 0x4cu) - c0.y) - q.y,
                                                (rec_f(rec, 0x5cu) - c0.z) - q.z), o));
            float dm = dot3(n, m);
            if (0.0 > dm) m = vec3(n.x * (-dm) + m.x, n.y * (-dm) + m.y, n.z * (-dm) + m.z);
            vec3 dir = vec3((m.x - n.x) * fr + n.x, (m.y - n.y) * fr + n.y, (m.z - n.z) * fr + n.z);
            float qq = dot3(q, q), qd = dot3(q, dir), ed = dot3(e, dir), dd = dot3(dir, dir);
            qq = R0 * (-R0) + qq;
            float Cc = L2 * qq, Bb = L2 * qd, Aa = L2 * dd;
            Cc = B0 * (-B0) + Cc; Bb = ed * (-B0) + Bb; Aa = ed * (-ed) + Aa;
            float disc = Bb * Bb; disc = Cc * (-Aa) + disc;
            vec3 outp = q;
            if (!(disc < 0.0) && !(Aa < EPS6)) {
                float s = rcp_f32(Aa) * (sqrt(disc) - Bb);
                bool cap;
                vec3 off = vec3(0.0);
                float Rc = R0;
                if (0.0 < s) {
                    vec3 h = vec3(s * dir.x + q.x, s * dir.y + q.y, s * dir.z + q.z);
                    float th = Rd; th = h.x * e.x + th; th = h.y * e.y + th; th = h.z * e.z + th; th = iL * th;
                    if (0.0 > th) { cap = true; off = vec3(0.0); Rc = R0; }
                    else if (1.0 < th) { cap = true; off = e; Rc = R1; }
                    else { cap = false; outp = h; }
                } else {
                    cap = true;
                    Rc = max(R0, R1);
                    off = (R0 > R1) ? vec3(0.0) : e;
                }
                if (cap) {
                    vec3 qc = q - off;
                    float b = dot3(dir, qc);
                    float cc = dot3(qc, qc); cc = (-Rc) * Rc + cc;
                    float d2 = b * b; d2 = cc * (-dd) + d2;
                    if (!(d2 < 0.0)) {
                        float s2 = rcp_f32(max(TINY, dd)) * (sqrt(d2) - b);
                        outp = vec3(dir.x * s2 + q.x, dir.y * s2 + q.y, dir.z * s2 + q.z);
                    }
                }
            }
            p = vec3(c0.x + outp.x, c0.y + outp.y, c0.z + outp.z);
        } else if (type == 1u) {
            vec3 c = rec_f3(rec, 0x10u);
            float rad = rec_f(rec, 0x1cu);
            vec3 d = p - c;
            float Rs = r + rad, Rs2 = Rs * Rs;
            float dd2 = dot3(d, d);
            if (dd2 > Rs2) return p;
            vec3 o = cur_of(v);
            vec3 n = snorm(d);
            vec3 m = snorm(motion_dir(rec, vec3(rec_f(rec, 0x3cu) - p.x, rec_f(rec, 0x4cu) - p.y, rec_f(rec, 0x5cu) - p.z), o));
            float dm = dot3(n, m);
            if (0.0 > dm) m = vec3(n.x * (-dm) + m.x, n.y * (-dm) + m.y, n.z * (-dm) + m.z);
            float cc = dd2 - Rs2;
            vec3 dir = vec3((m.x - n.x) * fr + n.x, (m.y - n.y) * fr + n.y, (m.z - n.z) * fr + n.z);
            float b = dot3(d, dir), dd = dot3(dir, dir);
            float disc = b * b; disc = cc * (-dd) + disc;
            if (!(disc < 0.0)) {
                float s = rcp_f32(max(TINY, dd)) * (sqrt(disc) - b);
                p = vec3(s * dir.x + p.x, s * dir.y + p.y, s * dir.z + p.z);
            }
        }
        return p;
    }
    // type 0: ellipsoid
    vec4 L0 = rec_f4(rec, 0x10u), L1 = rec_f4(rec, 0x20u), L2 = rec_f4(rec, 0x30u);
    vec4 W0 = rec_f4(rec, 0x40u), W1 = rec_f4(rec, 0x50u), W2 = rec_f4(rec, 0x60u);
    vec3 l = vec3(row_xf(W0, p), row_xf(W1, p), row_xf(W2, p));
    float ll = dot3(l, l);
    if (ll > 1.0) return p;
    vec3 n = snorm(l);
    vec3 o = cur_of(v);
    vec4 V0 = rec_f4(rec, 0x70u), V1 = rec_f4(rec, 0x80u), V2 = rec_f4(rec, 0x90u);
    float mA = V0.w - l.x; mA = o.x * V0.x + mA; mA = o.y * V0.y + mA; mA = o.z * V0.z + mA;
    float mB = V1.w - l.y; mB = o.x * V1.x + mB; mB = o.y * V1.y + mB; mB = o.z * V1.z + mB;
    float mC = V2.w - l.z; mC = o.x * V2.x + mC; mC = o.y * V2.y + mC; mC = o.z * V2.z + mC;
    vec3 mv = vec3(mA, mB, mC);
    float rm = rsq_f32(dot3(mv, mv));
    vec3 g = fin(rm) ? vec3(rm * mA - n.x, rm * mB - n.y, rm * mC - n.z) : -n;
    vec3 dir = vec3(fr * g.x + n.x, fr * g.y + n.y, fr * g.z + n.z);
    float b = dot3(l, dir), dd = dot3(dir, dir);
    float e = b * b; e = ll * (-dd) + e;
    float disc = e + dd;
    if (!(e < -dd)) {
        float s = rcp_f32(max(TINY, dd)) * (sqrt(disc) - b);
        l = vec3(s * dir.x + l.x, s * dir.y + l.y, s * dir.z + l.z);
    }
    return vec3(row_xf(L0, l), row_xf(L1, l), row_xf(L2, l));
}

// ------------------------------------------------------------------------------------------- cloth bones (end passes)
// one 3x4 matrix per vertex into the palette (C.V#(0x38) or C.V#(0x78) at T[0xe0] + v): it maps the bind pose of the
// vertex onto its position in Q (+ sh)
void cloth_bone(uint v, VB Q, vec3 sh, VB Out) {
    vec3 bind = vb_fmt(vbP18, v).xyz;
    uint flag = vb_u16(vbP108, v, 0u);
    int n2 = vb_i16(vbP108, v, 2u), n4 = vb_i16(vbP108, v, 4u);
    vec4 o0, o1, o2;
    if (flag != 0u) {
        // M: blended skin matrix (stage-2 groups) or the object matrix
        vec4 M0 = vec4(0.0), M1 = vec4(0.0), M2 = vec4(0.0);
        if ((P4 & 1u) != 0u) {
            int G = int(Pu(8u));
            uint base = v * uint(G);
            for (int g = int(Pu(0xcu)); g < G; g++) {
                uint j = base + uint(g);
                ivec4 bi = remap_bones(floatBitsToInt(vb_fmt(vbP28, j)));
                vec4 w = vb_fmt(vbP38, j);
                vec4 B[4][3];
                for (int k = 0; k < 4; k++)
                    for (int r = 0; r < 3; r++) B[k][r] = vb_f4(vbPal, uint(bi[k]), uint(16 * r));
                for (int c = 0; c < 4; c++) {
                    M0[c] = B[0][0][c] * w.x + M0[c]; M0[c] = w.y * B[1][0][c] + M0[c];
                    M0[c] = w.z * B[2][0][c] + M0[c]; M0[c] = w.w * B[3][0][c] + M0[c];
                    M1[c] = B[0][1][c] * w.x + M1[c]; M1[c] = w.y * B[1][1][c] + M1[c];
                    M1[c] = w.z * B[2][1][c] + M1[c]; M1[c] = w.w * B[3][1][c] + M1[c];
                    M2[c] = B[0][2][c] * w.x + M2[c]; M2[c] = w.y * B[1][2][c] + M2[c];
                    M2[c] = w.z * B[2][2][c] + M2[c]; M2[c] = w.w * B[3][2][c] + M2[c];
                }
            }
        } else {
            M0 = vec4(gldf(gObj + 4ul), gldf(gObj + 8ul), gldf(gObj + 12ul), gldf(gObj + 16ul));
            M1 = vec4(gldf(gObj + 20ul), gldf(gObj + 24ul), gldf(gObj + 28ul), gldf(gObj + 32ul));
            M2 = vec4(gldf(gObj + 36ul), gldf(gObj + 40ul), gldf(gObj + 44ul), gldf(gObj + 48ul));
        }
        vec3 Qa = vb_f3(Q, uint(n4), 0u), Qb = vb_f3(Q, uint(n2), 0u), Qv = vb_f3(Q, v, 0u);
        vec3 Sa = vb_fmt(vbP18, uint(n4)).xyz, Sb = vb_fmt(vbP18, uint(n2)).xyz;
        vec3 p = vec3(row_xf(M0, bind), row_xf(M1, bind), row_xf(M2, bind));
        vec3 d = Sa - Sb;
        vec3 e = vec3(dot3(M0.xyz, d), dot3(M1.xyz, d), dot3(M2.xyz, d));
        vec3 a = snorm(e);
        vec3 q = Qa - Qb;
        vec3 c = snorm(q);
        vec3 cur = vec3(sh.x + Qv.x, sh.y + Qv.y, sh.z + Qv.z);
        vec3 k = cross3(a, c);
        float kk = dot3(k, k);
        mat3 R;                     // R[i] = row i
        vec3 T;
        if (EPS4 > kk) {
            R = mat3(1.0);
            T = cur - p;
        } else {
            vec3 kh = snorm(k);
            vec3 ba = cross3(a, kh), bc = cross3(c, kh);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) R[i][j] = (c[i] * a[j] + kh[i] * kh[j]) + bc[i] * ba[j];
            float ap = dot3(a, p), kp = dot3(kh, p), bp = dot3(ba, p);
            for (int i = 0; i < 3; i++) T[i] = cur[i] - ((c[i] * ap + kh[i] * kp) + bc[i] * bp);
        }
        vec4 out_[3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) out_[i][j] = (R[i][0] * M0[j] + R[i][1] * M1[j]) + R[i][2] * M2[j];
            out_[i][3] = ((T[i] + R[i][0] * M0.w) + R[i][1] * M1.w) + R[i][2] * M2.w;
        }
        o0 = out_[0]; o1 = out_[1]; o2 = out_[2];
    } else {
        int n8 = vb_i16(vbP108, v, 8u), n6 = vb_i16(vbP108, v, 6u);
        vec3 e1 = vb_f3(Q, uint(n4), 0u) - vb_f3(Q, uint(n2), 0u);
        vec3 e2 = vb_f3(Q, uint(n8), 0u) - vb_f3(Q, uint(n6), 0u);
        vec3 Qv = vb_f3(Q, v, 0u);
        vec3 cur = vec3(sh.x + Qv.x, sh.y + Qv.y, sh.z + Qv.z);
        vec3 nh = nrm(cross3(e1, e2));
        vec3 eh = nrm(e1);
        vec3 bh = nrm(cross3(nh, eh));
        if (finite3(bh)) {
            vec3 f1 = vb_fmt(vbP18, uint(n4)).xyz - vb_fmt(vbP18, uint(n2)).xyz;
            vec3 f2 = vb_fmt(vbP18, uint(n8)).xyz - vb_fmt(vbP18, uint(n6)).xyz;
            vec3 mh = nrm(cross3(f1, f2));
            vec3 fh = nrm(f1);
            vec3 gh = nrm(cross3(mh, fh));
            float fb = dot3(fh, bind), gb = dot3(gh, bind), mb = dot3(mh, bind);
            vec4 out_[3];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) out_[i][j] = (eh[i] * fh[j] + bh[i] * gh[j]) + nh[i] * mh[j];
                out_[i][3] = cur[i] - ((eh[i] * fb + bh[i] * gb) + nh[i] * mb);
            }
            o0 = out_[0]; o1 = out_[1]; o2 = out_[2];
        } else {
            o0 = vec4(1.0, 0.0, 0.0, cur.x - bind.x);
            o1 = vec4(0.0, 1.0, 0.0, cur.y - bind.y);
            o2 = vec4(0.0, 0.0, 1.0, cur.z - bind.z);
        }
    }
    uint i = Tu(0xe0u) + v;
    if (vb_ok(Out, i, 0u)) {
        uint64_t a = vb_at(Out, i, 0u);
        for (int c = 0; c < 4; c++) {
            W32(a + uint64_t(4 * c)).v = floatBitsToUint(o0[c]);
            W32(a + uint64_t(16 + 4 * c)).v = floatBitsToUint(o1[c]);
            W32(a + uint64_t(32 + 4 * c)).v = floatBitsToUint(o2[c]);
        }
    }
}

// ------------------------------------------------------------------------------------------- broad phase and AABB
shared uint sBoxU[6];               // running union box: min x,y,z (ordered uint encoding), max x,y,z
shared uint sMaskW[96];             // candidate bits, 32-bit words (qword m = words 2m, 2m+1)
uint f2o(float f) { uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u); }
float o2f(uint u) { return uintBitsToFloat((u & 0x80000000u) != 0u ? (u & 0x7fffffffu) : ~u); }

// item i of the broad phase: set b = i / 192, record k = i % 192 (the guest's ballots: qword i >> 6, bit i & 63)
bool broad_item(uint i, vec3 bmin, vec3 bmax) {
    uint b = i / 192u, k = i - 192u * b;
    uint64_t setp = packUint2x32(uvec2(vb_u32(vbT18, b, 0u), vb_u32(vbT18, b, 4u)));
    uint count = gld32(setp + 152ul);
    if (!(k < count)) return false;
    bool en = true;
    if (b == 0u) {
        uint64_t mk = gld64(sTaddr + 0x118ul + 8ul * uint64_t(k >> 6));
        en = (mk & (1ul << (k & 63u))) != 0ul;
    }
    if (!en) return false;
    uint64_t V = gld64(setp + 136ul);
    uint64_t base = (V & 0xfffffffful) | (uint64_t(int64_t(int16_t(uint16_t((V >> 32) & 0xfffful)))) << 32);
    uint64_t rec = rec_at(rec_dev(base), k);
    uint f8 = rec_u(rec, 8u);
    if (!((f8 & Tu(0x10cu)) != 0u && (f8 & Tu(0x110u)) == 0u)) return false;
    uint F108 = Tu(0x108u);
    uint f4 = rec_u(rec, 4u);
    uint m1 = b == 0u ? 1u : 2u;
    uint m2 = (F108 & 16u) != 0u ? 12u : (b == 0u ? 4u : 8u);
    bool inflate = (F108 & 64u) == 0u && (f4 & m2) != 0u;
    bool hit = (f4 & m1) != 0u || inflate;
    if (!hit) return false;
    uint type = rec_u(rec, 0u);
    vec4 R0 = rec_f4(rec, 16u), R1 = rec_f4(rec, 32u);
    float sc = inflate ? 1.5 : 1.0;
    vec3 lo, hi;
    if (type == 0u) {
        vec4 R2 = rec_f4(rec, 48u);
        float ex = (abs(R0.x) + abs(R0.y)) + abs(R0.z), ey = (abs(R1.x) + abs(R1.y)) + abs(R1.z), ez = (abs(R2.x) + abs(R2.y)) + abs(R2.z);
        lo = vec3(R0.w - sc * ex, R1.w - sc * ey, R2.w - sc * ez);
        hi = vec3(sc * ex + R0.w, sc * ey + R1.w, sc * ez + R2.w);
    } else if (type == 1u) {
        float sr = sc * R0.w;
        lo = vec3(R0.x - sr, R0.y - sr, R0.z - sr);
        float sr2 = R0.w * sc;
        hi = vec3(sr2 + R0.x, sr2 + R0.y, sr2 + R0.z);
    } else if (type == 2u) {
        float s0 = sc * R0.w, s1 = sc * R1.w;
        lo = min(vec3(R0.x - s0, R0.y - s0, R0.z - s0), vec3(R1.x - s1, R1.y - s1, R1.z - s1));
        float t0 = R0.w * sc, t1 = R1.w * sc;
        hi = max(vec3(t0 + R0.x, t0 + R0.y, t0 + R0.z), vec3(t1 + R1.x, t1 + R1.y, t1 + R1.z));
    } else return true;
    return bmax.x > lo.x && bmax.y > lo.y && bmax.z > lo.z && bmin.x < hi.x && bmin.y < hi.y && bmin.z < hi.z;
}

// ------------------------------------------------------------------------------------------- main
uint64_t instance_table() {
    if (pc.user_table != 0ul) {
        uint base = WgMap(pc.wg_map).v[gl_WorkGroupID.x] * 24u;
        UserTable t = UserTable(pc.user_table);
        return packUint2x32(uvec2(t.v[base], t.v[base + 1u]));
    }
    return packUint2x32(uvec2(pc.user[0], pc.user[1]));
}

void sync() { memoryBarrierBuffer(); memoryBarrierShared(); barrier(); }
void stop_dump() {
#if SHARED_STORAGE
    for (uint v = gl_LocalInvocationIndex; v < uint(nverts); v += NT) vb_store3(vbT38, v, X_get(int(v)));
#endif
}

void main() {
    if (pc.pages == 0ul) return;
    FAULT_PAGE = NUM_PAGES;
    PROBE_N = 0u;
    SCALARS = 0ul;
    uint tid = gl_LocalInvocationIndex;
    if (tid == 0u) sTaddr = instance_table();
    barrier();
    for (uint k = tid; k < 76u; k += NT) sT[k] = gld32(sTaddr + 4ul * uint64_t(k));
    barrier();
    uint64_t Pa = packUint2x32(uvec2(sT[2], sT[3]));
    for (uint k = tid; k < 72u; k += NT) sP[k] = gld32(Pa + 4ul * uint64_t(k));
    barrier();
    nverts = int(Pu(0u));
    P4 = Pu(4u);
    F = Tu(0xd4u);
    gObj = packUint2x32(uvec2(sT[4], sT[5]));
    objscale = gldf(gObj);
    vbT18 = vb_T(0x18u); vbT28 = vb_T(0x28u); vbT38 = vb_T(0x38u);
    vbP18 = vb_P(0x18u); vbP28 = vb_P(0x28u); vbP38 = vb_P(0x38u); vbP48 = vb_P(0x48u); vbP58 = vb_P(0x58u);
    vbP68 = vb_P(0x68u); vbP78 = vb_P(0x78u); vbP88 = vb_P(0x88u); vbP98 = vb_P(0x98u); vbPA8 = vb_P(0xa8u);
    vbPB8 = vb_P(0xb8u); vbPC8 = vb_P(0xc8u); vbPD8 = vb_P(0xd8u); vbPE8 = vb_P(0xe8u); vbPF8 = vb_P(0xf8u);
    vbP108 = vb_P(0x108u);
    uint64_t G = packUint2x32(uvec2(sT[0], sT[1]));
    vbG0 = vb_make(gld32(G), gld32(G + 4ul), gld32(G + 8ul), gld32(G + 12ul));
#if SHARED_STORAGE
    VB vbG1 = vbG0;                     // not used (anchors in shared memory)
#else
    VB vbG1 = vb_make(gld32(G + 16ul), gld32(G + 20ul), gld32(G + 24ul), gld32(G + 28ul));
#endif
    uint64_t C = packUint2x32(uvec2(vb_u32(vbT18, 0u, 0u), vb_u32(vbT18, 0u, 4u)));   // set 0 / palette holder
    vbPal = vb_make(gld32(C + 0x38ul), gld32(C + 0x3cul), gld32(C + 0x40ul), gld32(C + 0x44ul));
    // storage of the 1024-thread program: anchors in G1 at T[0xbc] + i, positions in T.V#(0x38)
    vbA = vbG1; offA = Tu(0xbcu);
    vbX = vbT38; offX = 0u;
    nsets = Tb(0xa4u);
    for (uint k = tid; k < 16u && k < nsets; k += NT) {
        uint64_t setp = gld64(packUint2x32(uvec2(sT[6], sT[7] & 0xffffu)) + 8ul * uint64_t(k));
        sSetPtr[k] = setp;
        uint64_t V = gld64(setp + 0x88ul);
        // the full paths read records with s_buffer_load through this V#: bounds-checked against its num_records
        // (bytes); an empty V# (num_records 0, a dummy base) reads 0 and is never translated
        uint num = gld32(setp + 0x90ul), st = (uint(V >> 32) >> 16) & 0x3fffu;
        uint bytes = st != 0u ? num * st : num;            // as the translation bounds s_buffer_load
        sRecNum[k] = bytes;
        sRecBase[k] = bytes == 0u ? 0ul : rec_dev(V & 0xfffffffffffful);
    }
    if (tid == 0u) {
        sBoxU[0] = f2o(FLT_MAX); sBoxU[1] = f2o(FLT_MAX); sBoxU[2] = f2o(FLT_MAX);
        sBoxU[3] = f2o(-FLT_MAX); sBoxU[4] = f2o(-FLT_MAX); sBoxU[5] = f2o(-FLT_MAX);
    }
    // section A: anchors
    for (uint v = tid; v < uint(nverts); v += NT) A_set(int(v), anchor_of(v));
    sync();
#if STOP == 4
    for (uint v = tid; v < uint(nverts); v += NT) vb_store3(vbT38, v, A_get(int(v)));
    return;
#endif
#if STOP == 5
    for (uint v = tid; v < uint(nverts); v += NT) vb_store3(vbT38, v, anchor_of(v));
    return;
#endif
#if STOP == 6
    for (int v = nverts - 1 - int(tid); v >= 0; v -= NT) vb_store3(vbT38, uint(v), anchor_of(uint(v)));
    return;
#endif
#if STOP == 7
    [[dont_unroll]] for (uint v = tid; v < uint(nverts); v += NT) vb_store3(vbT38, v, anchor_of(v));
    return;
#endif
#if STOP == 8
    for (uint base = 0u; base < uint(nverts); base += NT) {
        uint v = base + tid;
        if (v < uint(nverts)) vb_store3(vbT38, v, anchor_of(v));
        barrier();
    }
    return;
#endif
#if STOP == 9
    {
        VB outb = vb_T(0x38u);
        for (uint v = tid; v < uint(nverts); v += NT) {
            uint64_t a = outb.dev + uint64_t(outb.stride * v);
            W32(a).v = floatBitsToUint(float(v));
            W32(a + 4ul).v = floatBitsToUint(float(tid));
            W32(a + 8ul).v = floatBitsToUint(1234.0);
        }
    }
    return;
#endif
    dt = Tf(0xccu);
    uint nsub = Tb(0xd8u);
    float h64 = Th(0x64u);
    bool run = dt > 0.0 && nsub > 0u;
    bool sim = run;
    if (h64 == 0.0) {
        // simulation off: positions = anchors
        for (uint v = tid; v < uint(nverts); v += NT) {
            vec3 a = A_get(int(v));
            if (run) vb_store3(vbT38, v, a);
            else vb_store3(vbT28, v, vec3(a.x - Tf(0x48u), a.y - Tf(0x4cu), a.z - Tf(0x50u)));
        }
    } else if (run) {
        float inv_nsub = rcp_f32(float(nsub));
        idt = Tf(0xc8u);
        bool F4 = (F & 4u) != 0u, F16 = (F & 16u) != 0u, F32 = (F & 32u) != 0u, F8 = (F & 8u) != 0u, F2 = (F & 2u) != 0u;
        uint T108 = Tu(0x108u);
        for (int s = int(nsub) - 1; s >= 0; s--) {
#if STOP == 1
            for (uint v = tid; v < uint(nverts); v += NT) X_set(int(v), predict(v));
            sync();
            stop_dump();
            return;
#endif
            for (uint v = tid; v < uint(nverts); v += NT) X_set(int(v), tethers(v, predict(v)));
            sync();
            // collision box: running union of the predicted positions (or T's box)
            vec3 bmin, bmax;
            if (F32) {
                vec3 mn = vec3(FLT_MAX), mx = vec3(-FLT_MAX);
                for (uint v = tid; v < uint(nverts); v += NT) {
                    vec3 x = X_get(int(v));
                    mx = max(vec3(-FLT_MAX), max(mx, x));
                    mn = min(vec3(FLT_MAX), min(mn, x));
                }
                mn = vec3(subgroupMin(mn.x), subgroupMin(mn.y), subgroupMin(mn.z));
                mx = vec3(subgroupMax(mx.x), subgroupMax(mx.y), subgroupMax(mx.z));
                if (subgroupElect()) {
                    atomicMin(sBoxU[0], f2o(mn.x)); atomicMin(sBoxU[1], f2o(mn.y)); atomicMin(sBoxU[2], f2o(mn.z));
                    atomicMax(sBoxU[3], f2o(mx.x)); atomicMax(sBoxU[4], f2o(mx.y)); atomicMax(sBoxU[5], f2o(mx.z));
                }
                sync();
                bmin = vec3(o2f(sBoxU[0]), o2f(sBoxU[1]), o2f(sBoxU[2]));
                bmax = vec3(o2f(sBoxU[3]), o2f(sBoxU[4]), o2f(sBoxU[5]));
            } else {
                bmin = vec3(Tf(0x98u), Tf(0x9cu), Tf(0xa0u));
                bmax = vec3(Tf(0xa8u), Tf(0xacu), Tf(0xb0u));
            }
            // broad phase -> candidate masks
            for (uint k = tid; k < 96u; k += NT) sMaskW[k] = 0u;
            sync();
            for (uint i = tid; i < 192u * nsets; i += NT)
                if (broad_item(i, bmin, bmax)) atomicOr(sMaskW[i >> 5], 1u << (i & 31u));
            sync();
            for (uint k = tid; k < 48u; k += NT) sMask[k] = packUint2x32(uvec2(sMaskW[2u * k], sMaskW[2u * k + 1u]));
            sync();
#if STOP == 2
            stop_dump();
            return;
#endif
            // constraint iterations
            int iters = max(int(Tb(0xdau)), int(Tb(0xd9u)) - (s >> 1));
            inv_iters = rcp_f32(float(iters));
            for (int it = iters - 1; it >= 0; it--) {
                // distance constraints, graph-colored groups
                uint G_ = Pu(0x10u);
                for (int g = 0; g < int(G_); g++) {
                    float hdc = Th(0xdcu);
                    uint type = vb_u32(vbPC8, 0u, uint(g) * 16u), first = vb_u32(vbPC8, 0u, uint(g) * 16u + 4u);
                    uint count = vb_u32(vbPC8, 0u, uint(g) * 16u + 8u);
                    bool compress = type == 0u && hdc < 1.0;
                    for (uint c = tid; c < count; c += NT) distance_constraint(first + c, compress, hdc);
                    sync();
                }
                // bending
                bool bend_once = F8 && !F2 && it == iters - 1, bend_every = !F8 && !F2;
                if (bend_once || bend_every) {
                    bool formulaA = (T108 & 0x80u) != 0u;
                    uint ng = Pu(0x14u);
                    for (uint g = 0u; g < ng; g++) {
                        uint first = vb_u32(vbPE8, 0u, g * 12u), count = vb_u32(vbPE8, 0u, g * 12u + 4u);
                        for (uint c = tid; c < min(count, BEND_LIMIT); c += NT) bending_constraint(first + c, formulaA, bend_every);
                        sync();
                    }
                }
                // edge collisions
                uint N = Tb(0xdbu);
                bool first_it = it == iters - 1, last_it = it == 0;
                uint rem = N == 0u ? uint(it) : uint(it) % N;
                bool skip = F4 && rem != 0u && !last_it && !first_it;
                if ((T108 & 0x200u) != 0u) {
                    for (int g = 0; g < int(G_); g++) {
                        if (vb_u32(vbPC8, 0u, uint(g) * 16u) != 0u) continue;
                        uint first = vb_u32(vbPC8, 0u, uint(g) * 16u + 4u), count = vb_u32(vbPC8, 0u, uint(g) * 16u + 8u);
                        for (uint c = tid; c < count; c += NT) {
                            if (!skip) edge_full(first + c, F4);
                            else edge_cached(first + c);
                        }
                        sync();
                    }
                }
                // per vertex: dt blend (last iteration), tethers, vertex collisions, max distance
                for (uint v = tid; v < uint(nverts); v += NT) {
                    vec3 p = X_get(int(v));
                    if (it == 0 && F16) {
                        float r = dt * rcp_f32(Tf(0xd0u));
                        if (uintBitsToFloat(0x3f666666u) > r) {
                            vec3 o = cur_of(v);
                            p = vec3(r * (p.x - o.x) + o.x, r * (p.y - o.y) + o.y, r * (p.z - o.z) + o.z);
                        }
                    }
                    p = tethers(v, p);
                    if ((T108 & 8u) == 0u) p = skip ? vertex_cached(v, p) : vertex_full(v, p, F4);
                    p = skin_constrain(v, p);
                    X_set(int(v), p);
                }
                sync();
#if STOP == 3
                stop_dump();
                return;
#endif
            }
            idt = rcp_f32(dt);
        }
        // result: finite positions, else the anchor
        for (uint v = tid; v < uint(nverts); v += NT) {
            vec3 p = X_get(int(v));
#if SHARED_STORAGE
            vb_store3(vbT38, v, finite3(p) ? p : A_get(int(v)));
#else
            if (!finite3(p)) vb_store3(vbT38, v, A_get(int(v)));
#endif
        }
    }
    sync();
    // cloth bones: pass 1 into C.V#(0x38), pass 2 into C.V#(0x78) when C.u32[0] != 0
    {
        VB Q = sim ? vbT38 : vbT28;
        vec3 sh = sim ? vec3(0.0) : vec3(Tf(0x48u), Tf(0x4cu), Tf(0x50u));
        for (uint v = tid; v < uint(nverts); v += NT) cloth_bone(v, Q, sh, vbPal);
    }
    if (gld32(C) != 0u) {
        VB Q = sim ? vbT28 : vbT38;
        vec3 sh = sim ? vec3(Tf(0x48u), Tf(0x4cu), Tf(0x50u)) : vec3(Tf(0x58u), Tf(0x5cu), Tf(0x60u));
        VB pal2 = vb_make(gld32(C + 0x78ul), gld32(C + 0x7cul), gld32(C + 0x80ul), gld32(C + 0x84ul));
        sync();
        for (uint v = tid; v < uint(nverts); v += NT) cloth_bone(v, Q, sh, pal2);
    }
    record_faults();
}
