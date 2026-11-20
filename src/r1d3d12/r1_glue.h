/* shaderscan r1d3d12 back end: the Resistance: Fall of Man D3D12 renderer's shader text pipeline.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The RSX->HLSL translators themselves (rsx_fp_decompiler.c / rsx_vp_decompiler.c) are compiled straight from
 * third_party/rfom_translators (a copy of the R1 port's ps3recomp/libs/video; SHADERSCAN_R1_VIDEO_DIR  overrides it
  * with a checkout of the port). r1_glue.c holds the few post-processing steps that live as statics inside its rsx_d3d12_backend.c, copied verbatim. `shaderscan --verify` on R1's shadercache re-derives
 * every recorded key, so any drift between the copy and the runtime shows up as mismatches. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One fragment-program compile state: everything vp_get_fp_pso feeds into the HLSL besides the ucode. */
typedef struct r1_fp_variant {
    uint8_t exp32;               /* SET_SHADER_CONTROL 0x40 */
    uint8_t pad[3];
    uint32_t cube_mask;
    uint8_t shadow[16][4];       /* rsx_fp_shadow_desc: enabled, func, linear, unnormalized */
    uint8_t expand[16];
    uint32_t unnorm_mask;
    uint32_t unnorm_dim[16][2];
    uint32_t unnorm_decl[16][2];
    uint32_t clamp_mask;
    uint32_t mirror_mask;
    uint8_t mirror[16];
    /* rsx_ext_state.h fpx bits besides fog (RSX_FOG applies to every draw): two-sided colours (8), flat shading (16),
     * border colour per unit (U axis bit 8+u, V axis bit 32+u), and the mirror flags rsx_sampb folds in per unit */
    uint64_t fpx;
    uint8_t bmirror[16];
} r1_fp_variant;

/* Fragment program -> HLSL exactly as vp_get_fp_pso builds it (debug hooks off). ucode is guest memory
 * (big-endian, half-word swapped) and must stay readable for 4096 bytes. Returns instructions or <= 0. */
int r1_fp_hlsl(const uint8_t* ucode, const r1_fp_variant* v, char* out, uint32_t cap);
/* Vertex program -> HLSL as vp_get_vs_uncached: 'le_words' = host-order (little-endian) transform words from the
 * start slot. Returns instructions or <= 0. */
int r1_vp_hlsl(const uint8_t* le_words, uint32_t bytes, char* out, uint32_t cap);
/* The GPU-fetch vertex shader text (gf_make_text). malloc'd, NULL when the program has no VSInput main. */
char* r1_gf_text(const char* hlsl, size_t* olen);
/* rsx_ext_state.h rsx_vsx_patch: the clip-plane (1) / point-sprite (2) / back-colour (4) VS variant of a decompiled
 * program, in place. Returns 0, or -1 (anchors missing: the runtime then uses the plain program). */
int r1_vsx_patch(char* hlsl, uint32_t cap, uint32_t flags);
/* rsx_fsr.inc mv_make_vs_text: the motion-vector VS built from a VS text (plain or GPU-fetch) when FSR/DLSS/XeSS
 * is on (RSX_FSR != 0). prune: drop what cannot reach the position (mv_prune_main, RSX_FSR_MV_PRUNE); *pruned = 1
 * when that changed the text. The runtime first looks the unpruned text up in the cache, so that is the one to seed.
 * malloc'd, NULL when the runtime makes none (vertex texture fetch, unknown main). */
char* r1_mv_vs_text(const char* base, int prune, int* pruned);
/* rsx_fsr.inc s_mv_ps_hlsl ("fsr_mv_ps") and, with reactive >= 0, its "#define RXV %.4f" form ("fsr_rx_ps").
 * Returns the length written to out (0 when cap is too small). */
size_t r1_mv_ps_text(float reactive, char* out, size_t cap);
/* shadercache\<key>.dxbc key: FNV-1a 64 over "rfom-shc-v1" ("rfom-shc-v1s" for a vertex shader compiled
 * IEEE-strict, RSX_VS_STRICT), target\0, entry\0, source (always the plain text). */
uint64_t r1_shc_key(const char* target, const char* entry, const char* src, size_t len, int strict);
/* RSX_VS_STRICT (rsx_d3d12_backend.c shc_strict_text): the text FXC compiles with D3DCOMPILE_IEEE_STRICTNESS
 * instead of src (malloc'd, *olen = length), or NULL: not a decompiler VS, which then compiles and keys as usual. */
char* r1_vs_strict_text(const char* src, size_t len, size_t* olen);

#define R1_HLSL_CAP (256u * 1024u)

#ifdef __cplusplus
}
#endif
