// shaderscan_ingest_program: hash (and optionally store) one program produced at run time, e.g. by the game's
// own recompiled shader code (README.md, "Runtime harvest").
// SPDX-License-Identifier: GPL-2.0-only
#include "shaderscan.h"
#include "rpcs3_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;
using namespace shaderscan;

namespace
{
	// Same scheme as the cache writer: temp file then rename, so a reader never sees a partial raw entry.
	bool write_raw(const fs::path& dir, const char* name, const void* data, size_t n, bool& existed)
	{
		std::error_code ec;
		const fs::path dst = dir / name;
		existed = fs::exists(dst, ec);
		if (existed) return true;
		fs::create_directories(dir, ec);
		const fs::path tmp = dir / (std::string(name) + ".ingest.tmp");
		{
			std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
			if (!f || !f.write(static_cast<const char*>(data), std::streamsize(n))) return false;
		}
		fs::rename(tmp, dst, ec);
		if (!ec) return true;
		fs::remove(tmp, ec);
		existed = fs::exists(dst, ec); // another writer won the race: same content by construction
		return existed;
	}
}

extern "C" int shaderscan_ingest_program(const shaderscan_program* prog, const char* out_dir, shaderscan_ingest_info* info)
{
	shaderscan_ingest_info tmp{};
	shaderscan_ingest_info& r = info ? *info : tmp;
	r = {};
	if (!prog || !prog->ucode || !prog->size) return SHADERSCAN_FAILED;

	char name[40];
	const void* bytes = nullptr;
	size_t nbytes = 0;
	std::vector<u32> vp_words;

	if (prog->kind == SHADERSCAN_FP)
	{
		const u8* p = static_cast<const u8*>(prog->ucode);
		fp_info fi;
		if (!fp_analyse(p, prog->size, fi) || fi.unknown_opcode) return SHADERSCAN_FAILED;
		r.hash = fp_hash(p + fi.start, fi.length);
		r.start = fi.start;
		r.length = fi.length;
		r.textures = fi.textures;
		bytes = p + fi.start;
		nbytes = fi.length;
		std::snprintf(name, sizeof(name), "%llX.fp", static_cast<unsigned long long>(r.hash));
	}
	else if (prog->kind == SHADERSCAN_VP)
	{
		const u32 slots = prog->size / 16;
		if (!slots || slots > max_vp_instructions || prog->vp_entry >= slots) return SHADERSCAN_FAILED;
		vertex_program vp;
		if (!vp_analyse(static_cast<const u32*>(prog->ucode), slots, prog->vp_entry, vp)) return SHADERSCAN_FAILED;
		r.hash = vp_hash(vp);
		r.start = vp.base;
		r.length = unsigned(vp.data.size() * 4);
		r.textures = vp.textures;
		vp_words = std::move(vp.data);
		bytes = vp_words.data();
		nbytes = vp_words.size() * 4;
		std::snprintf(name, sizeof(name), "%llX.vp", static_cast<unsigned long long>(r.hash));
	}
	else
	{
		return SHADERSCAN_FAILED;
	}

	std::memcpy(r.file_name, name, std::strlen(name) + 1);
	if (out_dir && *out_dir)
	{
		bool existed = false;
		const fs::path raw = fs::path(reinterpret_cast<const char8_t*>(out_dir)) / "raw";
		if (!write_raw(raw, name, bytes, nbytes, existed)) return SHADERSCAN_FAILED;
		r.existed = existed ? 1 : 0;
		r.written = existed ? 0 : 1;
	}
	return SHADERSCAN_OK;
}
