# rfom_translators

Unmodified copies of the RSX to HLSL translators of the Resistance: Fall of Man PC port's own D3D12 renderer.
shaderscan's `r1d3d12` back end compiles them, so the HLSL it generates and the cache keys it derives match the game
byte for byte.

| File here | Source in the port |
|---|---|
| `libs/video/rsx_fp_decompiler.c`, `.h` | `ps3recomp/libs/video/` |
| `libs/video/rsx_vp_decompiler.c`, `.h` | `ps3recomp/libs/video/` |
| `include/ps3emu/ps3types.h`, `endian.h`, `log_first.h` | `ps3recomp/include/ps3emu/` |

- Source: https://github.com/miroslavdanutalexandru-alt/Resistance-Fall-of-Man-Recomp (branch `dlss-upscaling`,
  commit `337e30fcbdbfa15de6e42fb29c6963489c88797c`), a fork of
  https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp.
- ps3recomp by Ned Heller / sp00nznet.
- Licence: **MIT**. `LICENSE.ps3recomp` is ps3recomp's licence and `LICENSE.rfom-port` the port's licence (it covers
  the port's changes to ps3recomp). Both notices must be kept with these files.

The layout mirrors the port (`libs/video` next to `include`), so `-DSHADERSCAN_R1_VIDEO_DIR=<ResistancePort>\ps3recomp\libs\video`
can point the build at a checkout instead. Update this copy when the port's translators change: `shaderscan --verify
<port>\shadercache --backend r1d3d12` then reports mismatches until it is refreshed.
