// RPCS3 on-disk shader cache format (rsx_cache.h, VKPipelineCompiler.h) and learned state profiles.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "rsx_program.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace shaderscan
{
	constexpr size_t props_size = 448; // sizeof(vk::pipeline_props) on x64

	// rsx::shaders_cache::pipeline_data with vk::pipeline_props as the storage type (672 bytes)
	struct pipeline_record
	{
		u64 vertex_program_hash;
		u64 fragment_program_hash;
		u64 pipeline_storage_hash;
		u32 vp_ctrl0, vp_ctrl1, vp_texture_dimensions, vp_reserved_0;
		u64 vp_instruction_mask[9];
		u32 vp_base_address, vp_entry;
		u16 vp_jump_table[32];
		u16 vp_multisampled_textures, vp_reserved_1;
		u32 vp_reserved_2;
		u32 fp_ctrl, fp_texture_dimensions, fp_texcoord_control;
		u16 fp_height, fp_pixel_layout, fp_lighting_flags, fp_shadow_textures, fp_redirected_textures, fp_multisampled_textures;
		u8 fp_mrt_count, fp_reserved0;
		u16 fp_reserved1;
		u32 fp_reserved2;
		u8 props[props_size];
	};
	static_assert(sizeof(pipeline_record) == 672, "pipeline_data layout");

	using props_blob = std::array<u8, props_size>;

	// UTF-8 text of a path (path::string() uses the ANSI code page on Windows and can throw)
	inline std::string u8str(const std::filesystem::path& p)
	{
		const auto s = p.u8string();
		return std::string(reinterpret_cast<const char*>(s.data()), s.size());
	}

	u64 props_hash(const u8* props);              // rpcs3::hash_struct<vk::pipeline_props>
	u64 state_hash(const pipeline_record& r);     // shaders_cache::store() state_params hash
	std::string record_file_name(const pipeline_record& r);

	struct cache_contents
	{
		std::vector<pipeline_record> records;
		std::vector<std::string> record_names; // parallel to records
		std::set<std::string> pipeline_names;
		std::set<u64> raw_fp, raw_vp;
		std::filesystem::file_time_type newest{};
		bool any = false;
	};

	bool read_cache(const std::filesystem::path& root, const std::string& version, cache_contents& out);

	// Loads a cache the way rsx::shaders_cache::load()/unpack() does and re-derives every key from the files.
	struct verify_report
	{
		unsigned files = 0;            // entries in pipelines\vulkan\<version>
		unsigned accepted = 0;         // RPCS3 would preload these
		unsigned bad_size = 0;         // not sizeof(pipeline_data): RPCS3 deletes the file
		unsigned missing_raw = 0;      // raw .vp/.fp missing or empty: RPCS3 skips the entry
		unsigned bad_name = 0;         // file name != hashes recomputed from the content (vp, fp, props, state)
		unsigned bad_program = 0;      // raw program does not analyse (FP END/length, VP mask past the data)
		unsigned bad_jump_table = 0;   // unsorted, out of range or unterminated jump table
		unsigned raw_fp = 0, raw_vp = 0, raw_unreferenced = 0;
		std::vector<std::string> examples; // first few problems
		bool ok() const { return files && accepted == files && !bad_name && !bad_program && !bad_jump_table; }
	};
	verify_report verify_cache(const std::filesystem::path& root, const std::string& version);

	// ---- state profile ------------------------------------------------------------------------------------------
	// Emulator-side fragment state that RPCS3 folds into a fragment program's cache key (RSXGraphEngine.cpp
	// get_current_fragment_program): dynamic control bits, centroid on/off (MSAA), MRT count, plus the Vulkan
	// pipeline_props seen with them and per-texture-unit dimension priors. Contains no game code.
	struct state_template
	{
		bool textured = true;
		u32 ctrl_hi = 0;      // fp ctrl & ~0xFF
		bool centroid = false;
		u8 mrt = 1;
		u64 weight = 0;
		std::vector<int> props; // indices into profile::props, most used first
	};

	struct profile
	{
		std::vector<state_template> textured, untextured;
		std::vector<props_blob> props;
		u64 dims[16][4]{}; // [unit][1=2D, 2=cube, 3=3D] observation counts
		u64 records = 0;

		bool empty() const { return textured.empty() && untextured.empty(); }
		int add_props(const props_blob& p);
	};

	profile learn_profile(const cache_contents& cache);
	bool parse_profile(const char* const* lines, profile& out);
	void emit_profile(const profile& p, std::FILE* f, const char* symbol);
	profile merge_profiles(const profile& primary, const profile& fallback);
}
