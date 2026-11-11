# shaderscan

Offline RSX shader-cache seeding for statically recompiled PS3 games.

shaderscan finds the RSX vertex and fragment programs in **your own** game files and writes them into the shader
cache of the renderer your port uses. The game then compiles them in its normal start-up preload ("Compile shaders
before playing"), so shaders no longer have to be discovered, and stutter, during a playthrough.

It is a command-line tool (`shaderscan.exe`, Windows x64, no installer, no runtime DLLs). The Resistance ports'
Setup downloads a pinned release build into `tools\shaderscan.exe`, and their launchers run it before Play.

- [Supported games and back ends](#supported-games-and-back-ends)
- [How it works](#how-it-works)
- [Command line](#command-line)
- [Launcher contract](#launcher-contract)
- [Results](#results)
- [Building](#building)
- [Extending it](#extending-it): C/C++ API, front ends, back ends, runtime harvest
- [Legal note and licence](#legal-note-and-licence)
- [Credits](#credits)

## Supported games and back ends

| Game | Port | Back end | Cache written |
|---|---|---|---|
| Resistance 3 (BCES01905 and others) | Resistance3Port (not released yet) | `rpcs3` | `<port>\shadercache\rpcs3\shaders_cache` (RPCS3's Vulkan cache: `raw\*.fp`, `raw\*.vp`, `pipelines\vulkan\v1.95\*.bin`) |
| Resistance 2 | Resistance2Port (not released yet) | `rpcs3` | same |
| Resistance: Fall of Man | Resistance-Fall-of-Man-Recomp (not released yet) | `r1d3d12` | `<port>\shadercache` (`<key>.hlsl` + `<key>.dxbc`, the port's own D3D12 renderer) |

The core knows nothing about a particular game or renderer. Engine knowledge lives in small **front ends**
(`insomniac`, plus the engine-agnostic `signature` research aid). Renderer knowledge (how a program becomes a cache
entry) lives in **back ends**: `rpcs3` (any port rendering through RPCS3's Vulkan backend) and `r1d3d12`.
`--backend auto` (the default) picks `r1d3d12` for Resistance: Fall of Man and `rpcs3` otherwise.

```
game files ──► front end (per engine) ──► program_set (dedupe by RPCS3 ucode hash)
                                              │
            state profile (embedded + learned from an existing cache)
                                              │
                                              ▼
                     back end ──► the renderer's on-disk cache (atomic writes, stamp last)
```

## How it works

1. **Inputs.** It reads the decrypted `EBOOT.elf` (default `<game>\game\EBOOT.elf`), loose files and PSARC
   archives under `--game` (zlib members are inflated with miniz). By default only the members a front end asks for
   (`wants_file`) are read. `--deep` reads every member: slow, and mainly useful while writing a new front end.
2. **Front end.** `scan()` gets one file image and adds each program it finds to a `program_set`, together with the
   register state it knows: FP texcoord mask, centroid mask, shader control bits, VP output mask.
3. **Analysis and hashing** (`rsx_program.cpp`) mirror RPCS3's `ProgramStateCache.cpp`:
   - FP: `analyse_fragment_program` (start offset, length including embedded constants, texture and BX2 masks), then
     `get_fragment_program_ucode_hash`.
   - VP: `analyse_vertex_program` (the branch walk builds the instruction mask and jump table; unreached slots are
     zeroed), then `get_vertex_program_ucode_hash`.

   Programs are deduplicated by these hashes, so the `raw\` file names are exactly the ones RPCS3 would produce.
4. **States.** A pipeline entry also needs the fragment and vertex state RPCS3 keys on (control bits, texture
   dimensions, MRT count, ...) plus the Vulkan pipeline properties (raster, depth/stencil, blend, render pass). These
   are not in the program, so they come from a **profile**:
   - **embedded** profiles (`profiles\*.inc`) are statistics over the emulator state (template lines only; no game
     data);
   - **learned**: when `--out` (or `--learn <dir>`) already holds a recorded cache with at least 100 pipelines, its
     state templates and VP↔FP pairings are learned and merged in front of the embedded ones. Entries written by
     earlier shaderscan runs (listed in `shaderscan_manifest.txt`) are never learned from.

   Each fragment program gets up to `--variants` state templates × `--td` texture-dimension combinations ×
   `--props` pipeline states. Every vertex program and output mask is emitted at least once.
5. **Write** (`rpcs3`). It writes `raw\<hash>.fp` (exact guest ucode), `raw\<hash>.vp` (RSXVertexProgram data) and
   a 672-byte `pipeline_data` record per entry. The file name is built from the same four hashes RPCS3 uses (ucode
   hashes, `hash_struct(vk::pipeline_props)`, `hash_array(state params)`). Modification times are placed after every
   recorded pipeline, so the preload order still follows real first use.

### What a generated entry buys you (`rpcs3`)

RPCS3's pipeline key includes state that only exists at run time (which VP is paired with which FP, render pass,
blend state ...). Offline, shaderscan therefore warms three things:

| Level | What is reused at run time | Needs exact state? |
|---|---|---|
| Shader modules (GLSL/SPIR-V decompile + compile of each VP/FP) | Every program it found | VP: output mask only. FP: control bits, texture dims |
| GPL library parts (`R2_VK_GPL`): vertex-input, pre-raster, fragment-shader, fragment-output | Parts whose key matches | Partially |
| Whole pipelines (exact `.bin` name) | Only with a recorded pairing | Yes |

The exact-name rate is low without a recorded cache. The point is to remove the expensive shader-module compiles
and as many GPL parts as possible before the first frame.

### r1d3d12 back end (Resistance: Fall of Man)

R1 has its own D3D12 renderer: RSX ucode → HLSL (`rsx_fp_decompiler.c` / `rsx_vp_decompiler.c`) → `D3DCompile`
(system `d3dcompiler_47`, flags 0, `ps_5_0` / `vs_5_0`) → `shadercache\<key>.dxbc` plus `<key>.hlsl`. The key is
FNV-1a 64 over `"rfom-shc-v1"`, target, entry `"main"` and the HLSL; there is no driver or version check, the key is
the validation.

- **Where the programs live:** only in `game\EBOOT.elf` (329 FP + 171 VP unique). There is no `shaders.dat` and no
  runtime assembly. The `insomniac` front end's EBOOT header scan covers it.
- **Translator:** the port's own translator sources, vendored under `third_party\rfom_translators` (MIT), so the
  generated HLSL is the game's byte for byte. `src\r1d3d12\r1_glue.c` holds verbatim copies of the renderer's text
  steps: FP state wrapper, VP and `SV_VertexID` (gf) forms, the motion-vector VS/PS, and the key.
- **Variants:** fragment state (shadow compare, expand, unnormalised dims, clamp/mirror, exp32) comes from draw
  state. It is learned from the recorded `.hlsl` files (`--learn`), the embedded `profiles\r1_profile.inc`, and a
  generic top-K (`--variants`, default 4). The VS gets plain, gf, and the MV forms when `RSX_FSR` > 0.
- **Environment:** `RSX_RES_SCALE`, `RSX_FSR`, `RSX_FSR_REACTIVE` are read like the runtime sees them (`run.bat` /
  `settings.bat` `set` lines, then the process environment) and fingerprinted in the stamp, so changing the scale
  makes `--check` ask for a rescan.
- **Verify:** `--verify <shadercache>` re-derives every key from its `.hlsl` and recompiles it, comparing the DXBC.
- **Not covered:** `pso_manifest_*2.bin` (the game's list of PSOs to build at start-up) needs the root signature,
  blend, raster and render-target state of real draws, so it is left to the game. The first draw of a new state
  still builds a PSO, but no longer compiles shaders.

## Command line

```
shaderscan --game <port or USRDIR dir> --out <shader cache dir> [options]
  --backend <name>     auto (default) | rpcs3 (RPCS3 Vulkan ...\shadercache\rpcs3\shaders_cache)
                       | r1d3d12 (Fall of Man D3D12 <port>\shadercache)
  --eboot <file>       decrypted EBOOT.elf (default <game>\game\EBOOT.elf)
  --frontend <name>    insomniac (default) | signature | none
  --discovery <mode>   how level containers are searched: both (default: byte-scan + index tables,
                       deduplicated) | tables (index tables; byte-scan only where unusable) | scan
  --profile <name>     embedded state profile: r3 | r2 | generic | auto (default, from PARAM.SFO)
                       | none (r1d3d12: learned states only)
  --learn <dir|->      shaders_cache to learn state templates from (default: --out; '-' = embedded only)
  --variants <K>       state templates per fragment program (default 6; r1d3d12: 4)
  --props <N>          pipeline states per fragment variant (default 1)
  --td <M>             texture-dimension combinations per variant (default 2)
  --cache-version <v>  pipeline directory version (default v1.95)
  --deep               scan every archived file, not only shader containers
  --dry-run            count only
  --clean              remove entries written by earlier runs
  --progress           machine-readable progress on stdout (@progress/@done/@error lines)
  --check              exit 0 if the cache is already seeded by this version, 1 otherwise
  --verify <dir>       load a cache like the renderer does and re-derive every key; exit 0 when consistent
  --emit-profile <f> <symbol>   write the back end's profile learned from --learn as a C include
  -q                   quiet
  --version            print the release, the cache stamp version and the built-in back ends
```

All arguments are Unicode (`wmain`); paths with spaces and non-ASCII characters work.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | done (`--check`: the cache is already seeded by this version; `--verify`: consistent) |
| `1` | failed; an `@error` line (with `--progress`) or a `shaderscan: ` line explains why (`--check`: a run is needed) |
| `2` | usage error |
| `3` | no supported programs found. The stamp is still written, so a launcher does not retry on every start |

### Progress output

With `--progress`, stdout gets one flushed line per event, at most about 20 per second (always the first and last
step of a phase):

```
@progress <phase> <done> <total>      phase = scan | write | clean
@done <entries_written> <entries_total>
@error <text>
```

Without `--progress`, human-readable progress goes to stderr as `[<phase>] <done>/<total>`.

### Versions and stamps

`shaderscan.stamp` in the output dir records `SHADERSCAN_VERSION` (currently **1**), the back end, and for
`r1d3d12` the game root and environment fingerprint. `--check` exits 1 when that does not match, which makes a
launcher rescan. `SHADERSCAN_VERSION` is only raised when the output changes; release numbers (`v1.0.0`, ...) can
change without it, so updating the exe does not force a rescan. `shaderscan --version` prints both.

## Launcher contract

This is the contract used by the Resistance launchers (R1, R2, R3). Any launcher can use it.

```
shaderscan.exe --check --out "<root>\shadercache\rpcs3\shaders_cache"          # exit 1 = run needed
shaderscan.exe --game "<root>" --out "<root>\shadercache\rpcs3\shaders_cache" --progress
```

(R1: `--out "<root>\shadercache"`; `--backend auto` recognises the game.)

- The exe is looked up as `<root>\shaderscan.exe`, then `<root>\tools\shaderscan.exe`. No exe = no hook; the game
  works without it.
- On **Play**, before the game starts, the launcher runs `--check`; exit 1 means a scan is needed (first launch, a
  new `SHADERSCAN_VERSION`, or after "Rebuild shader cache", which deletes `shaderscan.stamp`). If `--check` exits
  with anything other than 0 or 1, or takes more than 10 s, the launcher skips the scan for this launch.
- cwd = `<root>`, no console window, stdout and stderr on one pipe, below-normal priority. Nothing else is passed:
  the defaults (`--frontend auto`, `--profile auto`, EBOOT at `<root>\game\EBOOT.elf`, learn from `--out`) do the
  right thing for each port.
- The launcher shows the `@progress` lines in a progress panel with a Cancel / Skip button. It also accepts the
  stderr `[<phase>] <done>/<total>` format.
- Exit codes: `0` starts the game; `1` / `2` show the `@error` text and start the game (retried on the next Play);
  `3` starts the game silently (stamp written, not retried).

**Crash safety.** A launcher may `TerminateProcess` the tool at any time:
- Every file is written to `<out>\shaderscan.tmp\` and renamed into place. That dir is outside `pipelines\`, because
  RPCS3 counts every file in the pipelines dir and deletes wrong-sized ones.
- Each manifest line (`shaderscan_manifest.txt`) is flushed before its file appears.
- The stamp (`shaderscan.stamp`) is written last.

A rerun skips existing entries and finishes the job. Deleting the stamp forces a rescan, which only adds what is
missing. `--clean` removes only what the manifest lists, never entries the game recorded.

### Verify mode

`--verify <dir>` (`rpcs3`) mirrors `rsx::shaders_cache::load()` / `unpack()`, read-only:
- the record size must be 672;
- the raw VP must be at least 16 bytes and the raw FP non-empty;
- the VP instruction mask must not reach past the data;
- the FP must analyse from offset 0 with exactly the file's length;
- the jump table must be sorted and terminated.

It then **re-derives the file name** from the raw programs and the record. Run on a cache RPCS3 itself wrote, it is
a check of shaderscan's hashing. Run on generated output, it shows RPCS3 will accept and correctly key every entry.
`r1d3d12` re-derives every `.hlsl` key and recompiles each one, comparing against its `.dxbc`.

## Results

Measured on the Resistance ports with the user's own data. The recorded caches are the ones the games wrote while
playing the opening levels; they are only read.

| | R3 | R2 |
|---|---|---|
| Programs found (FP / VP) | 746 / 209 | 358 / 191 |
| Recorded raw FP reproduced (same hash, equal under RPCS3's compare) | **144 / 144** | **164 / 164** genuine |
| Recorded raw VP reproduced (byte-identical) | **70 / 70** | **94 / 94** |
| Entries written (defaults, embedded profile only) | 6324 | 2382 |
| Run time (scan + write) | ≈ 5 s | ≈ 1.6 s |
| `--verify` on generated cache | OK, 0 disagreements | OK, 0 disagreements |
| `--verify` on the game's own recorded cache | 1505/1505 keys re-derived | 2035/2035 keys re-derived (1656/1656 after the torn entries were purged) |
| Recorded pipelines whose VP module is warmed | 1505/1505 | 2035/2035 |
| ... whose FP module is warmed | 970/1505 | 1305/2035 |
| ... whose GPL pre-raster / fragment-output part is warmed | 594 / 692 | 931 / 1054 |
| Launcher mode (learning from the recorded cache): exact recorded pipelines reproduced | 149 | 90 |

R2's recorded cache once held 221 extra fragment programs that were torn reads of ring-buffer copies (a FIFO bug
since fixed in the port), not real programs; every genuine recorded FP is reproduced. Byte differences in recorded
`.fp` files are expected: the engine patches FP constants at run time, and RPCS3's `fragment_program_compare` skips
constant slots.

**Variants trade-off** (R3, embedded profile). More templates per program give more exact fragment states, at the
cost of more entries to compile in the preload:

| `--variants` / `--td` | entries | recorded pipelines with a generated FP state |
|---|---|---|
| 6 / 2 (default) | 6.3 k | 970 / 1505 |
| 12 / 2 | 12.3 k | 1289 / 1505 |
| 24 / 4 | 24.1 k | 1362 / 1505 (saturates) |

**r1d3d12** (R1, keys normalised to a 3× scale):

| mode | PS | VS | entries |
|---|---|---|---|
| `--verify` recorded cache | 190 keys, 0 disagree | 313 keys, 0 disagree | DXBC byte-identical |
| learned (`--learn` recorded cache) | 172/172 | 313/313 | 2058 |
| fresh install, embedded profile | 190/190 | 313/313 | 2058 |
| hold-out (learn half, test other half) | 66/91 (73 %); 73/91 with `--variants 8` | 160/160 | 2141 |

## Building

Requirements: Windows x64, Visual Studio 2022 C++ build tools (MSVC v143 and the Windows SDK), CMake ≥ 3.20 and
Ninja (both ship with Visual Studio). clang-cl (LLVM) is used when it is installed; release builds use it.

```
build.bat
```

or by hand, from a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

The result is `build\shaderscan.exe`, a single exe with the C runtime linked statically.

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `SHADERSCAN_R1_VIDEO_DIR` | `third_party/rfom_translators/libs/video` | Fall of Man translator sources for the `r1d3d12` back end. Point it at `<ResistancePort>\ps3recomp\libs\video` to build against a port checkout (its `rsx_d3d12_backend.c` must carry the `rfom-shc-v1` key). Empty = build without `r1d3d12`. Windows only. |
| `SHADERSCAN_MINIZ_DIR` | `third_party/miniz` | miniz sources |

Each push and pull request is built by GitHub Actions (`.github/workflows/build.yml`), which uploads
`shaderscan.exe` as an artifact. A `v*` tag attaches the exe and `shaderscan.exe.sha256` to the GitHub release,
unless the release already has a `shaderscan.exe` (a hand-made release keeps its exe, so hashes pinned by the ports
stay valid).

## Extending it

### C/C++ API

`include\shaderscan.h` (link `shaderscan_core`):

```c
shaderscan_opts o = {0};
o.game_dir = "D:\\Games\\MyPort";                        /* UTF-8 */
o.out_dir  = "D:\\Games\\MyPort\\shadercache\\rpcs3\\shaders_cache";
shaderscan_result r;
int rc = shaderscan_run(&o, my_progress, my_ctx, &r);    /* SHADERSCAN_OK / _FAILED / _NOTHING_FOUND */
if (shaderscan_needs_run(o.out_dir)) { ... }
```

- `my_progress(const char* phase, unsigned done, unsigned total, void* user)` is called from the calling thread.
- `opts.extra` / `extra_count` let a caller pass programs it extracted itself (for example from a memory dump or a
  custom container) as `shaderscan_program` records. They go through the same dedupe and write path.
- `opts.discovery` is `"tables"`, `"scan"`, `"both"` or NULL (= both). Any other value fails the run.
- `result` reports program counts, entries written/existing, and coverage against the learned cache.

Linking `shaderscan_core` into a program makes that program subject to GPL-2.0. Spawning the CLI (the launcher
contract above) has no licence effect on the caller.

### Writing a front end

Implement `shaderscan::frontend` (`src\frontend.h`):

```cpp
class my_engine_frontend final : public frontend {
public:
    const char* name() const override { return "my_engine"; }
    bool wants_file(const std::string& lower_path) const override { return lower_path.ends_with(".shaderpack"); }
    size_t scan(const u8* d, size_t n, const std::string& source, program_set& out) override {
        // find your container's program records, then for each:
        //   out.add_fp(ucode, max_bytes, texcoord_mask, centroid_mask, shader_ctrl, source);
        //   out.add_vp(words_host_order, instruction_count, entry, output_mask, source);
        return added;
    }
};
```

Then register a factory next to `make_insomniac_frontend()` in `shaderscan.cpp`'s front-end switch.

Optional hooks (all default to no-ops), for front ends that need several files at once:
- `set_discovery(mode)` receives `--discovery`.
- `begin_group(name)` / `end_group(out)` bracket each archive, and each directory of loose files. Loose files are
  grouped by directory, so files that index each other arrive together.
- `finish(out)` runs after all inputs; programs added here come after everything else.
- `report()` returns text that is logged after the scan.

Guidelines:
- **Pass the real register state when the container has it.** The FP texcoord control and centroid masks and the
  shader-control low byte, and the VP output mask, are part of RPCS3's key. With wrong values, the entries compile
  but never match.
- `add_fp` takes the bytes exactly as they sit in guest memory (RSX FP ucode is halfword-swapped little-endian; do
  not convert). `add_vp` takes host-order u32 words, as they are uploaded through `NV4097_SET_TRANSFORM_PROGRAM`.
- **Validate your matches.** `add_fp` rejects programs that don't terminate inside `avail` or use opcodes outside
  the RSX FP set. Front ends should add structural checks of their own; the Insomniac one requires the analysed
  program to fill the declared size exactly. That is what keeps `--deep` from picking up header look-alikes in
  movie files.
- **Check the result against a recorded cache:** play a little, then run with `--learn <that cache> --dry-run`. The
  `recorded cache: FP x/y, VP x/y` line is your recall.

#### Insomniac front end (`insomniac`)

Programs carry a 0x20-byte big-endian header (layout in `fe_insomniac.cpp`): FP size, texcoord/centroid masks,
`0x84xx` control word and ucode offset; VP instruction count, output mask, `0x20/0xFFFF` marker and ucode offset.
They are found in the EBOOT (engine shaders and all VPs) and, for R3, in `built\levels\<lvl>\shaders.dat` inside
each `level_cached.psarc`.

Level programs in `shaders.dat` are found in two ways (`--discovery`):

- **scan**: a byte-scan of `shaders.dat` for program headers.
- **tables**: an exact walk of the engine's own index, implemented from the format description only (IGHW
  container; no code from other tools):
  1. `assetlookup.dat` (IGHW) section `0x1D100` holds `{u64 tuid, u32 offset, u32 length}` records, each pointing
     at one shader blob in `shaders.dat`.
  2. Each blob is itself IGHW. Section `0x33002` holds `{u64 hash, u32 size, u32 offset}` records. The offset is
     into section `0x33001`, which holds the FP headers and microcode.
  3. Each record must pass every check, or it is rejected: offsets and sizes in bounds and 16-aligned; control
     word `0x84xx` and a zero word at `+0xC`; ucode offset and size consistent with the record size (at most
     64 KiB); the program analyses with no unknown opcode and ends exactly at the declared size.

  Every IGHW header, section and pointer is bounds-checked, and malformed input is rejected rather than read. The
  walk was fuzzed with 240 mutated copies of an R3 level, under a release build and an AddressSanitizer build.
- **both** (default): runs both and deduplicates. Programs found by the tables are added after the scan, so the
  output order (and the files written) is identical to the scan alone.

In `tables` mode, a level with no `assetlookup.dat`, no blob holding `0x33002` (R2, R3 `lobby`), or any rejected
entry is byte-scanned instead. The run log carries a `discovery` block with the counts per method.

| | R3 | R2 |
|---|---|---|
| levels via tables / fallback | 26 / 1 (`lobby`: no programs) | 0 / 27 (material-only blobs) |
| table entries / rejected | 45 155 / 0 | n/a |
| unique level FPs: tables / scan / both / tables-only | 190 / 190 / 190 / 0 | 0 / 0 (R2 programs are all in the EBOOT) |
| output vs. byte-scan-only build | byte-identical (7445 files) in every mode | identical |

R1 has no IGHW level data (its programs are only in the EBOOT); `--discovery` has no effect there.

#### Signature scanner (`--frontend signature`)

Engine-agnostic: it looks for runs of slots that decode as valid RSX FP / VP instructions ending in an END bit.

| | recall FP | recall VP | candidates (FP / VP) | exact FP state |
|---|---|---|---|---|
| R3 EBOOT | 97/144 (67 %) | 68/70 (97 %) | 20 345 / 41 948 | 0 % |
| R2 EBOOT | 133/385 (35 %) | 94/94 (100 %) | 24 462 / 29 712 | 0 % |

Precision is around 1–3 %, and the register state around the microcode is unknown. **Use it as a research aid** to
locate programs while writing a real front end, not to seed a user's cache: it would write over 100 k entries.

### Writing a back end (your recomp's own renderer)

The core (`src\shaderscan.cpp`) finds the game files, runs the front ends, dedups programs, and owns progress, the
"nothing found" stamp and the C API. Everything renderer-specific sits behind `shaderscan::backend` in
`src\backend.h`:

| method | duty |
|---|---|
| `name()` | id used by `--backend` and in the stamp |
| `begin(ctx)` | load what `out_dir` already holds: recorded entries, earlier manifests, learned/embedded state |
| `emit_program(ctx, ref)` | called once per unique program, front-end programs first, then `opts.extra` / runtime-harvested ones |
| `emit_pipeline(ctx, hint)` | optional caller-supplied pipeline record in the back end's own format (may ignore) |
| `finish(ctx, progs)` | expand state variants, translate, compile, write each file atomically (temp + rename), skip what exists, write the manifest, write the stamp **last** |
| `clean(ctx)` | remove only files listed in its own manifest, never recorded entries |
| `needs_run(out)` | 1 when there is no current stamp (game root, tool/translator version, env fingerprint): `--check` |
| `verify(opts, dir, report)` | re-derive every recorded key from its content; 0 means the back end reproduces the runtime exactly |
| `emit_profile(opts, f, sym)` | optional: write learned state as a C include for `profiles\` |

To add one:
1. Write `src\backend_<name>.cpp` with a `make_<name>_backend()` factory and declare it in `backend.h`.
2. Register it in `select_backend()` (`shaderscan.cpp`) and teach `auto` how to recognise the game (PARAM.SFO
   title/ID, layout) or the cache in `out_dir` (file extensions).
3. Reuse the renderer's own translator rather than reimplementing it: compile its sources into a static lib in
   `CMakeLists.txt`, plus a thin C glue that reproduces every text step between "RSX ucode" and "compiler input"
   byte for byte (defines, wrappers, key hash). Anything the runtime reads from the environment (resolution scale,
   upscaler) must be read the same way and folded into the stamp fingerprint.
4. Implement `verify` first. Run it against a cache the game recorded; until it reports 0 mismatches, nothing the
   back end writes will ever be hit.
5. Measure coverage: generate into an empty dir and count recorded keys reproduced; a hold-out (learn from half,
   test on the other) estimates what a fresh install gets.

### Runtime harvest via the game's own recompiled code

When a game builds RSX programs at run time, the static recompilation already contains the exact builder. Call the
recompiled builder (or record what it produces) and hand each result to `shaderscan_ingest_program()`
(`src\ingest.cpp`). It hashes the program exactly as RPCS3 does (FP: leading NOP slots skipped, walk to END; VP:
`analyse_vertex_program`) and writes `raw\<hash>.fp/.vp` atomically when `out_dir` is set. It is thread-safe, so it
can run on a loading or worker thread. It writes no pipeline entry: a later `shaderscan_run` pairs the raw programs
with learned state, or the renderer's own cache store does it on first use.

```c
shaderscan_program p = { SHADERSCAN_FP, ucode_ptr, ucode_bytes };
shaderscan_ingest_info info;
if (shaderscan_ingest_program(&p, "<port>\\shadercache\\rpcs3\\shaders_cache", &info) == SHADERSCAN_OK)
    printf("%s %s\n", info.file_name, info.written ? "new" : "known");
```

Before writing a harness, check the gap is real: if the recorded programs you don't find are all suffixes of known
programs on the copy granularity, or known heads followed by zeros or texture bytes, written in one short window,
they are torn reads and the fix belongs in the FIFO or sync, not in the cache. On R2 the bind chain
(`setFragmentProgram` → a copy of `hdr + hdr[0x10]` into a per-frame ring) showed that no instructions are
generated, so the `insomniac` front end already finds every R2 program.

## Legal note and licence

shaderscan only reads files the user already has: their own decrypted EBOOT and their own game data. Its output is
a local cache derived from those files. **Generated caches contain game microcode and must not be distributed**;
every user generates their own. The tool and its embedded profiles contain no game data, only state statistics.

- shaderscan is **GPL-2.0-only** (`LICENSE`). The program analysis, hashing and on-disk format follow RPCS3's
  implementation (GPL-2.0), and the tool is a derivative of it.
- PSARC inflate uses **miniz** (MIT), vendored under `third_party\miniz`.
- The `r1d3d12` translators are the Fall of Man port's (MIT), vendored under `third_party\rfom_translators`.
- See `THIRD_PARTY_NOTICES.md`.

Other projects can run the CLI as a separate program without licence effect on themselves.

## Credits

- [RPCS3](https://github.com/RPCS3/rpcs3): the RSX program analysis, ucode hashing and shader cache format
  shaderscan reproduces, and the renderer the R2/R3 ports embed.
- [ps3recomp](https://github.com/sp00nznet) by Ned Heller / sp00nznet: the static recompilation framework the ports
  are built on.
- The Resistance ports (not released yet): Resistance: Fall of Man
  (based on [Psyykosis' recomp](https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp)), whose RSX to HLSL
  translators the `r1d3d12` back end uses; Resistance 2; Resistance 3.
- [miniz](https://github.com/richgel999/miniz) by Rich Geldreich, RAD Game Tools and Valve Software.

## Support

shaderscan is free, and it will stay free: there are no paid builds, no early access and no perks.
If it saved you some stutter and you feel like buying me a coffee, it's genuinely appreciated and keeps
the late-night tooling sessions going ☕
[buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98)

A tip supports my own tools and time. It is not a payment for any game or port, and nothing is unlocked by it.
If you'd like to support the wider scene, the projects in the credits above deserve it just as much.

*Resistance: Fall of Man*, *Resistance 2* and *Resistance 3* are © Sony Interactive Entertainment, developed by
Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac.
