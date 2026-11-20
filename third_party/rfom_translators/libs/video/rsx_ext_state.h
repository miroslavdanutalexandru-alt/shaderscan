/*
 * ps3recomp - RSX state the D3D12 backend used to ignore (Resistance port):
 *   NV4097_SET_ANTI_ALIASING_CONTROL  (0x1D7C) bit4 = alpha-to-coverage
 *   NV4097_SET_USER_CLIP_PLANE_CONTROL (0x1478) 4 bits per plane
 *   NV4097_SET_POINT_SIZE (0x1EE0) / SET_POINT_SPRITE_CONTROL (0x1EE8)
 *   per-texture SET_TEXTURE_FILTER / SET_TEXTURE_CONTROL0 sampler state
 * Pure helpers (no D3D12), so the decode and the shader text patch are tested
 * offline (tests/test_ext_state.c).
 */
#ifndef RSX_EXT_STATE_H
#define RSX_EXT_STATE_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- alpha-to-coverage ------------------------------------------------------------------------------
 * Every target here is single-sample, where coverage from alpha is an alpha
 * test at one half (as RPCS3 does without MSAA). alpha_ctl packs
 * enable<<16 | (GL func & 0xFF)<<8 | ref (0..255); func low bytes:
 * 0 NEVER 1 LESS 2 EQUAL 3 LEQUAL 4 GREATER 5 NOTEQUAL 6 GEQUAL 7 ALWAYS. */
#define RSX_A2C_REF 128u
static inline uint32_t rsx_a2c_alpha_ctl(uint32_t alpha_ctl, uint32_t aa_control)
{
    if (!(aa_control & 0x10u)) return alpha_ctl;
    const uint32_t en = (alpha_ctl >> 16) & 1u, func = (alpha_ctl >> 8) & 0xFFu, ref = alpha_ctl & 0xFFu;
    if (!en || func == 7u) return (1u << 16) | (6u << 8) | RSX_A2C_REF;
    if ((func == 4u || func == 6u) && ref < RSX_A2C_REF) return (1u << 16) | (6u << 8) | RSX_A2C_REF;
    return alpha_ctl;   /* a stricter or different test already decides */
}

/* ---- user clip planes -------------------------------------------------------------------------------
 * Plane i: bits [4i+3:4i] = 0 off, 1 LT (kept where the distance < 0), 2 GE.
 * Factor per plane, as RPCS3: off 0, LT -1, GE +1; the shader keeps
 * lerp(1, d * f, |f|) >= 0. Returns the number of enabled planes. */
static inline int rsx_clip_factors(uint32_t ctl, float f[6])
{
    int n = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t m = (ctl >> (4 * i)) & 0xFu;
        f[i] = m == 1u ? -1.0f : m == 2u ? 1.0f : 0.0f;
        if (f[i] != 0.0f) n++;
    }
    return n;
}

/* ---- point sprites ----------------------------------------------------------------------------------
 * Control: bit0 sprite enable, bits[2:1] R mode (0 zero, 1 keep r, 2 from s),
 * bits[17:8] = TEX0..TEX9 replaced by the sprite coordinate. */
static inline int rsx_psprite_enabled(uint32_t ctl) { return (int)(ctl & 1u); }
static inline uint32_t rsx_psprite_mask(uint32_t ctl) { return (ctl & 1u) ? (ctl >> 8) & 0x3FFu : 0u; }
static inline uint32_t rsx_psprite_rmode(uint32_t ctl) { return (ctl >> 1) & 3u; }
/* RSX point size range (pixels): NV40 rasterises 1 .. 64. */
static inline float rsx_point_size_clamp(float s)
{
    if (!(s >= 1.0f)) return 1.0f;   /* NaN too */
    return s > 64.0f ? 64.0f : s;
}
/* The 6 expanded vertices of one point (two triangles): corner index into
 * TL(0) TR(1) BL(2) BR(3); NDC x = c&1 ? +1 : -1, y = c&2 ? -1 : +1. */
static inline uint32_t rsx_psprite_corner(uint32_t k)
{
    static const uint8_t c[6] = { 0, 1, 2, 2, 1, 3 };
    return c[k % 6u];
}
/* Sprite coordinate of a corner, GL style: s left to right, t top to bottom
 * (GL_POINT_SPRITE_COORD_ORIGIN = UPPER_LEFT, what RPCS3 samples). */
static inline void rsx_psprite_uv(uint32_t corner, float* s, float* t)
{
    *s = (corner & 1u) ? 1.0f : 0.0f;
    *t = (corner & 2u) ? 1.0f : 0.0f;
}

/* ---- vertex shader variants -------------------------------------------------------------------------
 * Text patch on rsx_vp_decompile output. RSX_VSX_CLIP adds SV_ClipDistance0/1
 * from o5.yzw / o6.yzw; RSX_VSX_PSPRITE expands each point, drawn as six
 * copies of its vertex, into a screen-aligned square. Both read a b0 tail:
 *   float4 vp_clipf[2]  (factors of planes 0..3, 4..5)
 *   float4 vp_psp       (x size px, y/z = 1 / |viewport scale x/y|, w = flags
 *                        as uint: bit0 per-vertex size (o6.x), bits 8..17 the
 *                        replaced texcoords, bits 4..5 the R mode)
 * placed after vp_gfx (byte RSX_VSX_CB_OFF of the slot). Returns 0, or -1 if
 * an anchor is missing or `cap` is too small (text unchanged then). */
#define RSX_VSX_CLIP    1u
#define RSX_VSX_PSPRITE 2u
#define RSX_VSX_BCOL    4u   /* two-sided lighting: back colours o3/o4 out as COLOR2/COLOR3 */
#define RSX_VSX_CB_OFF  8768u
#define RSX_VSX_CB_SIZE 48u

static inline int rsx_vsx_insert(char* h, size_t cap, const char* anchor, const char* add, int after)
{
    char* at = strstr(h, anchor);
    if (!at) return -1;
    if (after) at += strlen(anchor);
    const size_t len = strlen(h), n = strlen(add);
    if (len + n + 1 > cap) return -1;
    memmove(at + n, at, len - (size_t)(at - h) + 1);
    memcpy(at, add, n);
    return 0;
}

static inline void rsx_vsx_cat(char* d, size_t cap, const char* s)
{
    const size_t a = strlen(d), b = strlen(s);
    if (a + b < cap) memcpy(d + a, s, b + 1);
}

/* RSX_VS_STRICT: the decompiled VS text with its temporaries, outputs and
 * position declared `precise` (no fusing / reassociation on the way to
 * SV_Position), compiled with D3DCOMPILE_IEEE_STRICTNESS. A depth prepass and
 * the colour pass with depth EQUAL then agree to the bit even when they come
 * from different programs or the driver strips unused outputs from one.
 * Adapted from GoW-Ascension-Recomp ld_vs_precise_text (MIT; notice in
 * THIRD_PARTY.md). Writes NUL-terminated text to out; returns its length, or
 * -1 when src is not a decompiler VP (markers missing) or cap is too small. */
static inline int rsx_vs_precise_text(const char* src, char* out, size_t cap)
{
    static const char* const from[2] = { "    float4 r[32]; float4 o[16];\n", "    float4 _p = o[0];\n" };
    static const char* const to[2]   = { "    precise float4 r[32]; precise float4 o[16];\n", "    precise float4 _p = o[0];\n" };
    const char* at[2];
    if (!src || !out) return -1;
    for (int i = 0; i < 2; i++) if (!(at[i] = strstr(src, from[i]))) return -1;
    if (at[1] < at[0]) return -1;
    const size_t len = strlen(src);
    if (len + 32u + 1u > cap) return -1;
    size_t n = 0;
    const char* p = src;
    for (int i = 0; i < 2; i++) {
        memcpy(out + n, p, (size_t)(at[i] - p)); n += (size_t)(at[i] - p);
        memcpy(out + n, to[i], strlen(to[i]));   n += strlen(to[i]);
        p = at[i] + strlen(from[i]);
    }
    memcpy(out + n, p, len - (size_t)(p - src) + 1u);
    n += len - (size_t)(p - src);
    return (int)n;
}

static inline int rsx_vsx_patch(char* h, size_t cap, uint32_t flags)
{
    if (!h || !(flags & (RSX_VSX_CLIP | RSX_VSX_PSPRITE | RSX_VSX_BCOL))) return 0;
    static const char a_out[] = "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n";
    static const char a_cb[]  = "    uint4  vp_gfx;\n";
    static const char a_sig[] = "VSOutput main(VSInput input) {";
    static const char a_end[] = "    Out.t8=o[15]; Out.t9=o[6];\n";
    if (!strstr(h, a_out) || !strstr(h, a_cb) || !strstr(h, a_sig) || !strstr(h, a_end)) return -1;
    const int own_vid = strstr(h, "uint vertex_id:SV_VertexID;") != NULL;
    const size_t need = strlen(h) + 2048;
    if (need > cap) return -1;
    char body[1536]; body[0] = 0;
    if (flags & RSX_VSX_BCOL)
        rsx_vsx_cat(body, sizeof body, "    Out.vsx_bc0 = o[3]; Out.vsx_bc1 = o[4];\n");
    if (flags & RSX_VSX_CLIP)
        rsx_vsx_cat(body, sizeof body,
            "    Out.vsx_clip0 = lerp((float4)1, float4(o[5].yzw, o[6].y) * vp_clipf[0], abs(vp_clipf[0]));\n"
            "    Out.vsx_clip1 = lerp((float2)1, o[6].zw * vp_clipf[1].xy, abs(vp_clipf[1].xy));\n");
    if (flags & RSX_VSX_PSPRITE) {
        rsx_vsx_cat(body, sizeof body, own_vid ? "    { uint _vk = input.vertex_id % 6u;\n"
                             : "    { uint _vk = vsx_vid % 6u;\n");
        rsx_vsx_cat(body, sizeof body,
            "      uint _vc = _vk < 3u ? _vk : (_vk == 3u ? 2u : (_vk == 4u ? 1u : 3u));\n"
            "      float2 _cn = float2((_vc & 1u) ? 1.0 : -1.0, (_vc & 2u) ? -1.0 : 1.0);\n"
            "      uint _pf = asuint(vp_psp.w);\n"
            "      float _ps = (_pf & 1u) ? clamp(o[6].x, 1.0, 64.0) : vp_psp.x;\n"
            "      Out.pos.xy += _cn * (0.5 * _ps) * vp_psp.yz * Out.pos.w;\n"
            "      uint _rm = (_pf >> 4) & 3u;\n"
            "      float2 _st = float2(_cn.x * 0.5 + 0.5, 0.5 - _cn.y * 0.5);\n"
            "#define VSX_PT(N, T) if (_pf & (1u << (8u + N))) T = float4(_st, _rm == 1u ? T.z : (_rm == 2u ? _st.x : 0.0), 1.0);\n"
            "      VSX_PT(0u, Out.t0) VSX_PT(1u, Out.t1) VSX_PT(2u, Out.t2) VSX_PT(3u, Out.t3) VSX_PT(4u, Out.t4)\n"
            "      VSX_PT(5u, Out.t5) VSX_PT(6u, Out.t6) VSX_PT(7u, Out.t7) VSX_PT(8u, Out.t8) VSX_PT(9u, Out.t9)\n"
            "#undef VSX_PT\n"
            "    }\n");
    }
    char outm[224]; outm[0] = 0;
    /* back colours first: the PS declares them right after TEXCOORD9 */
    if (flags & RSX_VSX_BCOL)
        rsx_vsx_cat(outm, sizeof outm, "    float4 vsx_bc0:COLOR2; float4 vsx_bc1:COLOR3;\n");
    if (flags & RSX_VSX_CLIP)
        rsx_vsx_cat(outm, sizeof outm, "    float4 vsx_clip0:SV_ClipDistance0; float2 vsx_clip1:SV_ClipDistance1;\n");
    if ((outm[0] && rsx_vsx_insert(h, cap, a_out, outm, 1)) ||
        rsx_vsx_insert(h, cap, a_cb, "    float4 vp_clipf[2];\n    float4 vp_psp;\n", 1) ||
        ((flags & RSX_VSX_PSPRITE) && !own_vid &&
         rsx_vsx_insert(h, cap, a_sig, "VSOutput main(VSInput input, uint vsx_vid : SV_VertexID) {", 0)) ||
        rsx_vsx_insert(h, cap, a_end, body, 1))
        return -1;
    if ((flags & RSX_VSX_PSPRITE) && !own_vid) {   /* drop the original signature the insert kept */
        char* s = strstr(h, a_sig);
        if (!s) return -1;
        memmove(s, s + sizeof a_sig - 1, strlen(s + sizeof a_sig - 1) + 1);
    }
    return 0;
}

/* The b0 tail for a draw (RSX_VSX_CB_SIZE bytes). */
static inline void rsx_vsx_tail(float out[12], uint32_t clip_ctl, float point_size, uint32_t sprite_ctl,
                                uint32_t out_mask, float vp_scale_x, float vp_scale_y)
{
    float f[6];
    rsx_clip_factors(clip_ctl, f);
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = f[3];
    out[4] = f[4]; out[5] = f[5]; out[6] = 0.0f; out[7] = 0.0f;
    const float sx = fabsf(vp_scale_x), sy = fabsf(vp_scale_y);
    out[8] = rsx_point_size_clamp(point_size);
    out[9] = sx > 0.0f ? 1.0f / sx : 0.0f;
    out[10] = sy > 0.0f ? 1.0f / sy : 0.0f;
    const uint32_t fl = ((out_mask >> 5) & 1u) | (rsx_psprite_mask(sprite_ctl) << 8) |
                        (rsx_psprite_enabled(sprite_ctl) ? rsx_psprite_rmode(sprite_ctl) << 4 : 0u);
    memcpy(&out[11], &fl, 4);
}

/* ---- per-texture sampler ----------------------------------------------------------------------------
 * SET_TEXTURE_FILTER: bias [12:0] (signed 4.8), min [18:16] (1 NEAREST,
 * 2 LINEAR, 3 NEAREST_NEAREST, 4 LINEAR_NEAREST, 5 NEAREST_LINEAR,
 * 6 LINEAR_LINEAR, 7 convolution ~ linear_linear), mag [26:24] (1 NEAREST,
 * 2 LINEAR, 4 convolution ~ linear). SET_TEXTURE_CONTROL0: max aniso [6:4]
 * (1,2,4,6,8,10,12,16), max LOD [18:7], min LOD [30:19] (unsigned 4.8).
 * Address (SET_TEXTURE_ADDRESS) u [3:0] v [11:8] w [19:16]. */
typedef struct {
    uint8_t min_linear, mag_linear, mip;   /* mip: 0 none (level 0 only), 1 nearest, 2 linear */
    uint8_t aniso;                         /* 1 = off */
    uint8_t addr[3];                       /* D3D12_TEXTURE_ADDRESS_MODE values */
    float bias, min_lod, max_lod;
} rsx_gsamp;

static inline uint8_t rsx_gsamp_addr(uint32_t m)
{
    switch (m & 0xFu) {               /* D3D12: 1 WRAP 2 MIRROR 3 CLAMP 4 BORDER 5 MIRROR_ONCE */
    case 1: return 1;
    case 2: return 2;
    case 4: case 7: return 4;
    case 6: case 8: return 5;
    case 3: case 5: return 3;
    default: return 1;
    }
}

/* aniso_override > 1 (RSX_ANISO) replaces the game's level on mip-mapped
 * units; bias_add (RSX_LOD_BIAS / upscaler bias) adds to the game's bias.
 * honour_addr = 0 keeps WRAP (the shader emulates clamp / mirror itself). */
static inline rsx_gsamp rsx_gsamp_decode(uint32_t filter, uint32_t ctrl0, uint32_t address,
                                         int aniso_override, float bias_add, int honour_addr)
{
    rsx_gsamp g;
    memset(&g, 0, sizeof g);
    uint32_t minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    if (minf == 0u) minf = 6u;                 /* unprogrammed: trilinear */
    if (minf == 7u) minf = 6u;
    g.min_linear = (minf == 2u || minf == 4u || minf == 6u);
    g.mip = minf >= 5u ? 2u : minf >= 3u ? 1u : 0u;
    g.mag_linear = magf != 1u;
    int32_t b = (int32_t)(filter & 0x1FFFu);
    if (b & 0x1000) b -= 0x2000;
    g.bias = (float)b / 256.0f + bias_add;
    if (g.bias < -16.0f) g.bias = -16.0f;
    if (g.bias > 15.99f) g.bias = 15.99f;
    static const uint8_t an[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
    g.aniso = an[(ctrl0 >> 4) & 7u];
    if (aniso_override > 1) g.aniso = (uint8_t)(aniso_override > 16 ? 16 : aniso_override);
    if (!g.mip || !g.min_linear || !g.mag_linear) g.aniso = 1;   /* D3D anisotropy filters linearly */
    if (g.mip && !(ctrl0 & 0x80000000u)) g.max_lod = 16.0f;   /* CONTROL0 never written: whole chain */
    else if (g.mip) {                          /* non-mip filters sample the base level only */
        g.min_lod = (float)((ctrl0 >> 19) & 0xFFFu) / 256.0f;
        g.max_lod = (float)((ctrl0 >> 7) & 0xFFFu) / 256.0f;
        if (g.max_lod < g.min_lod) g.max_lod = g.min_lod;
    }
    if (honour_addr) {
        g.addr[0] = rsx_gsamp_addr(address);
        g.addr[1] = rsx_gsamp_addr(address >> 8);
        g.addr[2] = rsx_gsamp_addr(address >> 16);
    } else g.addr[0] = g.addr[1] = g.addr[2] = 1;
    return g;
}

/* D3D12_FILTER value (min 0x10, mag 0x4, mip-linear 0x1; 0x55 = ANISOTROPIC). */
static inline uint32_t rsx_gsamp_d3d_filter(const rsx_gsamp* g)
{
    if (g->aniso > 1) return 0x55u;
    return (g->min_linear ? 0x10u : 0u) | (g->mag_linear ? 0x4u : 0u) | (g->mip == 2 ? 0x1u : 0u);
}

/* 64-bit identity of a decoded sampler (cache key). */
static inline uint64_t rsx_gsamp_key(const rsx_gsamp* g)
{
    uint32_t bi, lo, hi;
    memcpy(&bi, &g->bias, 4);
    lo = (uint32_t)(g->min_lod * 256.0f);
    hi = (uint32_t)(g->max_lod * 256.0f);
    return ((uint64_t)g->min_linear) | ((uint64_t)g->mag_linear << 1) | ((uint64_t)g->mip << 2) |
           ((uint64_t)(g->aniso & 31u) << 4) | ((uint64_t)g->addr[0] << 9) | ((uint64_t)g->addr[1] << 12) |
           ((uint64_t)g->addr[2] << 15) | ((uint64_t)(lo & 0x1FFFu) << 18) | ((uint64_t)(hi & 0x1FFFu) << 31) |
           ((uint64_t)(bi >> 12) << 44);   /* LOD 4.8 in 13 bits (16.0 = 0x1000); float bias, 20 high bits */
}

/* ---- pipeline extension bits ------------------------------------------------------------------------
 * State that changes the D3D12 pipeline desc (blend / raster / depth-stencil)
 * rides in the draw's colour-mask key above the RT0 nibble: key = cmask |
 * ext << 4, and PsoRec.ext keeps it for the pre-built manifest.
 *   bits 0..11  RT1..3 write masks (D3D order R1 G2 B4 A8), valid with MRT_VALID
 *   bits 12..14 RT1..3 blend enable, valid with MRT_VALID
 *   bit 15      MRT_VALID: COLOR_MASK_MRT / BLEND_ENABLE_MRT were written
 *   bit 16/17   fill: wireframe / point (points fill solid: D3D12 has no point fill)
 *   bit 18      depth clip (ZMIN_MAX_CONTROL clip && !clamp)
 *   bit 19      depth bounds test
 *   bits 20..22 logic op emulation (RSX_LOGIC_*)
 *   bit 24      antialiased lines (LINE_SMOOTH) */
#define RSX_X_MRT_VALID   (1u << 15)
#define RSX_X_WIRE        (1u << 16)
#define RSX_X_POINTFILL   (1u << 17)
#define RSX_X_DEPTH_CLIP  (1u << 18)
#define RSX_X_DBOUNDS     (1u << 19)
#define RSX_X_LOGIC_SHIFT 20
#define RSX_X_LINE_AA     (1u << 24)
#define RSX_LOGIC_NONE    0u
#define RSX_LOGIC_CLEAR   1u   /* blend ZERO, ZERO */
#define RSX_LOGIC_NOOP    2u   /* write mask 0 */
#define RSX_LOGIC_COPY    3u   /* blend off */
#define RSX_LOGIC_OTHER   7u   /* no UNORM equivalent: the blend state is kept */

/* COLOR_MASK_MRT nibble (bit0 A, bit1 R, bit2 G, bit3 B) -> D3D write mask */
static inline uint32_t rsx_mrt_nibble_d3d(uint32_t n)
{
    return ((n & 2u) ? 1u : 0u) | ((n & 4u) ? 2u : 0u) | ((n & 8u) ? 4u : 0u) | ((n & 1u) ? 8u : 0u);
}

/* RT1..3 masks and blend enables. Registers never written: RT1..3 mirror RT0
 * (the old behaviour; MRT_VALID clear). Written (RPCS3): RT i's mask is the
 * COLOR_MASK_MRT nibble i, its blend bit i of BLEND_ENABLE_MRT. */
static inline uint32_t rsx_x_mrt(uint32_t rt0_mask, int rt0_blend, uint32_t blend_mrt, int blend_mrt_set,
                                 uint32_t mask_mrt, int mask_mrt_set)
{
    if (!blend_mrt_set && !mask_mrt_set) return 0;
    uint32_t x = RSX_X_MRT_VALID;
    for (uint32_t i = 1; i < 4; i++) {
        const uint32_t m = mask_mrt_set ? rsx_mrt_nibble_d3d(mask_mrt >> (i * 4u)) : (rt0_mask & 0xFu);
        const uint32_t b = blend_mrt_set ? ((blend_mrt >> i) & 1u) : (rt0_blend ? 1u : 0u);
        x |= m << ((i - 1u) * 4u);
        x |= b << (11u + i);
    }
    return x;
}

/* LOGIC_OP (GL enums 0x1500..0x150F) -> emulation code; 0 when disabled.
 * D3D12 logic ops need UINT targets and RSX targets here are UNORM/FLOAT. */
static inline uint32_t rsx_x_logic(int enable, uint32_t op)
{
    if (!enable) return RSX_LOGIC_NONE;
    switch (op) {
    case 0x1500u: return RSX_LOGIC_CLEAR;
    case 0x1505u: return RSX_LOGIC_NOOP;
    case 0x1503u: return RSX_LOGIC_COPY;
    default:      return RSX_LOGIC_OTHER;
    }
}

/* FRONT/BACK_POLYGON_MODE (0x1B00 POINT, 0x1B01 LINE, 0x1B02 FILL): one fill
 * mode for the draw -- the face that survives culling, front when both do. */
static inline uint32_t rsx_x_fill(uint32_t front, uint32_t back, int cull_on, uint32_t cull_face)
{
    uint32_t m = front;
    if (front != back && cull_on && cull_face == 0x0404u) m = back;   /* front faces culled */
    return m == 0x1B01u ? RSX_X_WIRE : m == 0x1B00u ? RSX_X_POINTFILL : 0u;
}

/* ZMIN_MAX_CONTROL: [3:0] clip enable, [7:4] clamp enable. Clipping happens
 * when it is enabled and clamping is not (RPCS3: clamp = clamp || !clip). */
static inline int rsx_depth_clip(uint32_t zminmax)
{
    return (zminmax & 0xFu) != 0u && ((zminmax >> 4) & 0xFu) == 0u;
}

/* CLIP_MIN/MAX: window depth is clipped to [n, f]. The VS writes window z
 * (z * s + w * o), so remap it to (zw - n) / (f - n) and let the viewport's
 * MinDepth/MaxDepth = n/f map it back (RPCS3's apply_zclip_xform). Returns 0
 * (nothing to do) for the default 0..1 or a range D3D12 cannot express. */
static inline int rsx_depth_range_xform(float n, float f, float* s, float* o)
{
    if (!(n >= 0.0f && f <= 1.0f && f > n)) return 0;
    if (n == 0.0f && f == 1.0f) return 0;
    const float k = 1.0f / (f - n);
    *s = *s * k;
    *o = (*o - n) * k;
    return 1;
}

/* POLYGON_OFFSET_BIAS in window-depth units of the guest's zeta format
 * (SURFACE_FORMAT depth field: 1 = Z16, else Z24S8): one unit is 1/65535 or
 * 1/16777215 of the depth range. Added to the VS z offset (RSX_POLY_OFFSET=3)
 * it moves window depth by exactly that, independent of the host DSV format
 * (GoW-Ascension-Recomp LD_ZBIAS_VS, MIT; notice in THIRD_PARTY.md). */
static inline float rsx_zbias_window(float bias, uint32_t depth_fmt)
{
    return bias / (depth_fmt == 1u ? 65535.0f : 16777215.0f);
}

/* SET_FOG_MODE -> code the shader switches on (rsx_ext[0].z as uint) */
static inline uint32_t rsx_fog_code(uint32_t mode)
{
    switch (mode) {
    case 0x2601u: return 1u;   /* LINEAR */
    case 0x0800u: return 2u;   /* EXP */
    case 0x0801u: return 3u;   /* EXP2 */
    case 0x0802u: return 4u;   /* EXP_ABS */
    case 0x0803u: return 5u;   /* EXP2_ABS */
    case 0x0804u: return 6u;   /* LINEAR_ABS */
    default:      return 0u;
    }
}
/* CPU reference of the shader's fog value (y); tests compare against it. */
static inline float rsx_fog_eval(uint32_t code, float p0, float p1, float x)
{
    if (code >= 4u) x = fabsf(x);
    float y = 0.0f, t;
    switch (code) {
    case 1u: case 6u: y = p1 * x + (p0 - 1.0f); break;
    case 2u: case 4u: y = expf(11.084f * (p1 * x + p0 - 1.5f)); break;
    case 3u: case 5u: t = 4.709f * (p1 * x + p0 - 1.5f); y = expf(-t * t); break;
    default: break;
    }
    return y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
}

/* SET_WINDOW_OFFSET (x [15:0], y [31:16]) moves the guest viewport inside the
 * surface. Returns 0 (full-target viewport) when the moved viewport is
 * degenerate or leaves the tw x th target, as the unmoved one did before. */
static inline int rsx_window_viewport(uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh,
                                      uint32_t wo, float tw, float th, float out_xy[2])
{
    const uint32_t x = vx + (wo & 0xFFFFu), y = vy + (wo >> 16);
    if (vw < 2u || vh < 2u || (float)(x + vw) > tw + 0.5f || (float)(y + vh) > th + 0.5f) return 0;
    out_xy[0] = (float)x; out_xy[1] = (float)y;
    return 1;
}

/* TEXTURE_BORDER_COLOR (ARGB8: B bits 0..7, G 8..15, R 16..23, A 24..31) */
static inline void rsx_border_rgba(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    out[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    out[2] = (float)(c & 0xFFu) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}
/* SET_TEXTURE_ADDRESS axes that use the border colour: bit0 U, bit1 V.
 * Modes BORDER (4) and MIRROR_ONCE_BORDER (7: mirrored with abs() first,
 * border where |uv| > 1). */
static inline uint32_t rsx_addr_border_axes(uint32_t address)
{
    const uint32_t u = address & 0xFu, v = (address >> 8) & 0xFu;
    return ((u == 4u || u == 7u) ? 1u : 0u) | ((v == 4u || v == 7u) ? 2u : 0u);
}
static inline int rsx_addr_border(uint32_t address) { return rsx_addr_border_axes(address) != 0u; }
/* axes in MIRROR_ONCE_BORDER (bit0 U, bit1 V) */
static inline uint32_t rsx_addr_mirror_once_border_axes(uint32_t address)
{
    return ((address & 0xFu) == 7u ? 1u : 0u) | (((address >> 8) & 0xFu) == 7u ? 2u : 0u);
}
/* Axes the shader must emulate. hw_ok: the unit's honoured hardware sampler
 * is BORDER with the game's border colour, exact for mode 4 (D3D12 has no
 * mirror-once-border, so mode 7 always needs the shader). */
static inline uint32_t rsx_border_emul_axes(uint32_t address, int hw_ok)
{
    return hw_ok ? rsx_addr_mirror_once_border_axes(address) : rsx_addr_border_axes(address);
}
/* Shader mirror flags of a unit (rsx_sampm / rsx_sampb): bit0 U mirror (2),
 * bit1 V mirror, bit2 U mirror-once (6..8), bit3 V mirror-once. */
static inline uint32_t rsx_mirror_flags(uint32_t address)
{
    const uint32_t mu = address & 0xFu, mv = (address >> 8) & 0xFu;
    return (mu == 2u ? 1u : 0u) | (mv == 2u ? 2u : 0u)
         | ((mu >= 6u && mu <= 8u) ? 4u : 0u) | ((mv >= 6u && mv <= 8u) ? 8u : 0u);
}
/* rsx_sampb's per-unit code: bits 0..1 border axes (U, V), bits 2..5 the
 * mirror flags, so one helper applies mirror, mirror-once and the border per
 * axis. 0: no emulated border (the unit stays with rsx_sampm / the sampler). */
static inline uint32_t rsx_sampb_code(uint32_t address, int hw_ok)
{
    const uint32_t b = rsx_border_emul_axes(address, hw_ok);
    return b ? (b | (rsx_mirror_flags(address) << 2)) : 0u;
}
/* CPU reference of one emulated axis of rsx_sampb: the coordinate sampled
 * (clamped half a texel inside) and whether the border colour is returned. */
static inline float rsx_border_axis(float uv, int mirror_once, uint32_t size, int* outside)
{
    if (mirror_once) uv = fabsf(uv);
    *outside = uv < 0.0f || uv > 1.0f;
    const float hp = 0.5f / (float)(size ? size : 1u);
    return uv < hp ? hp : uv > 1.0f - hp ? 1.0f - hp : uv;
}
/* Pipeline manifest header (8 bytes): 7 magic/version bytes + the device caps
 * byte (0x80 | alpha factor 1 | independent back stencil 2 | depth bounds 4).
 * Returns 0 usable, 1 same format but other caps (discard), -1 other format. */
static inline uint8_t rsx_psom_caps(int alpha_factor, int stencil2, int dbounds)
{
    return (uint8_t)(0x80u | (alpha_factor ? 1u : 0u) | (stencil2 ? 2u : 0u) | (dbounds ? 4u : 0u));
}
static inline int rsx_psom_hdr_check(const uint8_t hdr[8], const uint8_t want[8])
{
    if (memcmp(hdr, want, 7) != 0) return -1;
    return hdr[7] == want[7] ? 0 : 1;
}
/* BLEND_SFACTOR/DFACTOR CONSTANT_ALPHA without a device ALPHA_FACTOR blend:
 * CONSTANT_ALPHA maps to BLEND_FACTOR, so the colour half reads the blend
 * colour's RGB. When the colour factors use CONSTANT_ALPHA but not
 * CONSTANT_COLOR, splat Ac to all four channels. Returns 1 if changed, -1
 * if both are used (unrepresentable; left as is). */
static inline int rsx_blend_const_alpha_fixup(uint32_t sfactor, uint32_t dfactor, int alpha_factor_hw, float f[4])
{
    if (alpha_factor_hw) return 0;
    const uint32_t s = sfactor & 0xFFFFu, d = dfactor & 0xFFFFu;
    const int ca = s == 0x8003u || s == 0x8004u || d == 0x8003u || d == 0x8004u;
    const int cc = s == 0x8001u || s == 0x8002u || d == 0x8001u || d == 0x8002u;
    if (!ca) return 0;
    if (cc) return -1;
    f[0] = f[1] = f[2] = f[3];
    return 1;
}

/* BLEND_COLOR -> D3D blend factor. 8-bit targets: ARGB8 like the border
 * colour. FP16 targets: R = low half, G = high half of BLEND_COLOR, B / A
 * the halves of BLEND_COLOR2. */
static inline float rsx_half_to_float(uint32_t h)
{
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0u) v = ldexpf((float)m, -24);
    else if (e == 31u) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 0x400u), (int)e - 25);
    return s ? -v : v;
}
static inline void rsx_blend_factor(uint32_t c, uint32_t c2, int fp16, float out[4])
{
    if (!fp16) { rsx_border_rgba(c, out); return; }
    out[0] = rsx_half_to_float(c & 0xFFFFu);
    out[1] = rsx_half_to_float(c >> 16);
    out[2] = rsx_half_to_float(c2 & 0xFFFFu);
    out[3] = rsx_half_to_float(c2 >> 16);
}

/* Intersect a pixel rect (x0,y0,x1,y1) with the guest scissor (in the same
 * guest pixel space). Unset (0 size) or the reset 4096x4096 scissor leaves the
 * rect alone (returns 0). An empty intersection gives x1 == x0. */
static inline int rsx_scissor_clip(float r[4], uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh)
{
    if (!sw || !sh || (sx == 0u && sy == 0u && sw >= 4096u && sh >= 4096u)) return 0;
    const float x0 = (float)sx, y0 = (float)sy, x1 = (float)(sx + sw), y1 = (float)(sy + sh);
    if (r[0] < x0) r[0] = x0;
    if (r[1] < y0) r[1] = y0;
    if (r[2] > x1) r[2] = x1;
    if (r[3] > y1) r[3] = y1;
    if (r[2] < r[0]) r[2] = r[0];
    if (r[3] < r[1]) r[3] = r[1];
    return 1;
}

/* CLEAR_SURFACE only writes inside the scissor (moved by the window offset),
 * as on RSX / RPCS3. Fall of Man's split screen clears the shared scene
 * surface once per player with the scissor on that player's half; clearing
 * the whole target wiped player 1's finished view before the composite.
 * r = the cleared rect in guest pixels of a tw x th target. Returns 0: clear
 * the whole target, 1: clear r only, -1: nothing to clear. */
static inline int rsx_clear_rect(uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t wo,
                                 float tw, float th, float r[4])
{
    r[0] = 0.0f; r[1] = 0.0f; r[2] = tw; r[3] = th;
    if (!sw || !sh) return 0;
    const uint32_t wx = wo & 0xFFFFu, wy = wo >> 16;
    if (!wx && !wy && sx == 0u && sy == 0u && sw >= 4096u && sh >= 4096u) return 0;
    if (!rsx_scissor_clip(r, sx + wx, sy + wy, sw, sh)) return 0;
    if (r[2] <= r[0] || r[3] <= r[1]) return -1;
    if (r[0] <= 0.0f && r[1] <= 0.0f && r[2] >= tw && r[3] >= th) return 0;
    return 1;
}

/* ---- fragment program variants ----------------------------------------------------------------------
 * Text patch on the decompiled FP (after the b1 cbuffer is unified). fpx:
 *   bit0 fog: input.fog -> (fogc, fog factor, 0, 0) per FOG_MODE / FOG_PARAMS
 *   bit3 two-sided lighting: back-facing pixels read the back colours
 *        (VS variant RSX_VSX_BCOL outputs them as COLOR2/COLOR3)
 *   bit4 flat shading: colours not interpolated
 *   bits 8..23 / 32..47 units whose U / V axis returns the border colour
 *        outside [0, 1] (emulated per axis)
 * Per-draw values sit in b1 float4 rsx_ext[5] (#70..74): [0] = fog p0, p1,
 * mode code (uint bits), 0; [1..4] = border colours of units 0..15 (ARGB8
 * uint bits). Returns 0, or -1 (text unchanged where possible). */
#define RSX_FPX_FOG    1u
#define RSX_FPX_TWOSIDE 8u
#define RSX_FPX_FLAT   16u
#define RSX_FPX_BORDER_SHIFT 8    /* U axis */
#define RSX_FPX_BORDERV_SHIFT 32  /* V axis */
#define RSX_FPX_EXT_FLOAT 280u   /* float index of rsx_ext[0] in the b1 slot */

static inline int rsx_fpx_replace(char* h, size_t cap, const char* from, const char* to)
{
    const size_t lf = strlen(from), lt = strlen(to);
    int n = 0;
    char* p = h;
    while ((p = strstr(p, from)) != NULL) {
        const size_t len = strlen(h);
        if (len - lf + lt + 1 > cap) return -1;
        memmove(p + lt, p + lf, strlen(p + lf) + 1);
        memcpy(p, to, lt);
        p += lt; n++;
    }
    return n;
}

static inline int rsx_fpx_ins(char* h, size_t cap, char* at, const char* add)
{
    if (!at) return -1;
    const size_t len = strlen(h), n = strlen(add);
    if (len + n + 1 > cap) return -1;
    memmove(at + n, at, strlen(at) + 1);
    memcpy(at, add, n);
    return 0;
}
/* first of "struct PSOutput" / "float4 main(" / "PSOutput main(" */
static inline char* rsx_fpx_main(char* h)
{
    static const char* const marks[] = { "struct PSOutput", "float4 main(", "PSOutput main(" };
    char* q = NULL;
    for (int m = 0; m < 3; m++) { char* t = strstr(h, marks[m]); if (t && (!q || t < q)) q = t; }
    return q;
}

/* mirror: per unit mirror flags (rsx_mirror_flags) of the border units, or
 * NULL; folded into rsx_sampb's compile-time code. */
static inline int rsx_fpx_patch_m(char* h, size_t cap, uint64_t fpx, const uint8_t* mirror)
{
    if (!h || !fpx) return 0;
    if (strstr(h, "cbuffer PSConstants")) return -1;   /* buffered constants layout: not handled */
    static const char a_tc7[] = "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n";
    static const char a_tc9[] = "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n";
    static const char a_col[] = "float4 col0 : COLOR0; float4 col1 : COLOR1;";
    static const char a_front[] = "    bool _rsx_front : SV_IsFrontFace;\n";
    if (!strstr(h, a_tc7) || !strstr(h, a_col)) return -1;
    if (!rsx_fpx_main(h)) return -1;
    const uint32_t bmask_u = (uint32_t)(fpx >> RSX_FPX_BORDER_SHIFT) & 0xFFFFu;
    const uint32_t bmask_v = (uint32_t)(fpx >> RSX_FPX_BORDERV_SHIFT) & 0xFFFFu;
    const uint32_t bmask = bmask_u | bmask_v;
    int uses = 0;
    if ((fpx & RSX_FPX_FOG) && strstr(h, "input.fog")) uses |= 1;
    if ((fpx & RSX_FPX_TWOSIDE) && (strstr(h, "input.col0") || strstr(h, "input.col1"))) uses |= 2;
    if (fpx & RSX_FPX_FLAT) uses |= 4;
    char pat[96];
    if (bmask)
        for (uint32_t u = 0; u < 16u; u++) {
            if (!(bmask & (1u << u))) continue;
            snprintf(pat, sizeof pat, "rsx_tex[%u].Sample(rsx_samp[%u], ", u, u);
            if (strstr(h, pat)) { uses |= 8; break; }
            snprintf(pat, sizeof pat, "rsx_sampc(rsx_tex[%u], rsx_samp[%u], ", u, u);
            if (strstr(h, pat)) { uses |= 8; break; }
        }
    if (!uses) return 0;
    if (strlen(h) + 4096 > cap) return -1;
    /* b1: append rsx_ext[5] to whichever layout is declared */
    if (uses & (1 | 8)) {
        static const char* const ends[] = {
            "float4 fp_k[64]; float4 rsx_shscale;",
            "float4 _rsx_b1pad[69]; float4 rsx_shscale;",
            "float4 _rsx_b1pad2[64]; float4 rsx_shscale;",
            "float4 _rsx_atpad[4]; float4 rsx_alphatest;",
        };
        int done = 0;
        for (int i = 0; i < 4 && !done; i++) {
            char* e = strstr(h, ends[i]);
            if (!e) continue;
            if (rsx_fpx_ins(h, cap, e + strlen(ends[i]), i == 3 ? " float4 _rsx_xpad[65]; float4 rsx_ext[5];"
                                                               : " float4 rsx_ext[5];")) return -1;
            done = 1;
        }
        if (!done) {
            if (strstr(h, "register(b1)")) return -1;   /* some other b1 layout */
            if (rsx_fpx_ins(h, cap, rsx_fpx_main(h), "cbuffer RsxExt : register(b1) { float4 _rsx_xpad[70]; float4 rsx_ext[5]; };\n")) return -1;
        }
    }
    /* helpers, right before main (after every declaration they read) */
    char helpers[2048]; helpers[0] = 0;
    if (uses & 1)
        rsx_vsx_cat(helpers, sizeof helpers,
            "float4 _rsx_fogv(float4 f) {\n"
            "    const uint m = asuint(rsx_ext[0].z); const float p0 = rsx_ext[0].x, p1 = rsx_ext[0].y;\n"
            "    const float x = (m >= 4u) ? abs(f.x) : f.x; float y = 0.0;\n"
            "    if (m == 1u || m == 6u) y = p1 * x + (p0 - 1.0);\n"
            "    else if (m == 2u || m == 4u) y = exp(11.084 * (p1 * x + p0 - 1.5));\n"
            "    else if (m == 3u || m == 5u) { const float t = 4.709 * (p1 * x + p0 - 1.5); y = exp(-t * t); }\n"
            "    return float4(f.x, saturate(y), 0.0, 0.0);\n"
            "}\n");
    if (uses & 8)
        rsx_vsx_cat(helpers, sizeof helpers,
            "float4 rsx_sampb(Texture2D t, SamplerState s, uint u, uint ax, float2 uv) {\n"
            "    const bool eu = (ax & 1u) != 0u, ev = (ax & 2u) != 0u;\n"
            "    if ((ax & 4u) != 0u) uv.x = 1.0 - abs((uv.x - 2.0 * floor(uv.x * 0.5)) - 1.0);\n"
            "    if ((ax & 8u) != 0u) uv.y = 1.0 - abs((uv.y - 2.0 * floor(uv.y * 0.5)) - 1.0);\n"
            "    if ((ax & 16u) != 0u) uv.x = abs(uv.x);\n"
            "    if ((ax & 32u) != 0u) uv.y = abs(uv.y);\n"
            "    uint w, hh; t.GetDimensions(w, hh); const float2 hp = 0.5 / float2(max(w, 1u), max(hh, 1u));\n"
            "    float2 cu = uv;\n"
            "    if (eu) cu.x = clamp(uv.x, hp.x, 1.0 - hp.x); else if ((ax & 16u) != 0u) cu.x = saturate(uv.x);\n"
            "    if (ev) cu.y = clamp(uv.y, hp.y, 1.0 - hp.y); else if ((ax & 32u) != 0u) cu.y = saturate(uv.y);\n"
            "    const float4 v = t.Sample(s, cu);\n"
            "    const uint c = asuint(rsx_ext[1u + u / 4u][u % 4u]);\n"
            "    const float4 b = float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0;\n"
            "    const bool o = (eu && (uv.x < 0.0 || uv.x > 1.0)) || (ev && (uv.y < 0.0 || uv.y > 1.0));\n"
            "    return o ? b : v;\n"
            "}\n");
    if (helpers[0] && rsx_fpx_ins(h, cap, rsx_fpx_main(h), helpers)) return -1;
    if (uses & 1) {
        if (rsx_fpx_replace(h, cap, "input.fog", "_rsx_fogv(input.fog)") < 0) return -1;
    }
    if (uses & 8)
        for (uint32_t u = 0; u < 16u; u++) {
            if (!(bmask & (1u << u))) continue;
            char to[96];
            const uint32_t ax = ((bmask_u >> u) & 1u) | (((bmask_v >> u) & 1u) << 1)
                              | (mirror ? ((uint32_t)(mirror[u] & 0xFu) << 2) : 0u);
            snprintf(pat, sizeof pat, "rsx_tex[%u].Sample(rsx_samp[%u], ", u, u);
            snprintf(to, sizeof to, "rsx_sampb(rsx_tex[%u], rsx_samp[%u], %uu, %uu, ", u, u, u, ax);
            if (rsx_fpx_replace(h, cap, pat, to) < 0) return -1;
            snprintf(pat, sizeof pat, "rsx_sampc(rsx_tex[%u], rsx_samp[%u], ", u, u);
            if (rsx_fpx_replace(h, cap, pat, to) < 0) return -1;
        }
    if (uses & 2) {
        if (!strstr(h, a_tc9) && rsx_vsx_insert(h, cap, a_tc7, a_tc9, 1)) return -1;
        if (rsx_vsx_insert(h, cap, a_tc9, "    float4 bcol0 : COLOR2; float4 bcol1 : COLOR3;\n", 1)) return -1;
        if (!strstr(h, a_front)) {
            char* e = strstr(h, "    float4 bcol0 : COLOR2; float4 bcol1 : COLOR3;\n");
            if (!e || rsx_vsx_insert(e, cap - (size_t)(e - h), "    float4 bcol0 : COLOR2; float4 bcol1 : COLOR3;\n", a_front, 1)) return -1;
        }
        if (rsx_fpx_replace(h, cap, "input.col0", "(input._rsx_front ? input.col0 : input.bcol0)") < 0 ||
            rsx_fpx_replace(h, cap, "input.col1", "(input._rsx_front ? input.col1 : input.bcol1)") < 0)
            return -1;
    }
    if (uses & 4) {
        if (rsx_fpx_replace(h, cap, "float4 col0 : COLOR0; float4 col1 : COLOR1;",
                            "nointerpolation float4 col0 : COLOR0; nointerpolation float4 col1 : COLOR1;") < 0 ||
            rsx_fpx_replace(h, cap, "float4 bcol0 : COLOR2; float4 bcol1 : COLOR3;",
                            "nointerpolation float4 bcol0 : COLOR2; nointerpolation float4 bcol1 : COLOR3;") < 0)
            return -1;
    }
    return 1;
}
static inline int rsx_fpx_patch(char* h, size_t cap, uint64_t fpx) { return rsx_fpx_patch_m(h, cap, fpx, NULL); }

#endif /* RSX_EXT_STATE_H */
