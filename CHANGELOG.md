# Changelog

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
