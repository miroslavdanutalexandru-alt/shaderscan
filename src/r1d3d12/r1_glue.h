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
} r1_fp_variant;

/* Fragment program -> HLSL exactly as vp_get_fp_pso builds it (debug hooks off). ucode is guest memory
 * (big-endian, half-word swapped) and must stay readable for 4096 bytes. Returns instructions or <= 0. */
int r1_fp_hlsl(const uint8_t* ucode, const r1_fp_variant* v, char* out, uint32_t cap);
/* Vertex program -> HLSL as vp_get_vs_uncached: 'le_words' = host-order (little-endian) transform words from the
 * start slot. Returns instructions or <= 0. */
int r1_vp_hlsl(const uint8_t* le_words, uint32_t bytes, char* out, uint32_t cap);
/* The GPU-fetch vertex shader text (gf_make_text). malloc'd, NULL when the program has no VSInput main. */
char* r1_gf_text(const char* hlsl, size_t* olen);
/* rsx_fsr.inc mv_make_vs_text: the motion-vector VS built from a VS text (plain or GPU-fetch) when FSR/DLSS/XeSS
 * is on (RSX_FSR != 0). malloc'd, NULL when the runtime makes none (vertex texture fetch, unknown main). */
char* r1_mv_vs_text(const char* base);
/* rsx_fsr.inc s_mv_ps_hlsl ("fsr_mv_ps") and, with reactive >= 0, its "#define RXV %.4f" form ("fsr_rx_ps").
 * Returns the length written to out (0 when cap is too small). */
size_t r1_mv_ps_text(float reactive, char* out, size_t cap);
/* shadercache\<key>.dxbc key: FNV-1a 64 over "rfom-shc-v1", target\0, entry\0, source. */
uint64_t r1_shc_key(const char* target, const char* entry, const char* src, size_t len);

#define R1_HLSL_CAP (256u * 1024u)

#ifdef __cplusplus
}
#endif
