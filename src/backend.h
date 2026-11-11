// shaderscan back ends: turn the extracted RSX programs into one renderer's on-disk shader cache.
// SPDX-License-Identifier: GPL-2.0-only
//
// The core (shaderscan.cpp) finds the game files, runs the front ends and owns progress, the "nothing found" stamp
// and the C API. Everything that depends on the renderer -- how a program is translated, how the cache is keyed,
// which files are written, how a recorded cache is learned from and verified -- lives behind this interface.
//
// Call order of one shaderscan_run:
//   begin()                      load what is already in out_dir (recorded entries, earlier manifests, profiles)
//   emit_program() * N           each unique program, in discovery order (front ends first, then opts.extra /
//                                runtime-harvested programs); the program_set stays alive until finish()
//   emit_pipeline() * M          optional caller-supplied pipeline state in the back end's own record format
//   finish()                     state variants, compile, write atomically, manifest, stamp last
// clean() / needs_run() / verify() are used without a scan. See README.md "Writing a back end".
#pragma once

#include "shaderscan.h"
#include "frontend.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace shaderscan
{
	struct backend_ctx
	{
		const shaderscan_opts& o;
		std::filesystem::path out;
		shaderscan_progress_fn progress;
		void* user;
		shaderscan_result& r;

		void log(const char* fmt, ...) const;
		int fail(const char* fmt, ...) const; // fills r.message, returns SHADERSCAN_FAILED
		void step(const char* phase, unsigned done, unsigned total) const
		{
			if (progress) progress(phase, done, total, user);
		}
	};

	struct program_ref
	{
		const program_set& set;
		int kind;      // SHADERSCAN_FP / SHADERSCAN_VP
		size_t index;  // into set.fps / set.vps
	};

	struct pipeline_hint
	{
		const void* data; // back-end record (RPCS3: 672-byte pipeline_record; r1d3d12 ignores hints)
		size_t size;
	};

	class backend
	{
	public:
		virtual ~backend() = default;
		virtual const char* name() const = 0;
		virtual int begin(backend_ctx& c) = 0;
		virtual void emit_program(backend_ctx&, const program_ref&) {}
		virtual void emit_pipeline(backend_ctx&, const pipeline_hint&) {}
		virtual int finish(backend_ctx& c, program_set& progs) = 0;
		virtual int clean(backend_ctx& c) = 0;
		// 1 when out_dir lacks a current stamp from this back end (the launcher's --check)
		virtual int needs_run(const std::filesystem::path& out) const = 0;
		// Re-derive every key of a recorded cache; prints a report, returns 0 when consistent.
		virtual int verify(const shaderscan_opts& o, const std::filesystem::path& dir, std::FILE* report) = 0;
		// --emit-profile: write what was learned from o.learn_dir (or o.out_dir) as an embeddable C include.
		virtual int emit_profile(const shaderscan_opts&, std::FILE*, const char* /*symbol*/) { return -1; }
	};

	std::unique_ptr<backend> make_rpcs3_backend();
#ifdef SHADERSCAN_HAVE_R1D3D12
	std::unique_ptr<backend> make_r1d3d12_backend();
#endif

	// "rpcs3", "r1d3d12" or "auto"/NULL: auto picks r1d3d12 for Resistance: Fall of Man (PARAM.SFO / layout) or an
	// out_dir that already holds R1 shadercache files (*.dxbc / pso_manifest_*.bin), else rpcs3. NULL + message when
	// the requested back end is unknown or not built in.
	std::unique_ptr<backend> select_backend(const char* name, const char* game_dir, const char* out_dir, std::string* message);

	// shared helpers (shaderscan.cpp)
	constexpr const char* stamp_name = "shaderscan.stamp";
	constexpr const char* manifest_name = "shaderscan_manifest.txt";
	std::filesystem::path to_path(const char* s);
	std::string lower(std::string s);
	bool read_file(const std::filesystem::path& p, std::vector<u8>& out);
	std::string read_sfo_title(const char* game_dir); // "TITLE|TITLE_ID|" lower-case, empty when absent
}
