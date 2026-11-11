// SPDX-License-Identifier: GPL-2.0-only
#include "rpcs3_cache.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>

namespace fs = std::filesystem;

namespace shaderscan
{
	u64 props_hash(const u8* p)
	{
		// vk::pipeline_props: ia @0 (32), ds @32 (104), att_state[4] @136 (32 each), cs @264 (56), rs @320 (64),
		// ms @384 (48), temp_storage.msaa_sample_mask @432, renderpass_key @440
		u64 seed = le64(p + 440);
		seed ^= fnv_struct64(p + 0, 32);
		seed ^= fnv_struct64(p + 32, 104);
		seed ^= fnv_struct64(p + 320, 64);
		seed ^= fnv_struct64(p + 384, 48);
		seed ^= le32(p + 432);
		const u32 attachments = std::min<u32>(le32(p + 264 + 28), 4);
		for (u32 i = 0; i < attachments; i++) seed ^= fnv_struct64(p + 136 + i * 32, 32);
		return seed;
	}

	u64 state_hash(const pipeline_record& r)
	{
		const u32 params[] = { r.vp_ctrl0, r.vp_ctrl1, r.fp_ctrl, r.vp_texture_dimensions, r.fp_texture_dimensions, r.fp_texcoord_control,
			r.fp_height, r.fp_pixel_layout, r.fp_lighting_flags, r.fp_shadow_textures, r.fp_redirected_textures,
			r.vp_multisampled_textures, r.fp_multisampled_textures, r.fp_mrt_count };
		u64 h = fnv_seed;
		for (u32 v : params) h = fnv64(h, v);
		return h;
	}

	std::string record_file_name(const pipeline_record& r)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%" PRIX64 "+%" PRIX64 "+%" PRIX64 "+%" PRIX64 ".bin", r.vertex_program_hash, r.fragment_program_hash,
			r.pipeline_storage_hash, state_hash(r));
		return buf;
	}

	static bool parse_hex_name(const std::string& stem, u64& v)
	{
		if (stem.empty() || stem.size() > 16) return false;
		v = 0;
		for (char c : stem)
		{
			int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
			if (d < 0) return false;
			v = (v << 4) | u64(d);
		}
		return true;
	}

	bool read_cache(const fs::path& root, const std::string& version, cache_contents& out)
	{
		std::error_code ec;
		for (fs::directory_iterator it(root / "raw", ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::path& p = it->path();
			u64 h;
			if (!parse_hex_name(u8str(p.stem()), h)) continue;
			if (it->file_size(ec) == 0) continue;
			if (p.extension() == ".fp") out.raw_fp.insert(h);
			else if (p.extension() == ".vp") out.raw_vp.insert(h);
		}
		ec.clear();
		for (fs::directory_iterator it(root / "pipelines" / "vulkan" / version, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec) || it->file_size(ec) != sizeof(pipeline_record)) continue;
			pipeline_record r;
			{
				std::ifstream f(it->path(), std::ios::binary);
				if (!f || !f.read(reinterpret_cast<char*>(&r), sizeof(r))) continue;
			}
			out.records.push_back(r);
			out.record_names.push_back(u8str(it->path().filename()));
			out.pipeline_names.insert(out.record_names.back());
			const auto t = it->last_write_time(ec);
			if (!ec && (!out.any || t > out.newest)) out.newest = t;
			out.any = true;
		}
		return true;
	}

	static bool read_all(const fs::path& p, std::vector<u8>& out)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return false;
		f.seekg(0, std::ios::end);
		out.resize(size_t(f.tellg()));
		f.seekg(0);
		return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
	}

	verify_report verify_cache(const fs::path& root, const std::string& version)
	{
		verify_report v;
		auto note = [&](const std::string& s) { if (v.examples.size() < 8) v.examples.push_back(s); };
		std::set<std::string> raw_used, raw_all;
		std::error_code ec;
		for (fs::directory_iterator it(root / "raw", ec), end; !ec && it != end; it.increment(ec))
		{
			const std::string n = u8str(it->path().filename());
			raw_all.insert(n);
			if (it->path().extension() == ".fp") v.raw_fp++;
			else if (it->path().extension() == ".vp") v.raw_vp++;
		}
		ec.clear();
		std::vector<u8> vbuf, fbuf;
		for (fs::directory_iterator it(root / "pipelines" / "vulkan" / version, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->is_directory(ec)) continue; // shaders_cache::load() skips directories
			v.files++;
			const std::string name = u8str(it->path().filename());
			std::vector<u8> data;
			if (!read_all(it->path(), data) || data.size() != sizeof(pipeline_record))
			{
				v.bad_size++;
				note(name + ": size " + std::to_string(data.size()));
				continue;
			}
			pipeline_record r;
			std::memcpy(&r, data.data(), sizeof(r));
			char hn[40];
			std::snprintf(hn, sizeof(hn), "%" PRIX64 ".vp", r.vertex_program_hash);
			const std::string vpn = hn;
			std::snprintf(hn, sizeof(hn), "%" PRIX64 ".fp", r.fragment_program_hash);
			const std::string fpn = hn;
			raw_used.insert(vpn);
			raw_used.insert(fpn);
			// load_vp_raw / load_fp_raw: an empty program makes load_shaders() drop the entry
			if (!read_all(root / "raw" / vpn, vbuf) || vbuf.size() < 16 || !read_all(root / "raw" / fpn, fbuf) || fbuf.empty())
			{
				v.missing_raw++;
				note(name + ": raw program missing");
				continue;
			}
			v.accepted++;

			vertex_program vp;
			vp.data.resize(vbuf.size() / 4);
			std::memcpy(vp.data.data(), vbuf.data(), vp.data.size() * 4);
			std::memcpy(vp.instruction_mask, r.vp_instruction_mask, sizeof(vp.instruction_mask));
			const u32 slots = u32(vp.data.size() / 4);
			bool prog_ok = slots <= max_vp_instructions;
			for (u32 i = slots; i < max_vp_instructions; i++)
				if ((vp.instruction_mask[i >> 6] >> (i & 63)) & 1) prog_ok = false;
			fp_info fi;
			if (!fp_analyse(fbuf.data(), fbuf.size(), fi) || fi.start != 0 || fi.length != fbuf.size()) prog_ok = false;
			if (!prog_ok)
			{
				v.bad_program++;
				note(name + ": program does not analyse");
			}

			bool jt_ok = true;
			int prev = -1, k = 0;
			for (; k < 32 && r.vp_jump_table[k] != 0xFFFF; k++)
			{
				if (int(r.vp_jump_table[k]) <= prev || r.vp_jump_table[k] >= max_vp_instructions) jt_ok = false;
				prev = r.vp_jump_table[k];
			}
			if (!jt_ok)
			{
				v.bad_jump_table++;
				note(name + ": jump table");
			}

			pipeline_record h = r;
			h.vertex_program_hash = vp_hash(vp);
			h.fragment_program_hash = fp_hash(fbuf.data(), fbuf.size());
			h.pipeline_storage_hash = props_hash(r.props);
			if (record_file_name(h) != name)
			{
				v.bad_name++;
				note(name + ": recomputed " + record_file_name(h));
			}
		}
		for (const auto& n : raw_all) v.raw_unreferenced += raw_used.count(n) ? 0u : 1u;
		return v;
	}

	int profile::add_props(const props_blob& p)
	{
		for (size_t i = 0; i < props.size(); i++)
			if (props[i] == p) return int(i);
		props.push_back(p);
		return int(props.size() - 1);
	}

	profile learn_profile(const cache_contents& cache)
	{
		profile out;
		using key_t = std::tuple<bool, u32, bool, u8>;
		std::map<key_t, std::map<int, u64>> uses;
		std::map<key_t, u64> weights;
		for (const auto& r : cache.records)
		{
			if (r.fp_shadow_textures || r.fp_redirected_textures || r.fp_multisampled_textures) continue; // depth-texture variants
			out.records++;
			props_blob pb;
			std::memcpy(pb.data(), r.props, props_size);
			const int pi = out.add_props(pb);
			const key_t key{ r.fp_texture_dimensions != 0, r.fp_ctrl & ~0xFFu, ((r.fp_lighting_flags >> 1) & 0x3FF) != 0, r.fp_mrt_count };
			weights[key]++;
			uses[key][pi]++;
			for (int u = 0; u < 16; u++)
			{
				const u32 v = (r.fp_texture_dimensions >> (u * 2)) & 3;
				if (v) out.dims[u][v]++;
			}
		}
		for (const auto& [key, w] : weights)
		{
			state_template t;
			std::tie(t.textured, t.ctrl_hi, t.centroid, t.mrt) = key;
			t.weight = w;
			std::vector<std::pair<u64, int>> ps;
			for (const auto& [pi, n] : uses[key]) ps.emplace_back(n, pi);
			std::stable_sort(ps.begin(), ps.end(), [](auto& a, auto& b) { return a.first > b.first; });
			for (size_t i = 0; i < ps.size() && i < 8; i++) t.props.push_back(ps[i].second);
			(t.textured ? out.textured : out.untextured).push_back(std::move(t));
		}
		auto by_weight = [](const state_template& a, const state_template& b) { return a.weight > b.weight; };
		std::stable_sort(out.textured.begin(), out.textured.end(), by_weight);
		std::stable_sort(out.untextured.begin(), out.untextured.end(), by_weight);

		// keep only referenced props blobs
		std::vector<int> remap(out.props.size(), -1);
		std::vector<props_blob> kept;
		for (auto* list : { &out.textured, &out.untextured })
			for (auto& t : *list)
				for (int& pi : t.props)
				{
					if (remap[pi] < 0) { remap[pi] = int(kept.size()); kept.push_back(out.props[pi]); }
					pi = remap[pi];
				}
		out.props = std::move(kept);
		return out;
	}

	bool parse_profile(const char* const* lines, profile& out)
	{
		out = {};
		for (; lines && *lines; ++lines)
		{
			const char* l = *lines;
			if (l[0] == 'T')
			{
				state_template t;
				unsigned tx, ctrl, cen, mrt;
				unsigned long long w;
				int n = 0;
				if (std::sscanf(l, "T %u %x %u %u %llu%n", &tx, &ctrl, &cen, &mrt, &w, &n) < 5) return false;
				t.textured = tx != 0; t.ctrl_hi = ctrl; t.centroid = cen != 0; t.mrt = u8(mrt); t.weight = w;
				const char* s = l + n;
				int pi, m;
				while (std::sscanf(s, "%d%n", &pi, &m) == 1) { t.props.push_back(pi); s += m; }
				(t.textured ? out.textured : out.untextured).push_back(std::move(t));
			}
			else if (l[0] == 'U')
			{
				unsigned u;
				unsigned long long a, b, c;
				if (std::sscanf(l, "U %u %llu %llu %llu", &u, &a, &b, &c) != 4 || u >= 16) return false;
				out.dims[u][1] = a; out.dims[u][2] = b; out.dims[u][3] = c;
			}
			else if (l[0] == 'P')
			{
				const char* s = l + 2;
				if (std::strlen(s) != props_size * 2) return false;
				props_blob p;
				for (size_t i = 0; i < props_size; i++)
				{
					unsigned v;
					if (std::sscanf(s + i * 2, "%2x", &v) != 1) return false;
					p[i] = u8(v);
				}
				out.props.push_back(p);
			}
			else if (l[0] == 'N')
			{
				unsigned long long n;
				if (std::sscanf(l, "N %llu", &n) == 1) out.records = n;
			}
		}
		for (auto* list : { &out.textured, &out.untextured })
			for (auto& t : *list)
				for (int pi : t.props)
					if (pi < 0 || size_t(pi) >= out.props.size()) return false;
		return true;
	}

	void emit_profile(const profile& p, std::FILE* f, const char* symbol)
	{
		std::fprintf(f, "// Generated by shaderscan --emit-profile. Emulator state statistics only (RPCS3 fragment-state flags,\n"
			"// Vulkan pipeline state, texture-dimension counts); contains no game code or data.\n");
		std::fprintf(f, "static const char* const %s[] = {\n", symbol);
		std::fprintf(f, "\t\"N %llu\",\n", static_cast<unsigned long long>(p.records));
		for (const auto* list : { &p.textured, &p.untextured })
			for (const auto& t : *list)
			{
				std::fprintf(f, "\t\"T %u %x %u %u %llu", t.textured ? 1u : 0u, t.ctrl_hi, t.centroid ? 1u : 0u, unsigned(t.mrt),
					static_cast<unsigned long long>(t.weight));
				for (int pi : t.props) std::fprintf(f, " %d", pi);
				std::fprintf(f, "\",\n");
			}
		for (int u = 0; u < 16; u++)
			if (p.dims[u][1] | p.dims[u][2] | p.dims[u][3])
				std::fprintf(f, "\t\"U %d %llu %llu %llu\",\n", u, static_cast<unsigned long long>(p.dims[u][1]),
					static_cast<unsigned long long>(p.dims[u][2]), static_cast<unsigned long long>(p.dims[u][3]));
		for (const auto& b : p.props)
		{
			std::fprintf(f, "\t\"P ");
			for (u8 v : b) std::fprintf(f, "%02x", v);
			std::fprintf(f, "\",\n");
		}
		std::fprintf(f, "\tnullptr };\n");
	}

	profile merge_profiles(const profile& a, const profile& b)
	{
		if (a.empty()) return b;
		if (b.empty()) return a;
		profile out = a;
		auto same = [](const state_template& x, const state_template& y)
		{
			return x.textured == y.textured && x.ctrl_hi == y.ctrl_hi && x.centroid == y.centroid && x.mrt == y.mrt;
		};
		for (const auto* list : { &b.textured, &b.untextured })
			for (const auto& t : *list)
			{
				auto& dst = t.textured ? out.textured : out.untextured;
				if (std::any_of(dst.begin(), dst.end(), [&](const state_template& x) { return same(x, t); })) continue;
				state_template c = t;
				c.weight = 0; // after every learned template, in the fallback's order
				for (int& pi : c.props) pi = out.add_props(b.props[pi]);
				dst.push_back(std::move(c));
			}
		for (int u = 0; u < 16; u++)
			if (!(out.dims[u][1] | out.dims[u][2] | out.dims[u][3]))
				for (int k = 1; k < 4; k++) out.dims[u][k] = b.dims[u][k];
		return out;
	}
}
