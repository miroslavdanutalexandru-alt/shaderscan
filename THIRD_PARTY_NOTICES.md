# Third-party notices

shaderscan is licensed under **GPL-2.0-only** (see `LICENSE`). Its program analysis, hashing and on-disk cache format
follow RPCS3's implementation (GPL-2.0), and the tool is a derivative of it.

The release `shaderscan.exe` is built from this repository only. It contains the components below.

| Component | Licence | Licence file | Where |
|---|---|---|---|
| RPCS3 (program analysis, ucode hashing and shader cache format, reimplemented after `ProgramStateCache.cpp` and `rsx::shaders_cache`) | GPL-2.0 | `LICENSE` | `src/rsx_program.cpp`, `src/rpcs3_cache.cpp`, `src/backend_rpcs3.cpp` |
| miniz 10.0.3 (zlib inflate for PSARC archives) by Rich Geldreich, RAD Game Tools and Valve Software | MIT | `third_party/miniz/LICENSE` | `third_party/miniz/` |
| Resistance: Fall of Man port's RSX to HLSL translators (`rsx_fp_decompiler`, `rsx_vp_decompiler`) and ps3recomp headers (`ps3types.h`, `endian.h`, `log_first.h`) | MIT | `third_party/rfom_translators/LICENSE.ps3recomp`, `third_party/rfom_translators/LICENSE.rfom-port` | `third_party/rfom_translators/` |
| Post-processing steps copied from that port's `rsx_d3d12_backend.c` and `rsx_fsr.inc` (FP state wrapper, VP forms, motion-vector shaders, cache key) | MIT (same two licences) | as above | `src/r1d3d12/r1_glue.c` |

MIT-licensed parts keep their licence and notices; the combined program is distributed under GPL-2.0-only.

## Used at run time, not included

- `d3dcompiler_47.dll` (Microsoft, part of Windows): loaded by the `r1d3d12` back end to compile HLSL the same way the
  game does.

## Game data

shaderscan contains no game data. The embedded profiles (`profiles/*.inc`) are statistics over renderer state
(template lines only). The caches it writes are derived from the user's own game files and must not be distributed.

*Resistance: Fall of Man*, *Resistance 2* and *Resistance 3* are © Sony Interactive Entertainment, developed by
Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac.
