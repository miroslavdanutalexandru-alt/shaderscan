// shaderscan back end "rpcs3": RPCS3's Vulkan shader cache (raw\<hash>.fp/.vp + pipelines\vulkan\<ver>\*.bin).
// SPDX-License-Identifier: GPL-2.0-only
// For ports that embed RPCS3's renderer (Resistance 2 / 3). Moved verbatim out of shaderscan.cpp; its output is
// byte-identical to the single-back-end tool (checked against R2/R3 fresh + learn runs and --verify).
#include "backend.h"

#include "rpcs3_cache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#if __has_include("../profiles/r3_profile.inc")
#include "../profiles/r3_profile.inc"
#define SHADERSCAN_HAVE_R3 1
#endif
#if __has_include("../profiles/r2_profile.inc")
#include "../profiles/r2_profile.inc"
#define SHADERSCAN_HAVE_R2 1
#endif

namespace fs = std::filesystem;

namespace shaderscan
{
	namespace
	{
		const char* const* embedded_profile(const std::string& name)
		{
#ifdef SHADERSCAN_HAVE_R2
			if (name == "r2") return k_profile_r2;
#endif
#ifdef SHADERSCAN_HAVE_R3
			if (name == "r3" || name == "generic") return k_profile_r3;
#endif
			(void)name;
			return nullptr;
		}

		std::string guess_profile(const shaderscan_opts& o)
		{
			// PARAM.SFO TITLE / TITLE_ID first, then path names
			const std::string s = read_sfo_title(o.game_dir);
			for (const char* id : { "bcus98103", "bces00226" })
				if (s.find(id) != std::string::npos) return "r2";
			for (const char* id : { "bcus98176", "bces01118" })
				if (s.find(id) != std::string::npos) return "r3";
			if (s.find("resistance 2") != std::string::npos) return "r2";
			if (s.find("resistance 3") != std::string::npos) return "r3";

			const std::string p = lower(std::string(o.game_dir ? o.game_dir : "") + "|" + (o.eboot ? o.eboot : "") + "|" + (o.out_dir ? o.out_dir : ""));
			if (p.find("resistance2") != std::string::npos || p.find("bces00226") != std::string::npos || p.find("bcus98103") != std::string::npos)
				return "r2";
			if (p.find("resistance3") != std::string::npos) return "r3";
			return "generic";
		}

		// ---- variant generation -----------------------------------------------------------------------------------

		struct fp_state
		{
			u32 ctrl = 0, td = 0, tcc = 0;
			u16 lighting = 0;
			u8 mrt = 1;
			bool operator==(const fp_state&) const = default;
		};

		struct entry
		{
			int rank;
			size_t order;
			size_t fp, vp;
			u32 vp_output;
			fp_state st;
			int props;
		};

		// texture-dimension combinations for a referenced-texture mask, most likely first
		std::vector<u32> td_combos(u16 mask, const profile& p, int max_combos)
		{
			struct unit_opts { int unit; std::vector<std::pair<u32, double>> dims; };
			std::vector<unit_opts> units;
			u32 primary = 0;
			for (int u = 0; u < 16; u++)
			{
				if (!(mask & (1u << u))) continue;
				const u64 total = p.dims[u][1] + p.dims[u][2] + p.dims[u][3];
				unit_opts uo{ u, {} };
				if (!total) uo.dims.push_back({ 1, 1.0 });
				else
				{
					for (u32 k = 1; k < 4; k++)
						if (p.dims[u][k] && double(p.dims[u][k]) / double(total) >= 0.15) uo.dims.push_back({ k, double(p.dims[u][k]) / double(total) });
					std::stable_sort(uo.dims.begin(), uo.dims.end(), [](auto& a, auto& b) { return a.second > b.second; });
					if (uo.dims.empty()) uo.dims.push_back({ 1, 1.0 });
				}
				primary |= uo.dims[0].first << (u * 2);
				units.push_back(std::move(uo));
			}
			std::vector<u32> out{ primary };
			std::vector<std::pair<double, u32>> alts;
			for (const auto& uo : units)
				for (size_t i = 1; i < uo.dims.size(); i++)
					alts.push_back({ uo.dims[i].second, (primary & ~(3u << (uo.unit * 2))) | (uo.dims[i].first << (uo.unit * 2)) });
			std::stable_sort(alts.begin(), alts.end(), [](auto& a, auto& b) { return a.first > b.first; });
			for (const auto& a : alts)
			{
				if (int(out.size()) >= max_combos) break;
				out.push_back(a.second);
			}
			return out;
		}

		pipeline_record make_record(const vertex_program& vp, u32 vp_output, const fragment_program& fp, const fp_state& st, const props_blob& props)
		{
			pipeline_record r;
			std::memset(&r, 0, sizeof(r));
			r.vertex_program_hash = vp.hash;
			r.fragment_program_hash = fp.hash;
			std::memcpy(r.props, props.data(), props_size);
			r.pipeline_storage_hash = props_hash(r.props);
			r.vp_ctrl1 = vp_output;
			for (int u = 0; u < 4; u++)
				if (vp.textures & (1u << u)) r.vp_texture_dimensions |= 1u << (u * 2);
			std::memcpy(r.vp_instruction_mask, vp.instruction_mask, sizeof(r.vp_instruction_mask));
			r.vp_base_address = vp.base;
			r.vp_entry = vp.entry;
			size_t i = 0;
			for (; i < vp.jump_table.size() && i < 32; i++) r.vp_jump_table[i] = vp.jump_table[i];
			if (i < 32) r.vp_jump_table[i] = 0xFFFF;
			r.fp_ctrl = st.ctrl;
			r.fp_texture_dimensions = st.td;
			r.fp_texcoord_control = st.tcc;
			r.fp_lighting_flags = st.lighting;
			r.fp_mrt_count = st.mrt;
			return r;
		}

		// Written under <out>\shaderscan.tmp then renamed into place, so a killed run never leaves a truncated cache
		// file where RPCS3 would load it.
		constexpr const char* tmp_dir_name = "shaderscan.tmp";

		bool write_bytes(const fs::path& tmp_dir, const fs::path& p, const void* data, size_t n, const fs::file_time_type* mtime = nullptr)
		{
			const fs::path tmp = tmp_dir / p.filename();
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				if (!f || !f.write(static_cast<const char*>(data), std::streamsize(n))) return false;
			}
			std::error_code ec;
			if (mtime) fs::last_write_time(tmp, *mtime, ec);
			fs::rename(tmp, p, ec);
			if (!ec) return true;
			fs::remove(tmp, ec);
			return false;
		}

		class rpcs3_backend final : public backend
		{
			std::string version;
			int K = 6, N = 1, TD = 2;
			cache_contents existing, recorded, learned_cache;
			const cache_contents* learn = nullptr;
			bool no_learn = false;
			profile prof;

		public:
			const char* name() const override { return "rpcs3"; }

			int begin(backend_ctx& c) override
			{
				const shaderscan_opts& o = c.o;
				const fs::path& out = c.out;
				version = o.cache_version && *o.cache_version ? o.cache_version : "v1.95";
				K = o.variants > 0 ? o.variants : 6;
				N = o.props > 0 ? o.props : 1;
				TD = o.td_max > 0 ? o.td_max : 2;

				// ---- existing cache + state profile
				read_cache(out, version, existing);
				// Entries written by earlier runs (manifest) are not recorded gameplay: keep them out of learning, pairing
				// and coverage, otherwise a rerun would learn from its own output.
				{
					std::set<std::string> generated;
					std::ifstream m(out / manifest_name);
					for (std::string l; std::getline(m, l);)
					{
						const size_t slash = l.find_last_of('/');
						if (l.size() > 2 && slash != std::string::npos) generated.insert(l.substr(slash + 1));
					}
					auto hex_of = [](u64 h, const char* ext) { char b[40]; std::snprintf(b, sizeof(b), "%llX.%s", static_cast<unsigned long long>(h), ext); return std::string(b); };
					for (size_t i = 0; i < existing.records.size(); i++)
						if (!generated.count(existing.record_names[i]))
						{
							recorded.records.push_back(existing.records[i]);
							recorded.record_names.push_back(existing.record_names[i]);
							recorded.pipeline_names.insert(existing.record_names[i]);
						}
					for (u64 h : existing.raw_fp) if (!generated.count(hex_of(h, "fp"))) recorded.raw_fp.insert(h);
					for (u64 h : existing.raw_vp) if (!generated.count(hex_of(h, "vp"))) recorded.raw_vp.insert(h);
					recorded.any = !recorded.records.empty();
				}
				no_learn = o.learn_dir && std::strcmp(o.learn_dir, "-") == 0;
				learn = &recorded;
				if (o.learn_dir && *o.learn_dir && !no_learn)
				{
					read_cache(to_path(o.learn_dir), version, learned_cache);
					learn = &learned_cache;
				}
				std::string prof_name = o.profile && *o.profile && std::strcmp(o.profile, "auto") != 0 ? o.profile : guess_profile(o);
				profile base;
				if (const char* const* lines = embedded_profile(prof_name))
				{
					if (!parse_profile(lines, base)) return c.fail("embedded profile '%s' is corrupt", prof_name.c_str());
					if (prof_name == "generic") std::memset(base.dims, 0, sizeof(base.dims));
				}
				prof = base;
				if (!no_learn && learn->records.size() >= 100) prof = merge_profiles(learn_profile(*learn), base);
				if (prof.empty() || prof.props.empty()) return c.fail("no state profile: build with profiles\\*.inc or pass --learn <cache>");
				c.log("profile: %s%s, %zu textured / %zu untextured templates, %zu pipeline states\n", prof_name.c_str(),
					(!no_learn && learn->records.size() >= 100) ? " + learned" : "", prof.textured.size(), prof.untextured.size(), prof.props.size());
				return 0;
			}

			int finish(backend_ctx& c, program_set& progs) override
			{
				const shaderscan_opts& o = c.o;
				const fs::path& out = c.out;
				shaderscan_result& res = c.r;

				// ---- coverage against the recorded cache
				const cache_contents& eval = no_learn ? recorded : *learn;
				res.cache_fp = unsigned(eval.raw_fp.size());
				res.cache_vp = unsigned(eval.raw_vp.size());
				for (u64 h : eval.raw_fp) res.cache_fp_covered += progs.fp_index.count(h) ? 1u : 0u;
				for (u64 h : eval.raw_vp) res.cache_vp_covered += progs.vp_index.count(h) ? 1u : 0u;

				// partners learned from recorded pipelines (fp -> most used vp, vp -> most used fp)
				std::unordered_map<u64, std::map<u64, unsigned>> fp_vp, vp_fp;
				for (const auto& r : learn->records)
				{
					fp_vp[r.fragment_program_hash][r.vertex_program_hash]++;
					vp_fp[r.vertex_program_hash][r.fragment_program_hash]++;
				}
				auto best = [](const std::map<u64, unsigned>& m) { u64 b = 0; unsigned n = 0; for (auto& [h, k] : m) if (k > n) { n = k; b = h; } return b; };

				// ---- variants
				std::vector<entry> entries;
				std::vector<std::vector<fp_state>> fp_states(progs.fps.size());
				size_t rr_vp = 0;
				for (size_t fi = 0; fi < progs.fps.size(); fi++)
				{
					const fragment_program& fp = progs.fps[fi];
					const bool textured = fp.textures != 0;
					const auto& list = textured ? prof.textured : prof.untextured;
					const std::vector<u32> tds = textured ? td_combos(fp.textures, prof, TD) : std::vector<u32>{ 0 };
					size_t vi;
					if (auto it = fp_vp.find(fp.hash); it != fp_vp.end() && progs.vp_index.count(best(it->second))) vi = progs.vp_index[best(it->second)];
					else vi = rr_vp++ % progs.vps.size();
					for (const auto& hs : fp.states)
						for (int ti = 0; ti < K && ti < int(list.size()); ti++)
						{
							const state_template& t = list[size_t(ti)];
							fp_state st;
							st.ctrl = hs.shader_ctrl | t.ctrl_hi | ((textured && fp.bx2) ? 0x4000000u : 0u);
							st.tcc = hs.texcoord_mask;
							st.lighting = t.centroid ? u16(hs.centroid_mask << 1) : 0;
							st.mrt = t.mrt;
							for (int pj = 0; pj < N && pj < int(t.props.size()); pj++)
								for (size_t ci = 0; ci < tds.size(); ci++)
								{
									st.td = tds[ci];
									if (std::find(fp_states[fi].begin(), fp_states[fi].end(), st) == fp_states[fi].end()) fp_states[fi].push_back(st);
									entries.push_back({ int((ti * N + pj) * int(tds.size()) + int(ci)), fi, fi, vi, progs.vps[vi].output_mask, st, t.props[size_t(pj)] });
								}
						}
				}
				// every vertex program (and output mask) at least once, ahead of everything else
				size_t rr_fp = 0;
				for (size_t vi = 0; vi < progs.vps.size(); vi++)
				{
					const vertex_program& vp = progs.vps[vi];
					size_t fi;
					if (auto it = vp_fp.find(vp.hash); it != vp_fp.end() && progs.fp_index.count(best(it->second))) fi = progs.fp_index[best(it->second)];
					else fi = rr_fp++ % progs.fps.size();
					const fragment_program& fp = progs.fps[fi];
					const auto& list = fp.textures ? prof.textured : prof.untextured;
					if (list.empty() || fp_states[fi].empty()) continue;
					for (u32 mask : vp.output_masks) entries.push_back({ -1, vi, fi, vi, mask, fp_states[fi][0], list[0].props[0] });
				}
				for (const auto& s : fp_states) res.fp_variants += unsigned(s.size());

				// cached pipelines whose exact fragment state is generated
				for (const auto& r : eval.records)
				{
					res.cache_pipelines++;
					auto it = progs.fp_index.find(r.fragment_program_hash);
					if (it == progs.fp_index.end() || r.fp_shadow_textures || r.fp_redirected_textures || r.fp_multisampled_textures) continue;
					const fp_state st{ r.fp_ctrl, r.fp_texture_dimensions, r.fp_texcoord_control, r.fp_lighting_flags, r.fp_mrt_count };
					const auto& v = fp_states[it->second];
					if (std::find(v.begin(), v.end(), st) != v.end()) res.cache_fp_state_hits++;
				}

				// first-use order for the preload: VP coverage, then by template rank, engine (EBOOT) programs before levels
				std::stable_sort(entries.begin(), entries.end(), [](const entry& a, const entry& b) { return a.rank != b.rank ? a.rank < b.rank : a.order < b.order; });

				// ---- write
				const fs::path raw = out / "raw";
				const fs::path pipes = out / "pipelines" / "vulkan" / version;
				std::error_code ec;
				if (!o.dry_run)
				{
					fs::create_directories(raw, ec);
					fs::create_directories(pipes, ec);
					fs::remove_all(out / tmp_dir_name, ec); // leftovers of a killed run
					fs::create_directories(out / tmp_dir_name, ec);
					if (!fs::is_directory(pipes) || !fs::is_directory(out / tmp_dir_name)) return c.fail("cannot create %s", u8str(pipes).c_str());
				}
				const fs::path tmp = out / tmp_dir_name;
				std::ofstream manifest;
				if (!o.dry_run) manifest.open(out / manifest_name, std::ios::app);

				std::unordered_set<std::string> names;
				std::vector<std::pair<pipeline_record, std::string>> todo;
				for (const auto& e : entries)
				{
					pipeline_record r = make_record(progs.vps[e.vp], e.vp_output, progs.fps[e.fp], e.st, prof.props[size_t(e.props)]);
					std::string name = record_file_name(r);
					if (!names.insert(name).second) continue;
					if (existing.pipeline_names.count(name))
					{
						res.entries_existing++;
						continue;
					}
					todo.emplace_back(r, std::move(name));
				}
				res.entries_total = unsigned(todo.size() + res.entries_existing);

				// mtimes: after every recorded pipeline (those reflect real first use), 32 entries per second in order
				const auto now = fs::file_time_type::clock::now();
				const auto span = std::chrono::seconds(todo.size() / 32 + 1);
				auto t_base = now - span;
				if (existing.any && existing.newest + std::chrono::seconds(1) > t_base) t_base = existing.newest + std::chrono::seconds(1);

				std::unordered_set<u64> raw_done;
				unsigned n = 0;
				for (const auto& [r, name] : todo)
				{
					if (!o.dry_run)
					{
						const fragment_program& fp = progs.fps[progs.fp_index[r.fragment_program_hash]];
						const vertex_program& vp = progs.vps[progs.vp_index[r.vertex_program_hash]];
						char hn[40];
						if (raw_done.insert(fp.hash).second && !existing.raw_fp.count(fp.hash))
						{
							std::snprintf(hn, sizeof(hn), "%llX.fp", static_cast<unsigned long long>(fp.hash));
							if (!fs::exists(raw / hn, ec))
							{
								manifest << "R raw/" << hn << std::endl; // listed before it exists, so a kill never orphans a file
								if (!write_bytes(tmp, raw / hn, fp.ucode.data(), fp.ucode.size())) return c.fail("cannot write %s", u8str(raw / hn).c_str());
							}
						}
						if (raw_done.insert(vp.hash ^ 0x5650ull).second && !existing.raw_vp.count(vp.hash))
						{
							std::snprintf(hn, sizeof(hn), "%llX.vp", static_cast<unsigned long long>(vp.hash));
							if (!fs::exists(raw / hn, ec))
							{
								manifest << "R raw/" << hn << std::endl;
								if (!write_bytes(tmp, raw / hn, vp.data.data(), vp.data.size() * 4)) return c.fail("cannot write %s", u8str(raw / hn).c_str());
							}
						}
						const fs::path pp = pipes / name;
						const fs::file_time_type mt = t_base + std::chrono::seconds(n / 32);
						manifest << "P pipelines/vulkan/" << version << "/" << name << std::endl;
						if (!write_bytes(tmp, pp, &r, sizeof(r), &mt)) return c.fail("cannot write %s", u8str(pp).c_str());
					}
					res.entries_written++;
					if ((++n & 63) == 0) c.step("write", n, unsigned(todo.size()));
				}
				c.step("write", n, unsigned(todo.size()));

				if (!o.dry_run)
				{
					manifest.close();
					fs::remove_all(tmp, ec);
					// last: its presence means every entry above is on disk
					const fs::path st_tmp = out / "shaderscan.stamp.tmp";
					{
						std::ofstream stamp(st_tmp, std::ios::trunc);
						stamp << "shaderscan " << SHADERSCAN_VERSION << "\nfp " << res.fp_programs << "\nvp " << res.vp_programs << "\nvariants " << res.fp_variants
							  << "\nentries " << res.entries_total << "\n";
					}
					fs::rename(st_tmp, out / stamp_name, ec);
					if (ec) return c.fail("cannot write %s", u8str(out / stamp_name).c_str());
				}
				return 0;
			}

			int clean(backend_ctx& c) override
			{
				const fs::path& out = c.out;
				std::ifstream m(out / manifest_name);
				std::vector<std::string> lines;
				for (std::string l; std::getline(m, l);) if (l.size() > 2) lines.push_back(l);
				m.close();
				std::error_code ec;
				unsigned n = 0;
				for (const auto& l : lines)
				{
					fs::remove(out / fs::path(l.substr(2)), ec);
					if ((++n & 255) == 0) c.step("clean", n, unsigned(lines.size()));
				}
				fs::remove(out / manifest_name, ec);
				fs::remove(out / stamp_name, ec);
				c.step("clean", n, unsigned(lines.size()));
				c.log("removed %u generated files\n", n);
				return 0;
			}

			int needs_run(const fs::path& out) const override
			{
				std::ifstream f(out / stamp_name);
				std::string tool;
				int ver = 0;
				if (!(f >> tool >> ver)) return 1;
				return ver != SHADERSCAN_VERSION;
			}

			int verify(const shaderscan_opts& o, const fs::path& dir, std::FILE* rep) override
			{
				const std::string ver = o.cache_version && *o.cache_version ? o.cache_version : "v1.95";
				const verify_report v = verify_cache(dir, ver);
				std::fprintf(rep, "verify %s (pipelines\\vulkan\\%s)\n", u8str(dir).c_str(), ver.c_str());
				std::fprintf(rep, "  pipeline files %u: accepted by the loader %u, wrong size %u, raw program missing %u\n", v.files, v.accepted, v.bad_size,
					v.missing_raw);
				std::fprintf(rep, "  keys re-derived from content: %u file names disagree; %u programs fail analysis; %u bad jump tables\n", v.bad_name,
					v.bad_program, v.bad_jump_table);
				std::fprintf(rep, "  raw: %u fp, %u vp, %u unreferenced\n", v.raw_fp, v.raw_vp, v.raw_unreferenced);
				for (const auto& e : v.examples) std::fprintf(rep, "  ! %s\n", e.c_str());
				std::fprintf(rep, "%s\n", v.ok() ? "OK" : "FAILED");
				return v.ok() ? 0 : 1;
			}

			int emit_profile(const shaderscan_opts& o, std::FILE* f, const char* symbol) override
			{
				const char* src = o.learn_dir ? o.learn_dir : o.out_dir;
				if (!src) return -1;
				const std::string ver = o.cache_version && *o.cache_version ? o.cache_version : "v1.95";
				cache_contents cache;
				read_cache(to_path(src), ver, cache);
				const profile p = learn_profile(cache);
				shaderscan::emit_profile(p, f, symbol);
				std::printf("profile: %llu pipelines, %zu textured / %zu untextured templates, %zu states",
					static_cast<unsigned long long>(p.records), p.textured.size(), p.untextured.size(), p.props.size());
				return 0;
			}
		};
	}

	std::unique_ptr<backend> make_rpcs3_backend()
	{
		return std::make_unique<rpcs3_backend>();
	}
}
