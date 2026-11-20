/* shaderscan r1d3d12 back end glue. SPDX-License-Identifier: GPL-2.0-only
 *
 * Verbatim copies of the post-processing statics of the R1 port's ps3recomp/libs/video/rsx_d3d12_backend.c
 * (hlsl_replace_all, fp_str_replace, fp_apply_clamp_samplers, fp_unify_b1, s_gf_hlsl_helpers, gf_make_text,
 * shc_strict_text, the inline unnorm/mirror blocks of vp_get_fp_pso and its rsx_fpx_patch_m call) and of rsx_fsr.inc
 * (the motion-vector shaders and mv_prune_main), arranged in the order the runtime applies them. rsx_ext_state.h
 * (fog / flat / two-side / border FP patch, VS variants, strict VS text) is the port's own header, vendored whole. If the
 * runtime changes one of these, `shaderscan --verify <R1 shadercache>` reports key mismatches; recopy then. */
#include "r1_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_ext_state.h"

extern int g_rsx_fp_kslot_mode;

/* ---- rsx_d3d12_backend.c: hlsl_replace_all ------------------------------------------------------------------ */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* ---- rsx_d3d12_backend.c: fp_str_replace / fp_apply_clamp_samplers ------------------------------------------ */
static int fp_str_replace(char* h, size_t cap, const char* from, const char* to)
{
    int n = 0;
    const size_t lf = strlen(from), lt = strlen(to);
    for (char* q = strstr(h, from); q; q = strstr(q + lt, from)) {
        const size_t len = strlen(h);
        if (len + lt - lf + 1 > cap) return -1;
        memmove(q + lt, q + lf, len - (size_t)(q - h) - lf + 1);
        memcpy(q, to, lt);
        n++;
    }
    return n;
}
static int fp_apply_clamp_samplers(char* h, size_t cap, u32 mask)
{
    int n = 0;
    if (fp_str_replace(h, cap, "SamplerState rsx_samp[16] : register(s0);",
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float4 rsx_sampc(Texture2D t, SamplerState s, float2 uv) { uint w, h; t.GetDimensions(w, h);"
            " float2 hp = 0.5 / float2(max(w, 1u), max(h, 1u)); return t.Sample(s, clamp(uv, hp, 1.0 - hp)); }") != 1) return -1;
    for (u32 u = 0; u < 16; u++) {
        if (!(mask & (1u << u))) continue;
        char from[64], to[64], decl[48];
        snprintf(from, sizeof from, "rsx_tex[%u].Sample(rsx_samp[%u], ", u, u);
        snprintf(to, sizeof to, "rsx_sampc(rsx_tex[%u], rsx_samp[%u], ", u, u);
        int k = fp_str_replace(h, cap, from, to);
        if (k < 0) return -1;
        n += k;
        snprintf(decl, sizeof decl, "Texture2D   rsx_tex%u :", u);   /* separate 2D declaration */
        if (strstr(h, decl)) {
            snprintf(from, sizeof from, "rsx_tex%u.Sample(rsx_samp[%u], ", u, u);
            snprintf(to, sizeof to, "rsx_sampc(rsx_tex%u, rsx_samp[%u], ", u, u);
            k = fp_str_replace(h, cap, from, to);
            if (k < 0) return -1;
            n += k;
        }
    }
    return n;
}

/* ---- rsx_d3d12_backend.c: fp_unify_b1 ----------------------------------------------------------------------- */
static void fp_unify_b1(char* hlsl, size_t cap)
{
    if (!strstr(hlsl, "fp_k[")) return;
    static const char* const forms[] = {
        "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[69]; float4 rsx_shscale; };\n",
        "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[4]; float4 rsx_alphatest; float4 _rsx_b1pad2[64]; float4 rsx_shscale; };\n",
        "cbuffer RsxAlphaTest : register(b1) { float4 _rsx_atpad[4]; float4 rsx_alphatest; };\n",
    };
    static const char uni[] =
        "cbuffer RsxB1 : register(b1) { float4 rsx_texscale[4]; float4 rsx_alphatest; float4 fp_k[64]; float4 rsx_shscale; };\n";
    size_t first_off = (size_t)-1;
    for (int f = 0; f < 3; f++) {
        char* at;
        while ((at = strstr(hlsl, forms[f])) != NULL) {
            const size_t n = strlen(forms[f]);
            if ((size_t)(at - hlsl) < first_off) first_off = (size_t)(at - hlsl);
            memmove(at, at + n, strlen(at + n) + 1);
        }
    }
    char* ins = NULL;
    const char* marks[] = { "struct PSOutput", "float4 main(", "PSOutput main(" };
    for (int m = 0; m < 3; m++) { char* q = strstr(hlsl, marks[m]); if (q && (!ins || q < ins)) ins = q; }
    if (first_off != (size_t)-1 && first_off <= strlen(hlsl) && (!ins || hlsl + first_off < ins)) ins = hlsl + first_off;
    if (!ins) return;
    const size_t len = strlen(hlsl), ul = sizeof uni - 1;
    if (len + ul + 1 > cap) return;
    memmove(ins + ul, ins, strlen(ins) + 1);
    memcpy(ins, uni, ul);
}

/* ---- rsx_d3d12_backend.c: vp_get_fp_pso (decompile .. fp_unify_b1, debug env hooks omitted) ------------------- */
int r1_fp_hlsl(const uint8_t* ucode, const r1_fp_variant* v, char* hlsl, uint32_t cap)
{
    const u32 fp_ctrl = v->exp32 ? 0x40u : 0u;   /* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS */
    rsx_fp_shadow_desc shadow[16];
    int anyx = 0, anys = 0;
    for (int u = 0; u < 16; u++) {
        shadow[u].enabled = v->shadow[u][0]; shadow[u].func = v->shadow[u][1];
        shadow[u].linear = v->shadow[u][2]; shadow[u].unnormalized = v->shadow[u][3];
        anyx |= v->expand[u]; anys |= shadow[u].enabled;
    }
    { static int ks = -1; if (ks < 0) { const char* e = getenv("RSX_FP_KSLOTS"); ks = e ? atoi(e) : 1; }
      g_rsx_fp_kslot_mode = ks; }
    int n = anys ? rsx_fp_decompile_shadow_expand_ex(ucode, 4096, fp_ctrl, 0u, shadow, v->expand, hlsl, cap)
          : anyx ? rsx_fp_decompile_expand_ex(ucode, 4096, fp_ctrl, 0u, v->expand, hlsl, cap)
                 : rsx_fp_decompile(ucode, 4096, fp_ctrl, hlsl, cap);
    g_rsx_fp_kslot_mode = 0;
    if (n <= 0) return n;
    const u32 cube_mask = v->cube_mask;
    if (v->unnorm_mask) {
        for (u32 u = 0; u < 16; u++) {
            if (!(v->unnorm_mask & (1u << u)) || !v->unnorm_decl[u][0]) continue;
            char nd[48]; snprintf(nd, sizeof nd, "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
            for (char* at = strstr(hlsl, nd); at; at = strstr(at + 1, nd)) {
                char* arg = at + strlen(nd);
                char* open = strchr(at, '('); int depth = 0; char* q = open;
                for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')') { if (!--depth) break; } }
                if (*q != ')') break;
                char suf[96]; snprintf(suf, sizeof suf, "), float2(0.5, 0.5), float2(%.1f, %.1f))",
                                       (double)v->unnorm_decl[u][0] - 0.5, (double)v->unnorm_decl[u][1] - 0.5);
                static const char pre[] = " clamp((";
                const size_t sl = strlen(suf), pl = sizeof pre - 1, len = strlen(hlsl);
                if (len + sl + pl + 1 > cap) break;
                memmove(q + sl, q, strlen(q) + 1); memcpy(q, suf, sl);
                memmove(arg + pl, arg, strlen(arg) + 1); memcpy(arg, pre, pl);
            }
        }
        rsx_fp_apply_unnorm_scale(hlsl, cap, v->unnorm_mask, (const u32 (*)[2])v->unnorm_dim, cube_mask);
    }
    if (v->clamp_mask) fp_apply_clamp_samplers(hlsl, cap, v->clamp_mask);
    if (v->mirror_mask) {
        if (fp_str_replace(hlsl, cap, "SamplerState rsx_samp[16] : register(s0);",
                "SamplerState rsx_samp[16] : register(s0);\n"
                "float4 rsx_sampm(Texture2D t, SamplerState s, float2 uv, float4 m) {"
                " float2 mr = 1.0 - abs((uv - 2.0 * floor(uv * 0.5)) - 1.0); float2 mo = saturate(abs(uv));"
                " float2 a = uv; a = (m.xy > 0.5) ? mr : a; a = (m.zw > 0.5) ? mo : a; return t.Sample(s, a); }") == 1) {
            for (u32 u = 0; u < 16; u++) {
                if (!(v->mirror_mask & (1u << u))) continue;
                char nd[48]; snprintf(nd, sizeof nd, "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
                const u8 f = v->mirror[u];
                char tail[64]; snprintf(tail, sizeof tail, ", float4(%d, %d, %d, %d))", f & 1, (f >> 1) & 1, (f >> 2) & 1, (f >> 3) & 1);
                char rpl[64]; snprintf(rpl, sizeof rpl, "rsx_sampm(rsx_tex[%u], rsx_samp[%u],", u, u);
                const size_t tl = strlen(tail), rl = strlen(rpl), nl = strlen(nd);
                for (char* at = strstr(hlsl, nd); at; at = strstr(hlsl, nd)) {
                    char* open = strchr(at, '('); int depth = 0; char* q = open;
                    for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')') { if (!--depth) break; } }
                    if (*q != ')') break;
                    if (strlen(hlsl) + tl + rl + 4 > cap) break;
                    memmove(q + tl, q + 1, strlen(q + 1) + 1); memcpy(q, tail, tl);
                    memmove(at + rl, at + nl, strlen(at + nl) + 1); memcpy(at, rpl, rl);
                }
            }
        }
    }
    { static int at = -1; if (at < 0) { const char* e = getenv("RSX_ALPHATEST"); at = e ? atoi(e) : 1; }
      if (at) rsx_fp_apply_alpha_test_dynamic(hlsl, cap); }
    if (cube_mask) {
        for (int _u = 0; _u < 16 /* RSX_UNITS */; _u++) {
            if (!(cube_mask & (1u << _u))) continue;
            char f1[64], r1[64], f2[64], r2[64];
            snprintf(f1, sizeof f1, "Texture2D    rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube  rsx_tex%d", _u);
            hlsl_replace_all(hlsl, cap, f1, r1);
            snprintf(f1, sizeof f1, "Texture2D rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube rsx_tex%d", _u);
            hlsl_replace_all(hlsl, cap, f1, r1);
            snprintf(f2, sizeof f2, ".xy * rsx_texscale[%d].xy", _u);
            snprintf(r2, sizeof r2, ".xyz");
            hlsl_replace_all(hlsl, cap, f2, r2);
        }
    }
    fp_unify_b1(hlsl, cap);
    /* fp_set_unnorm / draw record: RSX_FOG on every draw, the other bits per draw state and their own switches;
     * a failed patch leaves the text unchanged (as the runtime) */
    {
        static int fog = -1, flat = -1, two = -1, border = -1;
        if (fog < 0) {
            const char* e;
            e = getenv("RSX_FOG");            fog = e ? atoi(e) : 1;
            e = getenv("RSX_FLAT_SHADE");     flat = e ? atoi(e) : 1;
            e = getenv("RSX_TWO_SIDE_LIGHT"); two = e ? atoi(e) : 1;
            e = getenv("RSX_BORDER_COLOR");   border = e ? atoi(e) : 1;
        }
        uint64_t x = fog ? RSX_FPX_FOG : 0u;
        if (flat) x |= v->fpx & RSX_FPX_FLAT;
        if (two) x |= v->fpx & RSX_FPX_TWOSIDE;
        if (border) x |= v->fpx & ((0xFFFFull << RSX_FPX_BORDER_SHIFT) | (0xFFFFull << RSX_FPX_BORDERV_SHIFT));
        if (x) rsx_fpx_patch_m(hlsl, cap, x, v->bmirror);
    }
    return n;
}

/* ---- rsx_d3d12_backend.c: vp_get_vs_uncached --------------------------------------------------------------------- */
int r1_vp_hlsl(const uint8_t* vuc, uint32_t vlen, char* hlsl, uint32_t cap)
{
    const u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
    if (instrs) vlen = instrs * 16u;
    return rsx_vp_decompile(vuc, vlen, hlsl, cap);
}

/* ---- rsx_d3d12_backend.c: s_gf_hlsl_helpers / gf_make_text --------------------------------------------------- */
static const char s_gf_hlsl_helpers[] =
"ByteAddressBuffer vp_raw : register(t30);\n"   /* vs_5_0: no register spaces */
"uint vp_b8(uint a) { return (vp_raw.Load(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu; }\n"
"uint vp_be32(uint a) {\n"
"    uint al = a & ~3u, sh = (a & 3u) * 8u;\n"
"    uint w = vp_raw.Load(al);\n"
"    if (sh != 0u) w = (w >> sh) | (vp_raw.Load(al + 4u) << (32u - sh));\n"
"    return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24);\n"
"}\n"
"uint vp_be16(uint a) { return (vp_b8(a) << 8) | vp_b8(a + 1u); }\n"
"float4 vp_gf(uint i, uint vid) {\n"
"    uint4 d = vp_gfa[i]; float4 r = vp_gfc[i];\n"
"    if ((d.z >> 16) != 1u) return r;\n"
"    uint ty = d.z & 0xFFu, n = (d.z >> 8) & 0xFFu;\n"
"    uint ei = (vid + vp_gfx.x) & 0xFFFFFu;\n"
"    uint a = (uint)((int)d.x + (int)(ei * d.y));\n"
"    if (ty == 6u) {\n"
"        uint w = vp_be32(a);\n"
"        int x = (int)(w << 21) >> 21, y = (int)(w << 10) >> 21, z = (int)w >> 22;\n"
"        return float4((float)x / 1023.0, (float)y / 1023.0, (float)z / 511.0, 1.0);\n"
"    }\n"
"    float c[4] = { r.x, r.y, r.z, r.w };\n"
"    [unroll] for (uint k = 0u; k < 4u; k++) {\n"
"        if (k < n) {\n"
"            float v;\n"
"            if (ty == 2u)      v = asfloat(vp_be32(a + 4u * k));\n"
"            else if (ty == 3u) v = f16tof32(vp_be16(a + 2u * k));\n"
"            else if (ty == 4u) v = (float)vp_b8(a + k) * (1.0 / 255.0);\n"
"            else if (ty == 1u) v = (float)((int)(vp_be16(a + 2u * k) << 16) >> 16) / 32767.0;\n"
"            else if (ty == 5u) v = (float)((int)(vp_be16(a + 2u * k) << 16) >> 16);\n"
"            else               v = (float)vp_b8(a + k);\n"
"            c[k] = v;\n"
"        }\n"
"    }\n"
"    return float4(c[0], c[1], c[2], c[3]);\n"
"}\n";

char* r1_gf_text(const char* hlsl, size_t* olen)
{
    if (!hlsl) return NULL;
    static const char sig[] = "VSOutput main(VSInput input) {";
    const char* m = strstr(hlsl, sig);
    if (!m) return NULL;
    const size_t cap = strlen(hlsl) * 2 + sizeof s_gf_hlsl_helpers + 4096;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, hlsl, (size_t)(m - hlsl)); o = (size_t)(m - hlsl);
    memcpy(out + o, s_gf_hlsl_helpers, sizeof s_gf_hlsl_helpers - 1); o += sizeof s_gf_hlsl_helpers - 1;
    static const char nsig[] = "VSOutput main(uint _vid : SV_VertexID) {";
    memcpy(out + o, nsig, sizeof nsig - 1); o += sizeof nsig - 1;
    for (const char* p = m + sizeof sig - 1; *p && o + 64 < cap; ) {
        if (p[0] == 'i' && !strncmp(p, "input.a", 7) && p[7] >= '0' && p[7] <= '9') {
            const char* q = p + 7; unsigned idx = 0;
            while (*q >= '0' && *q <= '9') idx = idx * 10 + (unsigned)(*q++ - '0');
            o += (size_t)snprintf(out + o, cap - o, "vp_gf(%uu, _vid)", idx);
            p = q;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    if (olen) *olen = o;
    return out;
}

/* ---- rsx_fsr.inc: s_mv_ps_hlsl / mv_init, mv_subst, mv_make_vs_text ------------------------------------------ */
static const char s_mv_ps_hlsl[] =
"struct MVOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prv:TEXCOORD1; };\n"
"struct MVT { float2 mv:SV_Target0; float z:SV_Target1; float rx:SV_Target2; };\n"
"cbuffer MVK : register(b4) { float4 mv_k; };\n"
"#ifndef RXV\n#define RXV 0.0\n#endif\n"
"MVT main(MVOut i) {\n"
"    MVT o; o.rx = RXV; o.z = i.prv.z > 1e29 ? i.cur.z : i.prv.z < -0.5 ? (mv_k.w < -1.5 ? -3.0 : -1.0) - saturate(i.pos.z) : i.pos.z;\n"
"    if (abs(i.prv.w) < 1e-6 || abs(i.cur.w) < 1e-6) { o.mv = float2(0, 0); return o; }\n"
"    float2 c = i.cur.xy / i.cur.w, p = i.prv.xy / i.prv.w;\n"
"    o.mv = (p - c) * float2(0.5, -0.5);\n"
"    return o;\n"
"}\n";

size_t r1_mv_ps_text(float reactive, char* out, size_t cap)
{
    if (reactive < 0.0f) {
        if (cap < sizeof s_mv_ps_hlsl) return 0;
        memcpy(out, s_mv_ps_hlsl, sizeof s_mv_ps_hlsl);
        return sizeof s_mv_ps_hlsl - 1;
    }
    const int k = snprintf(out, cap, "#define RXV %.4f\n%s", reactive, s_mv_ps_hlsl);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

static char* mv_subst(const char* s, size_t n, const char* const* from, const char* const* to, int k)
{
    size_t cap = n * 2 + 1024, o = 0;
    char* out = (char*)malloc(cap); if (!out) return NULL;
    for (size_t i = 0; i < n; ) {
        int hit = -1;
        for (int q = 0; q < k; q++) { size_t L = strlen(from[q]); if (i + L <= n && !memcmp(s + i, from[q], L)) { hit = q; break; } }
        if (o + 64 + (hit >= 0 ? strlen(to[hit]) : 1) >= cap) { cap *= 2; char* t = (char*)realloc(out, cap); if (!t) { free(out); return NULL; } out = t; }
        if (hit >= 0) { size_t L = strlen(to[hit]); memcpy(out + o, to[hit], L); o += L; i += strlen(from[hit]); }
        else out[o++] = s[i++];
    }
    out[o] = 0;
    return out;
}

/* ---- rsx_fsr.inc: mv_prune_main and helpers (RSX_FSR_MV_PRUNE), mv_make_vs_text --------------------------- */
#define MVP_REGS 64
typedef struct { const char* p; size_t n; u8 isv, ndef; u8 dk[8], di[8], dm[8], dc[8]; u64 use[5]; } MvpStmt;
static int mvp_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int mvp_lit(const char* s, const char* e, const char* lit)
{
    const size_t L = strlen(lit);
    return (size_t)(e - s) >= L && !memcmp(s, lit, L);
}
/* register reads in [s, e): r o v cc (constant index) and a0/a1. 0 = not modelled. */
static int mvp_uses(const char* s, const char* e, u64 use[5])
{
    static const char* const pre[4] = { "cc[", "r[", "o[", "v[" };
    static const int kind[4] = { 3, 0, 1, 2 };
    for (const char* p = s; p < e; p++) {
        if (p > s && mvp_ident(p[-1])) continue;
        if (*p == 'a' && p + 1 < e && (p[1] == '0' || p[1] == '1') && (p + 2 >= e || !mvp_ident(p[2]))) {
            use[4] |= 1ull << (p[1] - '0'); p++; continue;
        }
        for (int k = 0; k < 4; k++) {
            if (!mvp_lit(p, e, pre[k])) continue;
            const char* q = p + strlen(pre[k]);
            int idx = 0, nd = 0;
            while (q < e && *q >= '0' && *q <= '9' && nd < 3) { idx = idx * 10 + (*q - '0'); q++; nd++; }
            if (!nd || q >= e || *q != ']' || idx >= MVP_REGS) return 0;
            use[kind[k]] |= 1ull << idx;
            p = q;
            break;
        }
    }
    return 1;
}
static int mvp_mask(const char** pp, const char* e, u8* m)
{
    const char* p = *pp; *m = 0;
    while (p < e && (*p == 'x' || *p == 'y' || *p == 'z' || *p == 'w')) {
        *m |= (u8)(*p == 'x' ? 1 : *p == 'y' ? 2 : *p == 'z' ? 4 : 8); p++; }
    if (p == *pp || p - *pp > 4) return 0;
    *pp = p; return 1;
}
/* "[if (COND) ](r|o|cc)[N].MASK = _v.MASK;" */
static int mvp_def(const char* s, const char* e, MvpStmt* st)
{
    u8 cond = 0;
    if (mvp_lit(s, e, "if (")) {
        const char* q = NULL;
        for (const char* p = s; p + 1 < e; p++) if (p[0] == ')' && p[1] == ' ') q = p;
        if (!q) return 0;
        if (!mvp_uses(s + 4, q, st->use)) return 0;
        s = q + 2; cond = 1;
    }
    int k;
    if (mvp_lit(s, e, "cc[")) { k = 3; s += 3; } else if (mvp_lit(s, e, "r[")) { k = 0; s += 2; }
    else if (mvp_lit(s, e, "o[")) { k = 1; s += 2; } else return 0;
    int idx = 0, nd = 0;
    while (s < e && *s >= '0' && *s <= '9' && nd < 3) { idx = idx * 10 + (*s - '0'); s++; nd++; }
    if (!nd || idx >= MVP_REGS || !mvp_lit(s, e, "].")) return 0;
    s += 2;
    u8 m, m2;
    if (!mvp_mask(&s, e, &m) || !mvp_lit(s, e, " = _v.")) return 0;
    s += 6;
    if (!mvp_mask(&s, e, &m2) || e - s != 1 || *s != ';' || st->ndef >= 8) return 0;
    st->dk[st->ndef] = (u8)k; st->di[st->ndef] = (u8)idx; st->dm[st->ndef] = m; st->dc[st->ndef] = cond; st->ndef++;
    return 1;
}
/* m: "VSOutput main(...) {" through the end of the text. malloc'd pruned
 * function, NULL = keep the original (not the modelled form, or nothing to drop). */
static char* mv_prune_main(const char* m)
{
    const size_t fn = strlen(m);
    size_t nl = 1;
    for (size_t i = 0; i < fn; i++) nl += m[i] == '\n';
    const char** ls = (const char**)malloc(nl * sizeof *ls);
    size_t* ln = (size_t*)malloc(nl * sizeof *ln);
    MvpStmt* st = (MvpStmt*)calloc(nl * 4 + 16, sizeof *st);   /* a v[] line holds up to 4 fetches */
    u8* keep = (u8*)calloc(nl * 4 + 16, 1);
    u8* head = (u8*)calloc(nl, 1);
    char* out = NULL;
    size_t L = 0, ns = 0, endi = 0;
    int ok = ls && ln && st && keep && head, only_v = 1;
    if (ok) {
        const char* p = m;
        for (size_t i = 0; i < nl; i++) {
            const char* q = strchr(p, '\n');
            ls[L] = p; ln[L] = q ? (size_t)(q - p) : strlen(p); L++;
            if (!q) break;
            p = q + 1;
        }
        ok = 0;
        for (size_t i = 1; i + 2 < L; i++) {
            const char* t = ls[i]; const char* e = t + ln[i];
            while (t < e && *t == ' ') t++;
            if (e - t == 13 && !memcmp(t, "VSOutput Out;", 13)) { endi = i; ok = 1; break; }
        }
    }
    if (ok) {   /* the position must come from o[0] alone */
        const char* t = ls[endi + 1]; const char* e = t + ln[endi + 1];
        while (t < e && *t == ' ') t++;
        const char* t2 = ls[endi + 2]; const char* e2 = t2 + ln[endi + 2];
        while (t2 < e2 && *t2 == ' ') t2++;
        ok = e - t == 17 && !memcmp(t, "float4 _p = o[0];", 17) &&
             mvp_lit(t2, e2, "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);");
    }
    static const char* const hp[] = { "float4 v[", "float4 r[", "[unroll] for (int _i", "[unroll] for (int _j",
                                       "int4 a0 = (int4)0;", "float4 cc[2];" };
    for (size_t i = 1; ok && i < endi; i++) {
        const char* t = ls[i]; const char* e = t + ln[i];
        while (t < e && *t == ' ') t++;
        while (e > t && (e[-1] == ' ' || e[-1] == '\r')) e--;
        if (t == e) continue;
        if (mvp_lit(t, e, "{ float4 _v =")) {
            MvpStmt* s = &st[ns];
            s->p = ls[i];
            if (memchr(t, '}', (size_t)(e - t)) || !mvp_uses(t, e, s->use)) { ok = 0; break; }
            size_t j = i + 1;
            for (; j < endi; j++) {
                const char* a = ls[j]; const char* b = a + ln[j];
                while (a < b && *a == ' ') a++;
                while (b > a && (b[-1] == ' ' || b[-1] == '\r')) b--;
                if (b - a == 1 && *a == '}') break;
                if (!mvp_def(a, b, s)) { ok = 0; break; }
            }
            if (!ok || j >= endi) { ok = 0; break; }
            s->n = (size_t)(ls[j] + ln[j] - s->p);
            ns++; only_v = 0; i = j;
            continue;
        }
        if (mvp_lit(t, e, "{ int4 _a = ")) {   /* "{ int4 _a = EXPR; aN.MASK = _a.MASK; }" */
            const char* q = NULL;
            for (const char* p = t; p + 4 < e; p++) if (p[0] == ';' && p[1] == ' ' && p[2] == 'a' && (p[3] == '0' || p[3] == '1') && p[4] == '.') q = p;
            MvpStmt* s = &st[ns];
            u8 m1, m2;
            const char* r = q ? q + 5 : NULL;
            if (!q || !mvp_uses(t + 12, q, s->use) || !mvp_mask(&r, e, &m1) || !mvp_lit(r, e, " = _a.")) { ok = 0; break; }
            r += 6;
            if (!mvp_mask(&r, e, &m2) || e - r != 3 || memcmp(r, "; }", 3)) { ok = 0; break; }
            s->p = ls[i]; s->n = ln[i];
            s->dk[0] = 4; s->di[0] = (u8)(q[3] - '0'); s->dm[0] = m1; s->ndef = 1;
            ns++; only_v = 0;
            continue;
        }
        if (mvp_lit(t, e, "v[")) {   /* "v[N]=EXPR;  v[N]=EXPR; ..." */
            const char* p = t;
            while (ok && p < e) {
                while (p < e && *p == ' ') p++;
                if (p >= e) break;
                const char* sc = (const char*)memchr(p, ';', (size_t)(e - p));
                if (!sc || !mvp_lit(p, sc, "v[")) { ok = 0; break; }
                const char* q = p + 2;
                int idx = 0, nd = 0;
                while (q < sc && *q >= '0' && *q <= '9' && nd < 3) { idx = idx * 10 + (*q - '0'); q++; nd++; }
                if (!nd || idx >= MVP_REGS || !mvp_lit(q, sc, "]=")) { ok = 0; break; }
                MvpStmt* s = &st[ns];
                if (!mvp_uses(q + 2, sc, s->use) || ns + 1 >= nl * 4 + 16) { ok = 0; break; }
                s->p = p; s->n = (size_t)(sc - p); s->isv = 1;
                s->dk[0] = 2; s->di[0] = (u8)idx; s->dm[0] = 15; s->ndef = 1;
                ns++;
                p = sc + 1;
            }
            continue;
        }
        int h = 0;
        for (size_t k = 0; k < sizeof hp / sizeof hp[0]; k++) if (mvp_lit(t, e, hp[k])) h = 1;
        if (!h || !only_v) { ok = 0; break; }
        head[i] = 1;
    }
    size_t kept = 0;
    if (ok) {
        u8 live[5][MVP_REGS];
        memset(live, 0, sizeof live);
        live[1][0] = 15;
        for (size_t k = ns; k-- > 0;) {
            const MvpStmt* s = &st[k];
            int any = 0;
            for (int d = 0; d < s->ndef; d++) any |= live[s->dk[d]][s->di[d]] & s->dm[d];
            if (!any) continue;
            keep[k] = 1; kept++;
            for (int d = 0; d < s->ndef; d++) if (!s->dc[d]) live[s->dk[d]][s->di[d]] &= (u8)~s->dm[d];
            for (int c = 0; c < 5; c++)
                for (int r = 0; r < MVP_REGS; r++) if (s->use[c] >> r & 1) live[c][r] = 15;
        }
        ok = kept < ns;
    }
    if (ok) {
        out = (char*)malloc(fn + ns * 8 + 16);
        if (out) {
            size_t o = 0;
            memcpy(out + o, ls[0], ln[0]); o += ln[0]; out[o++] = '\n';
            for (size_t i = 1; i < endi; i++) if (head[i]) { memcpy(out + o, ls[i], ln[i]); o += ln[i]; out[o++] = '\n'; }
            for (size_t k = 0; k < ns; k++) {
                if (!keep[k]) continue;
                if (st[k].isv) { memcpy(out + o, "    ", 4); o += 4; memcpy(out + o, st[k].p, st[k].n); o += st[k].n; out[o++] = ';'; }
                else { memcpy(out + o, st[k].p, st[k].n); o += st[k].n; }
                out[o++] = '\n';
            }
            const size_t tl = fn - (size_t)(ls[endi] - m);
            memcpy(out + o, ls[endi], tl); o += tl;
            out[o] = 0;
        }
    }
    free((void*)ls); free(ln); free(st); free(keep); free(head);
    return out;
}
/* The decompiled program (or its GPU-fetch form) -> motion-vector VS.
 * prune: drop what cannot reach the position (mv_prune_main); *pruned = 1
 * when that changed the text. */
static char* mv_make_vs_text(const char* base, int prune, int* pruned)
{
    static const char sigA[] = "VSOutput main(VSInput input) {";
    static const char sigB[] = "VSOutput main(uint _vid : SV_VertexID) {";
    const char* m = strstr(base, "VSOutput main(");
    if (!m || strstr(base, "rsx_vtex")) return NULL;     /* vertex texture fetch: not handled */
    const int gfv = !strncmp(m, sigB, sizeof sigB - 1);
    if (!gfv && strncmp(m, sigA, sizeof sigA - 1)) return NULL;
    const size_t pre = (size_t)(m - base);
    char* pm = prune ? mv_prune_main(m) : NULL;
    if (pruned) *pruned = pm != NULL;
    if (pm) m = pm;
    const size_t fn = strlen(m);
    static const char* f1[] = { "VSOutput main(", "uint _vid : SV_VertexID" };
    static const char* t1[] = { "VSOutput mv_cur(", "uint _vid" };
    static const char* f2[] = { "VSOutput main(", "uint _vid : SV_VertexID", "vp_c[", "vp_posscale", "vp_posoffset" };
    static const char* t2[] = { "VSOutput mv_prv(", "uint _vid", "vp_cp[", "vp_pps", "vp_ppo" };
    char* c1 = mv_subst(m, fn, f1, t1, 2);
    char* c2 = mv_subst(m, fn, f2, t2, 5);
    free(pm);
    if (!c1 || !c2) { free(c1); free(c2); return NULL; }
    static const char extra[] =
        "cbuffer VPPrev : register(b3) { float4 vp_cp[512]; float4 vp_pps; float4 vp_ppo; uint4 vp_pbr; };\n"
        "cbuffer MVK : register(b4) { float4 mv_k; };\n"
        "struct MVOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prv:TEXCOORD1; };\n";
    const char* tail = gfv
        ? "MVOut main(uint _vid : SV_VertexID) { VSOutput a = mv_cur(_vid); VSOutput b = mv_prv(_vid);\n"
        : "MVOut main(VSInput input) { VSOutput a = mv_cur(input); VSOutput b = mv_prv(input);\n";
    static const char tail2[] =
        "    MVOut O; O.pos = a.pos; O.cur = float4(a.pos.xy - mv_k.xy * a.pos.w, a.pos.zw); O.prv = b.pos;\n"
    "    if (mv_k.w > 0.5) { O.cur.z = mv_k.z; O.prv.z = 1e30; } else O.prv.z = mv_k.w < -0.5 ? -1.0 : 0.0; return O; }\n";
    const size_t L = pre + strlen(c1) + strlen(c2) + sizeof extra + strlen(tail) + sizeof tail2 + 16;
    char* out = (char*)malloc(L);
    if (out) {
        size_t o = 0;
        memcpy(out, base, pre); o = pre;
        o += (size_t)sprintf(out + o, "%s%s\n%s\n%s%s", extra, c1, c2, tail, tail2);
    }
    free(c1); free(c2);
    return out;
}
char* r1_mv_vs_text(const char* base, int prune, int* pruned) { return mv_make_vs_text(base, prune, pruned); }

/* ---- rsx_ext_state.h: rsx_vsx_patch ---------------------------------------------------------------------------- */
int r1_vsx_patch(char* hlsl, uint32_t cap, uint32_t flags) { return rsx_vsx_patch(hlsl, cap, flags); }

/* ---- rsx_d3d12_backend.c: shc_hash / shc_compile_ex key ------------------------------------------------------ */
static uint64_t shc_hash(const char* a, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= (u8)a[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t r1_shc_key(const char* target, const char* entry, const char* src, size_t len, int strict)
{
    uint64_t h = 14695981039346656037ull;
    h = shc_hash(strict ? "rfom-shc-v1s" : "rfom-shc-v1", strict ? 12 : 11, h);
    h = shc_hash(target, strlen(target) + 1, h);
    h = shc_hash(entry, strlen(entry) + 1, h);
    return shc_hash(src, len, h);
}

/* ---- rsx_d3d12_backend.c: shc_strict_text ------------------------------------------------------------------ */
char* r1_vs_strict_text(const char* src, size_t len, size_t* olen)
{
    char* z = (char*)malloc(len + 1u), *o = (char*)malloc(len + 64u);
    if (!z || !o) { free(z); free(o); return NULL; }
    memcpy(z, src, len); z[len] = 0;
    const int n = rsx_vs_precise_text(z, o, len + 64u);
    free(z);
    if (n < 0) { free(o); return NULL; }
    *olen = (size_t)n;
    return o;
}
