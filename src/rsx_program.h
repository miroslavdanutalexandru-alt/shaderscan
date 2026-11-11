// shaderscan core: RSX program analysis and hashing, bit-exact with RPCS3's shader cache.
// SPDX-License-Identifier: GPL-2.0-only
// The analysis/hash rules mirror rpcs3/Emu/RSX/Program/ProgramStateCache.cpp (GPL-2.0).
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace shaderscan
{
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;

	constexpr u32 max_vp_instructions = 544; // rsx::max_vertex_program_instructions

	inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
	inline u16 be16(const u8* p) { return u16((p[0] << 8) | p[1]); }
	inline u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
	inline u64 le64(const u8* p) { return u64(le32(p)) | (u64(le32(p + 4)) << 32); }
	inline u64 rotr64(u64 x, unsigned n) { n &= 63; return n ? (x >> n) | (x << (64 - n)) : x; }

	// ---- fragment programs --------------------------------------------------------------------------------------

	struct fp_info
	{
		u32 start = 0;        // byte offset of the first non-NOP slot (fragment_program_metadata::program_start_offset)
		u32 length = 0;       // ucode bytes from start, including embedded constants
		u16 textures = 0;     // referenced_textures_mask
		u16 bx2 = 0;          // bx2_texture_reads_mask
		u32 instructions = 0; // executed slots (constants excluded)
		bool branches = false;
		bool nop = false;
		bool unknown_opcode = false; // opcode outside the RSX FP set (only used by the signature scanner)
	};

	// Walks the program like fragment_program_utils::analyse_fragment_program. False when it runs past 'avail'.
	bool fp_analyse(const u8* p, size_t avail, fp_info& out);
	// fragment_program_utils::get_fragment_program_ucode_hash over 'len' bytes starting at the program start.
	u64 fp_hash(const u8* p, size_t len);
	bool fp_slot_has_constant(const u8* slot);

	struct fp_header_state
	{
		u32 texcoord_mask = 0;  // fp_texcoord_control
		u32 centroid_mask = 0;  // fp lighting flags bits 1..10 when the target is multisampled
		u32 shader_ctrl = 0;    // static shader-control bits (& 0xCE)
		bool operator==(const fp_header_state&) const = default;
	};

	struct fragment_program
	{
		std::vector<u8> ucode;  // exactly what RPCS3 stores in raw\<hash>.fp
		u64 hash = 0;
		u16 textures = 0, bx2 = 0;
		std::vector<fp_header_state> states; // distinct register states seen with this ucode (usually one)
		std::string source;
		// Distinct byte images as found (from the program start incl. leading NOP slots, embedded constants
		// intact). RPCS3's hash ignores constant values; renderers that bake them into the shader need these.
		std::vector<std::vector<u8>> images;
	};

	// ---- vertex programs ----------------------------------------------------------------------------------------

	struct vertex_program
	{
		std::vector<u32> data;      // RSXVertexProgram::data (relative to base, unreached slots zeroed)
		u64 instruction_mask[9]{};  // bit i = slot i (relative to base)
		std::vector<u16> jump_table;
		u32 base = 0, entry = 0;
		u16 textures = 0;
		u32 output_mask = 0;          // first output mask seen
		std::vector<u32> output_masks; // all distinct output masks seen with this ucode
		u64 hash = 0;
		std::string source;
		// Distinct transform-memory images as found (host-order words from slot 0, every slot), see fragment_program.
		std::vector<std::vector<u32>> images;
	};

	// Port of vertex_program_utils::analyse_vertex_program. 'words' holds the transform-program memory
	// (up to max_vp_instructions * 4 u32, host order). False on malformed control flow.
	bool vp_analyse(const u32* words, u32 slot_count, u32 entry, vertex_program& out);
	// vertex_program_utils::get_vertex_program_ucode_hash
	u64 vp_hash(const vertex_program& vp);

	// ---- RPCS3 util/fnv_hash.hpp --------------------------------------------------------------------------------
	constexpr u64 fnv_seed = 14695981039346656037ull;
	constexpr u64 fnv_prime = 1099511628211ull;
	inline u64 fnv64(u64 h, u64 v) { return (h ^ v) * fnv_prime; }
	inline u64 fnv_struct64(const u8* p, size_t n)
	{
		u64 h = fnv_seed;
		for (size_t i = 0; i + 8 <= n; i += 8) h = fnv64(h, le64(p + i));
		return h;
	}
}
