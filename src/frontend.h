// shaderscan front ends: per-engine extractors that find RSX programs in game files.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "rsx_program.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace shaderscan
{
	// Deduplicating program store (keyed by RPCS3's ucode hash, like raw\<hash>.fp/.vp).
	class program_set
	{
	public:
		// 'p' points at the program start (leading NOP slots allowed); 'avail' bounds the walk.
		// Returns false when the ucode does not analyse as a fragment program.
		bool add_fp(const u8* p, size_t avail, u32 texcoord_mask, u32 centroid_mask, u32 shader_ctrl, const std::string& source);
		// 'words' = 'count' instructions (4 u32 each, host order) loaded at transform slot 0.
		bool add_vp(const u32* words, u32 count, u32 entry, u32 output_mask, const std::string& source);

		std::vector<fragment_program> fps; // discovery order
		std::vector<vertex_program> vps;
		std::map<u64, size_t> fp_index, vp_index;
	};

	// How a front end that has both may find programs inside its containers (--discovery).
	enum class discovery_mode
	{
		tables = 1, // follow the containers' own index tables; byte-scan only containers whose tables are unusable
		scan = 2,   // byte-scan only (the original heuristics)
		both = 3,   // byte-scan, then add what the tables find (deduplicated)
	};

	class frontend
	{
	public:
		virtual ~frontend() = default;
		virtual const char* name() const = 0;
		// Archive members / loose files worth scanning outside --deep mode (lower-case path).
		virtual bool wants_file(const std::string& lower_path) const = 0;
		// Scan one file image (EBOOT, archive member, memory dump). Returns programs added.
		virtual size_t scan(const u8* data, size_t size, const std::string& source, program_set& out) = 0;

		// Optional hooks for front ends that pair files (an index plus the data it addresses). The core passes the
		// files of one container (an archive, or one directory of loose files) between begin_group() and
		// end_group(); finish() runs once after every input. Each returns programs added.
		virtual void set_discovery(discovery_mode) {}
		virtual void begin_group(const std::string& /*name*/) {}
		virtual size_t end_group(program_set&) { return 0; }
		virtual size_t finish(program_set&) { return 0; }
		// Human-readable per-method counts for the log (empty = nothing to report).
		virtual std::string report() const { return {}; }
	};

	// Insomniac Games engine (Resistance 1-3, Ratchet & Clank Future): headed RSX programs in the EBOOT and
	// in per-level built\levels\<lvl>\shaders.dat (located through assetlookup.dat's tables where present).
	std::unique_ptr<frontend> make_insomniac_frontend();
	// Engine-agnostic: finds headerless FP/VP microcode by instruction-stream signature. Lower precision.
	std::unique_ptr<frontend> make_signature_frontend();
}
