/* shaderscan - offline RSX shader-cache seeding for ps3recomp ports that use the embedded RPCS3 renderer.
 *
 * Extracts RSX vertex/fragment program microcode from the user's own game files (per-engine front ends plus a
 * generic signature scanner), dedupes/hashes it exactly like RPCS3 and writes entries in RPCS3's on-disk shader
 * cache format (raw\<hash>.fp/.vp + pipelines\vulkan\<ver>\<vh>+<fh>+<ph>+<sh>.bin). The renderer's existing
 * start-up preload then compiles them. Runs locally on the user's files and writes the local cache only.
 *
 * SPDX-License-Identifier: GPL-2.0-only (hashing / program analysis follow RPCS3's implementation)
 */
#ifndef SHADERSCAN_H
#define SHADERSCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define SHADERSCAN_VERSION 1

/* phase: "scan", "write", "clean"; done/total count units of that phase. */
typedef void (*shaderscan_progress_fn)(const char* phase, unsigned done, unsigned total, void* user);

enum shaderscan_kind { SHADERSCAN_FP = 0, SHADERSCAN_VP = 1 };

/* A program supplied by a caller-side extractor (custom engine, memory dump, ...). */
typedef struct shaderscan_program
{
	int kind;                  /* SHADERSCAN_FP / SHADERSCAN_VP */
	const void* ucode;         /* FP: raw bytes as in guest memory. VP: u32 words in host order (as uploaded via FIFO) */
	unsigned size;             /* bytes; FP may extend past the end instruction (the analyser trims it) */
	unsigned fp_texcoord_mask; /* FP: texcoord control (2D texcoord) mask, RSX shader-program register */
	unsigned fp_centroid_mask; /* FP: texcoord centroid mask used when the target is multisampled */
	unsigned fp_shader_ctrl;   /* FP: shader control register (KIL 0x80, 32-bit regs 0x40, depth export 0xE) */
	unsigned vp_output_mask;   /* VP: output attribute mask (NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK) */
	unsigned vp_entry;         /* VP: entry slot inside ucode (normally 0) */
} shaderscan_program;

typedef struct shaderscan_opts
{
	/* all paths are UTF-8 */
	const char* game_dir;   /* port root or USRDIR; searched recursively for archives / loose files */
	const char* eboot;      /* decrypted EBOOT.elf; NULL = <game_dir>\game\EBOOT.elf when present */
	const char* out_dir;    /* <port>\shadercache\rpcs3\shaders_cache */
	const char* learn_dir;  /* existing shaders_cache to learn state templates from; NULL = out_dir */
	const char* frontend;   /* "insomniac", "signature", "none" or NULL/"auto" (= insomniac) */
	const char* profile;    /* embedded default state profile: "r3", "r2", "generic" or NULL/"auto" */
	const char* cache_version; /* NULL = "v1.95" (VKGSRender's shaders_cache version prefix) */
	int variants;           /* state templates per fragment program (0 = default 6) */
	int props;              /* pipeline-state blobs per fragment-program variant (0 = default 1) */
	int td_max;             /* texture-dimension combinations per variant (0 = default 2) */
	int deep;               /* scan every archived file, not only the engine's shader containers */
	int dry_run;            /* count only, write nothing */
	int clean;              /* remove previously generated entries (manifest) and return */
	int verbose;
	const shaderscan_program* extra; /* optional caller-extracted programs */
	unsigned extra_count;
	/* appended after SHADERSCAN_VERSION 1 (zero-initialised callers keep the old behaviour) */
	const char* backend;    /* "rpcs3", "r1d3d12" or NULL/"auto" (from PARAM.SFO / the out_dir contents) */
	const struct shaderscan_pipeline* pipelines; /* optional recorded pipeline states for the back end */
	unsigned pipeline_count;
	/* front-end discovery inside containers: "tables" (the engine's own index, byte-scan fallback), "scan"
	 * (byte-scan only) or NULL/"both" (byte-scan plus tables, deduplicated) */
	const char* discovery;
} shaderscan_opts;

/* One pipeline state in the selected back end's own record format (rpcs3: the 672-byte pipeline_storage record;
 * r1d3d12: the 48-byte PsoRec of pso_manifest_*2.bin). Unknown sizes are ignored. */
typedef struct shaderscan_pipeline
{
	const void* data;
	unsigned size;
} shaderscan_pipeline;

typedef struct shaderscan_result
{
	unsigned fp_programs, vp_programs;        /* unique programs */
	unsigned fp_variants;                     /* unique (fp, fp state) variants generated */
	unsigned entries_total, entries_written, entries_existing;
	unsigned cache_fp, cache_fp_covered;      /* raw programs in the learn cache / also found by the scan */
	unsigned cache_vp, cache_vp_covered;
	unsigned cache_pipelines;                 /* recorded pipelines in the learn cache */
	unsigned cache_fp_state_hits;             /* ... whose exact (fp, fp state) the default profile generates */
	double seconds;
	char message[256];                        /* error text when the call fails */
} shaderscan_result;

/* Return codes of shaderscan_run (also the CLI exit codes). */
#define SHADERSCAN_OK 0
#define SHADERSCAN_FAILED 1          /* I/O error, bad options; result->message has the reason */
#define SHADERSCAN_NOTHING_FOUND 3   /* no supported programs in the game files; the stamp is written anyway */

/* Returns SHADERSCAN_OK on success. result may be NULL. Files are written atomically and the stamp last, so a
 * killed run can simply be repeated (existing entries are skipped). */
int shaderscan_run(const shaderscan_opts* opts, shaderscan_progress_fn progress, void* user, shaderscan_result* result);

/* 1 when out_dir has no stamp from this tool version (the launcher's "run on first launch" test). The back end
 * is picked from out_dir's contents; the r1d3d12 stamp also records the translator settings (RSX_RES_SCALE ...),
 * so changing them makes this return 1. */
int shaderscan_needs_run(const char* out_dir);

/* Re-derive every key of an existing cache with the back end opts->backend / auto picks for dir; prints a report
 * to stdout. 0 = consistent. */
int shaderscan_verify(const shaderscan_opts* opts, const char* dir);

/* Runtime harvest (src/ingest.cpp): analyse + hash one program exactly as RPCS3 does (FP: leading NOP slots are
 * skipped, the walk stops at END) and, when out_dir is set, store it as <out_dir>\raw\<hash>.fp/.vp if missing.
 * Thread-safe; no pipeline entry is written (pair it with recorded state via shaderscan_run's learn pass). */
typedef struct shaderscan_ingest_info
{
	unsigned long long hash;   /* RPCS3 ucode hash (the raw file name) */
	unsigned start, length;    /* FP: program start offset / ucode bytes from it; VP: base slot / data bytes */
	unsigned textures;         /* referenced texture mask */
	int written, existed;      /* raw file created now / already present */
	char file_name[40];        /* "<HASH>.fp" / "<HASH>.vp" */
} shaderscan_ingest_info;

int shaderscan_ingest_program(const shaderscan_program* prog, const char* out_dir, shaderscan_ingest_info* info);

#ifdef __cplusplus
}
#endif

#endif
