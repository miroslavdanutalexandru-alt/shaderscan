# Changelog

## v1.0.2 (2026-11-23)

`rpcs3` back end (Resistance 2 / 3): fragment variants for the two control bits RPCS3 sets from draw state.

- `EMULATE_DEPTH_COMPARE` (0x08000000, polygon draws with depth func EQUAL) and `TEXTURE_FORMAT_CONVERT`
  (0x04000000, a bound texture needs a format remap) give one program several fragment keys. Templates of the
  profile that differ from a chosen one only in these bits are now added, with their own learned pipeline state:
  depth-compare ones always, format-convert ones when used at least half as often as the least used chosen template.
  Embedded profiles: R2 2382 -> 3108 fragment variants, recorded pipelines with a generated fragment state 83 -> 91 %;
  R3 6311 -> 9268, 53 -> 73 % (`--variants 9` alone gets 67 % for the same count).
- The stamp gets a `ctrlvar 1` line. `--check` asks for a rescan of R2/R3 caches written without it (it only adds
  the new entries). `SHADERSCAN_VERSION` stays 1, so R1 (`r1d3d12`) caches are not rescanned.

## v1.0.1 (2026-11-20)

Matches the shader text and keys of Resistance: Fall of Man port v0.3.0-beta. The R2/R3 (`rpcs3`) back end is
unchanged: the ports' RPCS3 cache format and program hashing did not change, so `SHADERSCAN_VERSION` stays 1 and R2/R3
caches are not rescanned.

- `r1d3d12` motion-vector shaders (`RSX_FSR` > 0: FSR / DLSS / XeSS): the PS now uses v0.3.0's text (the `MVK`
  constant buffer, so the draw's own depth range applies, and the negative depth markers for unmatched / ambiguous
  draws). The VS uses the text the port has used since 0.2.0. v1.0.0 seeded the pre-0.2.0 text, keys the game never
  asks for, so every motion shader was still compiled while playing (1-15 s each on a 6-core). A recorded pruned
  motion VS (`RSX_FSR_MV_PRUNE`) counts as covered; the game looks up the full text first, and that is what is seeded.
- `r1d3d12` IEEE-strict vertex shaders (`RSX_VS_STRICT`): when it is on, VS keys use the port's `rfom-shc-v1s` tag
  and are compiled with `D3DCOMPILE_IEEE_STRICTNESS` from the port's strict text. As in the port, it defaults to on
  unless the GPU is NVIDIA. shaderscan picks the adapter the way the game does (high-performance hardware adapter, or
  `CELLMARK_IGPU`) without creating anything on it.
- `r1d3d12` fragment state: the port's fog, flat shading, two-side lighting and border-colour patch
  (`RSX_FOG`, `RSX_FLAT_SHADE`, `RSX_TWO_SIDE_LIGHT`, `RSX_BORDER_COLOR`) is applied. Flat, two-side and border
  variants are learned from a recorded cache. The vertex shader extension variants are seeded for programs a recorded
  cache shows them for. The point-sprite VP gets no `SV_VertexID` form, as in the game.
- `r1d3d12` translator refresh: `rsx_fp_decompiler.*` from the port's v0.3.0-beta (extended ops, `RSX_FP_EXTOPS` /
  `RSX_FP_RFL`) and the port's new `rsx_ext_state.h` (MIT; parts adapted from GoW-Ascension-Recomp, MIT, whose
  licence is now in `third_party\rfom_translators\LICENSE.gow-ascension` and `THIRD_PARTY_NOTICES.md`) are vendored.
- `r1d3d12` environment fingerprint: also covers `RSX_FP_EXTOPS`, `RSX_FP_RFL`, `RSX_FOG`, `RSX_FLAT_SHADE`,
  `RSX_TWO_SIDE_LIGHT`, `RSX_BORDER_COLOR` and `RSX_VS_STRICT`.
- `r1d3d12` stamp: new lines `mvtext 3` and `vsstrict 0|1`. `--check` asks for one rescan when they are missing or
  differ (for example after a GPU change between NVIDIA and AMD / Intel). Only the new shaders are compiled;
  everything else is already present.
- `--verify` (`r1d3d12`): re-derives `rfom-shc-v1s` keys and recompiles them strictly, and accepts the port's
  `cs_5_0` DLSS camera-fill shader.

Checked against a local v0.3.0-beta cache (2679 shaders): `--verify` finds 0 disagreeing keys and 2368 byte-identical
recompiles (0 differ, 0 fail). A scan reproduces both current motion PS and all 127 current-text motion VS of that
cache. A strict scan (`RSX_VS_STRICT=1`) verifies with 684 strict VS, all byte-identical.

## v1.0.0 (2026-11-11)

First standalone release. Before this, shaderscan lived in `ps3recomp\tools\shaderscan` of each Resistance port
and was built from source by each port's `build.bat`.

- Seeds a recompiled game's shader cache offline from the user's own game files, with no playthrough.
- Back ends:
  - `rpcs3`: RPCS3's Vulkan shader cache (`raw\*.fp` / `*.vp` plus `pipelines\vulkan\v1.95\*.bin`). Used by
    Resistance 2 and Resistance 3.
  - `r1d3d12`: the Resistance: Fall of Man port's own D3D12 renderer (`shadercache\<key>.hlsl` / `.dxbc`, key
    tag `rfom-shc-v1`). The port's RSX to HLSL translators are vendored under `third_party\rfom_translators`, so
    the release exe needs no ResistancePort checkout.
- Front ends: `insomniac` (EBOOT program headers plus R3 `shaders.dat` level data, by index tables and byte scan)
  and `signature` (engine-agnostic research aid).
- Embedded state profiles for R1, R2 and R3; templates are learned from an existing recorded cache.
- `--verify` re-derives every key of an existing cache the way the renderer loads it.
- Launcher contract: `--check`, `--progress` (`@progress` / `@done` / `@error`), exit codes 0 / 1 / 2 / 3, atomic
  writes and stamp last.
- New: `--version` prints the release, the cache stamp version (`SHADERSCAN_VERSION 1`) and the built-in back ends.
- The release exe links the C runtime statically (no VC++ redistributable needed).

Cache compatibility: `SHADERSCAN_VERSION` is still 1, so stamps written by the port-built tool stay valid and the
launchers do not rescan. Output is byte-identical to the last port-built tool (checked on R2 and R3; R1 `--verify`
is unchanged).
