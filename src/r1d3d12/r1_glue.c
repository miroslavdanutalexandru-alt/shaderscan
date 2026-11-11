/* shaderscan r1d3d12 back end glue. SPDX-License-Identifier: GPL-2.0-only
 *
 * Verbatim copies of the post-processing statics of the R1 port's ps3recomp/libs/video/rsx_d3d12_backend.c
 * (hlsl_replace_all, fp_str_replace, fp_apply_clamp_samplers, fp_unify_b1, s_gf_hlsl_helpers, gf_make_text and
 * the inline unnorm/mirror blocks of vp_get_fp_pso), arranged in the order vp_get_fp_pso applies them. If the
 * runtime changes one of these, `shaderscan --verify <R1 shadercache>` reports key mismatches; recopy then. */
#include "r1_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

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
"#ifndef RXV\n#define RXV 0.0\n#endif\n"
"MVT main(MVOut i) {\n"
"    MVT o; o.rx = RXV; o.z = i.prv.z > 1e29 ? i.cur.z : i.pos.z;\n"
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

char* r1_mv_vs_text(const char* base)
{
    static const char sigA[] = "VSOutput main(VSInput input) {";
    static const char sigB[] = "VSOutput main(uint _vid : SV_VertexID) {";
    const char* m = strstr(base, "VSOutput main(");
    if (!m || strstr(base, "rsx_vtex")) return NULL;
    const int gfv = !strncmp(m, sigB, sizeof sigB - 1);
    if (!gfv && strncmp(m, sigA, sizeof sigA - 1)) return NULL;
    const size_t pre = (size_t)(m - base), fn = strlen(m);
    static const char* f1[] = { "VSOutput main(", "uint _vid : SV_VertexID" };
    static const char* t1[] = { "VSOutput mv_cur(", "uint _vid" };
    static const char* f2[] = { "VSOutput main(", "uint _vid : SV_VertexID", "vp_c[", "vp_posscale", "vp_posoffset" };
    static const char* t2[] = { "VSOutput mv_prv(", "uint _vid", "vp_cp[", "vp_pps", "vp_ppo" };
    char* c1 = mv_subst(m, fn, f1, t1, 2);
    char* c2 = mv_subst(m, fn, f2, t2, 5);
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
    "    if (mv_k.w > 0.5) { O.cur.z = mv_k.z; O.prv.z = 1e30; } else O.prv.z = 0.0; return O; }\n";
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

/* ---- rsx_d3d12_backend.c: shc_hash / shc_compile_ex key ------------------------------------------------------ */
static uint64_t shc_hash(const char* a, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= (u8)a[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t r1_shc_key(const char* target, const char* entry, const char* src, size_t len)
{
    uint64_t h = 14695981039346656037ull;
    h = shc_hash("rfom-shc-v1", 11, h);
    h = shc_hash(target, strlen(target) + 1, h);
    h = shc_hash(entry, strlen(entry) + 1, h);
    return shc_hash(src, len, h);
}
