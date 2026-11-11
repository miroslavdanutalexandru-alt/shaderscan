// shaderscan orchestration: inputs -> front ends -> program set -> back end (state variants + cache entries).
// SPDX-License-Identifier: GPL-2.0-only
#include "shaderscan.h"

#include "backend.h"
#include "psarc.h"
#include "rpcs3_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using namespace shaderscan;

namespace shaderscan
{
	fs::path to_path(const char* s)
	{
		return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s)));
	}

	std::string lower(std::string s)
	{
		for (char& c : s) c = char((c >= 'A' && c <= 'Z') ? c + 32 : c);
		std::replace(s.begin(), s.end(), '\\', '/');
		return s;
	}

	bool read_file(const fs::path& p, std::vector<u8>& out)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return false;
		f.seekg(0, std::ios::end);
		out.resize(size_t(f.tellg()));
		f.seekg(0);
		return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
	}

	std::string read_sfo_title(const char* game_dir)
	{
		std::string sfo_text;
		if (game_dir && *game_dir)
		{
			for (const char* rel : { "gamefiles/PS3_GAME/PARAM.SFO", "PS3_GAME/PARAM.SFO", "../PARAM.SFO", "PARAM.SFO" })
			{
				std::vector<u8> b;
				if (!read_file(to_path(game_dir) / rel, b) || b.size() < 20 || std::memcmp(b.data(), "\0PSF", 4) != 0) continue;
				const u32 kt = le32(&b[8]), dt = le32(&b[12]), n = le32(&b[16]);
				for (u32 i = 0; i < n && 20 + size_t(i) * 16 + 16 <= b.size(); i++)
				{
					const u8* e = &b[20 + size_t(i) * 16];
					const size_t ko = kt + size_t(e[0] | (e[1] << 8)), len = le32(e + 4), dof = dt + size_t(le32(e + 12));
					if (ko >= b.size() || dof + len > b.size()) continue;
					const std::string key(reinterpret_cast<const char*>(&b[ko]), strnlen(reinterpret_cast<const char*>(&b[ko]), b.size() - ko));
					if (key == "TITLE" || key == "TITLE_ID")
						sfo_text += std::string(reinterpret_cast<const char*>(&b[dof]), strnlen(reinterpret_cast<const char*>(&b[dof]), len)) + "|";
				}
				break;
			}
		}
		return lower(sfo_text);
	}

	void backend_ctx::log(const char* fmt, ...) const
	{
		if (!o.verbose) return;
		va_list ap;
		va_start(ap, fmt);
		std::vfprintf(stdout, fmt, ap);
		va_end(ap);
		std::fflush(stdout);
	}

	int backend_ctx::fail(const char* fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(r.message, sizeof(r.message), fmt, ap);
		va_end(ap);
		return SHADERSCAN_FAILED;
	}

	std::unique_ptr<backend> select_backend(const char* name, const char* game_dir, const char* out_dir, std::string* message)
	{
		const std::string n = name && *name ? lower(name) : "auto";
		bool r1 = n == "r1d3d12" || n == "r1";
		if (n == "auto")
		{
			const std::string s = read_sfo_title(game_dir);
			for (const char* id : { "bcus98107", "bces00001", "bcjs30001", "bcas20001", "fall of man" })
				if (s.find(id) != std::string::npos) r1 = true;
			if (!r1 && s.empty() && game_dir && *game_dir)
			{
				std::error_code ec;
				r1 = fs::exists(to_path(game_dir) / "game" / "game.elf", ec) && fs::exists(to_path(game_dir) / "game" / "EBOOT.elf", ec);
			}
			if (!r1 && out_dir && *out_dir)
			{
				std::error_code ec;
				const fs::path out = to_path(out_dir);
				r1 = fs::exists(out / "pso_manifest_game2.bin", ec) || fs::exists(out / "pso_manifest_fe2.bin", ec);
				// --check gets only --out; R1's cache is <root>\shadercache
				if (!r1 && !(game_dir && *game_dir))
					r1 = fs::exists(out.parent_path() / "game" / "game.elf", ec) && fs::exists(out.parent_path() / "game" / "EBOOT.elf", ec);
				for (fs::directory_iterator it(out, ec), end; !r1 && !ec && it != end; it.increment(ec))
					r1 = it->path().extension() == ".dxbc";
			}
		}
		else if (!r1 && n != "rpcs3")
		{
			if (message) *message = "unknown back end '" + n + "' (rpcs3 | r1d3d12 | auto)";
			return nullptr;
		}
		if (r1)
		{
#ifdef SHADERSCAN_HAVE_R1D3D12
			return make_r1d3d12_backend();
#else
			if (message) *message = "the r1d3d12 back end is not built in (configure with SHADERSCAN_R1_VIDEO_DIR)";
			return nullptr;
#endif
		}
		return make_rpcs3_backend();
	}
}

namespace
{
	// ---- input scan -----------------------------------------------------------------------------------------------

	bool skip_dir(const std::string& name)
	{
		const std::string n = lower(name);
		return n == ".git" || n == "shadercache" || n == "third_party" || n == "ps3recomp" || n == "recompiled" || n.rfind("build", 0) == 0 ||
			n == "logs" || n == "scratch";
	}

	void collect_inputs(const fs::path& dir, std::vector<fs::path>& archives, std::vector<fs::path>& loose, const frontend& fe, bool deep)
	{
		std::error_code ec;
		fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
		std::set<std::string> seen;
		for (; !ec && it != end; it.increment(ec))
		{
			if (it->is_directory(ec))
			{
				if (skip_dir(u8str(it->path().filename()))) it.disable_recursion_pending();
				continue;
			}
			const std::string lp = lower(u8str(it->path()));
			const uintmax_t size = it->file_size(ec);
			if (lp.size() > 6 && lp.compare(lp.size() - 6, 6, ".psarc") == 0)
			{
				// the same archive can exist in two install layouts (PS3_GAME\USRDIR and game\<id>\USRDIR)
				const size_t k = lp.rfind("/packed/");
				const std::string key = (k != std::string::npos ? lp.substr(k) : lower(u8str(it->path().filename()))) + "#" + std::to_string(size);
				if (seen.insert(key).second) archives.push_back(it->path());
			}
			else if (deep ? size < (256u << 20) : fe.wants_file(lp))
			{
				loose.push_back(it->path());
			}
		}
	}
}

extern "C" int shaderscan_needs_run(const char* out_dir)
{
	if (!out_dir) return 1;
	const auto b = select_backend(nullptr, nullptr, out_dir, nullptr);
	return b ? b->needs_run(to_path(out_dir)) : 1;
}

extern "C" int shaderscan_run(const shaderscan_opts* opts, shaderscan_progress_fn progress, void* user, shaderscan_result* result)
{
	shaderscan_result local{};
	shaderscan_result& res = result ? *result : local;
	res = {};
	if (!opts) return 1;
	const auto t0 = std::chrono::steady_clock::now();
	const shaderscan_opts& o = *opts;
	backend_ctx c{ o, {}, progress, user, res };
	if (!o.out_dir || !*o.out_dir) return c.fail("no output directory");
	const fs::path out = to_path(o.out_dir);
	c.out = out;
	std::string why;
	const std::unique_ptr<backend> be = select_backend(o.backend, o.game_dir, o.out_dir, &why);
	if (!be) return c.fail("%s", why.c_str());
	if (o.clean) return be->clean(c);

	if (const int rc = be->begin(c)) return rc;

	// ---- scan
	std::unique_ptr<frontend> fe;
	const std::string fe_name = o.frontend && *o.frontend ? o.frontend : "auto";
	if (fe_name == "auto" || fe_name == "insomniac" || fe_name == "insomniac_shaders_dat") fe = make_insomniac_frontend();
	else if (fe_name == "signature") fe = make_signature_frontend();
	else if (fe_name != "none") return c.fail("unknown front end '%s'", fe_name.c_str());
	const std::string disc = o.discovery && *o.discovery ? lower(o.discovery) : "both";
	if (disc != "both" && disc != "tables" && disc != "scan") return c.fail("unknown discovery '%s' (tables | scan | both)", disc.c_str());
	if (fe) fe->set_discovery(disc == "tables" ? discovery_mode::tables : disc == "scan" ? discovery_mode::scan : discovery_mode::both);

	program_set progs;
	if (fe)
	{
		// the EBOOT, plus Fall of Man's second executable (game\game.elf = game.self, the in-game ELF)
		std::vector<std::pair<fs::path, std::string>> exes;
		if (o.eboot && *o.eboot) exes.push_back({ to_path(o.eboot), "EBOOT" });
		else if (o.game_dir && fs::exists(to_path(o.game_dir) / "game" / "EBOOT.elf")) exes.push_back({ to_path(o.game_dir) / "game" / "EBOOT.elf", "EBOOT" });
		if (o.game_dir && *o.game_dir && fs::exists(to_path(o.game_dir) / "game" / "game.elf"))
			exes.push_back({ to_path(o.game_dir) / "game" / "game.elf", "game.elf" });

		std::vector<fs::path> archives, loose;
		if (o.game_dir && *o.game_dir)
		{
			fs::path g = to_path(o.game_dir);
			if (fs::exists(g / "gamefiles")) g /= "gamefiles";
			collect_inputs(g, archives, loose, *fe, o.deep != 0);
		}
		const unsigned total = unsigned(archives.size() + loose.size() + exes.size());
		unsigned done = 0;
		std::vector<u8> buf;
		for (const auto& [exe, label] : exes)
		{
			if (!read_file(exe, buf)) return c.fail("cannot read %s", u8str(exe).c_str());
			const size_t n = fe->scan(buf.data(), buf.size(), label == "EBOOT" ? "eboot" : label, progs);
			c.log("%s: %zu programs\n", label.c_str(), n);
			c.step("scan", ++done, total);
		}
		// loose files grouped by directory (first-seen order kept), so a front end can pair files of one directory
		std::map<fs::path, size_t> dir_rank;
		for (const auto& p : loose) dir_rank.emplace(p.parent_path(), dir_rank.size());
		std::stable_sort(loose.begin(), loose.end(),
			[&](const fs::path& a, const fs::path& b) { return dir_rank.at(a.parent_path()) < dir_rank.at(b.parent_path()); });
		for (size_t i = 0; i < loose.size(); i++)
		{
			const fs::path& p = loose[i];
			if (i == 0 || p.parent_path() != loose[i - 1].parent_path()) fe->begin_group(u8str(p.parent_path()));
			if (read_file(p, buf))
			{
				const size_t n = fe->scan(buf.data(), buf.size(), u8str(p.filename()), progs);
				if (n) c.log("%s: %zu programs\n", u8str(p).c_str(), n);
			}
			if (i + 1 == loose.size() || p.parent_path() != loose[i + 1].parent_path())
			{
				const size_t n = fe->end_group(progs);
				if (n) c.log("%s: %zu programs\n", u8str(p.parent_path()).c_str(), n);
			}
			c.step("scan", ++done, total);
		}
		for (const auto& p : archives)
		{
			psarc a;
			if (a.open(p))
			{
				fe->begin_group(u8str(p));
				for (size_t i = 0; i < a.count(); i++)
				{
					const std::string lp = lower(a.name(i));
					if (o.deep ? a.size(i) >= (256u << 20) : !fe->wants_file(lp)) continue;
					if (!a.read(i, buf)) continue;
					const size_t n = fe->scan(buf.data(), buf.size(), a.name(i), progs);
					if (n) c.log("%s:%s: %zu programs (%zu FP / %zu VP total)\n", u8str(p.filename()).c_str(), a.name(i).c_str(), n,
						progs.fps.size(), progs.vps.size());
				}
				if (const size_t n = fe->end_group(progs))
					c.log("%s: %zu programs (%zu FP / %zu VP total)\n", u8str(p).c_str(), n, progs.fps.size(), progs.vps.size());
			}
			else c.log("skipping %s (not a zlib psarc)\n", u8str(p).c_str());
			c.step("scan", ++done, total);
		}
		if (const size_t n = fe->finish(progs)) c.log("%s tables: %zu programs (%zu FP / %zu VP total)\n", fe->name(), n, progs.fps.size(), progs.vps.size());
		if (const std::string rep = fe->report(); !rep.empty()) c.log("%s", rep.c_str());
	}
	for (unsigned i = 0; i < o.extra_count; i++)
	{
		const shaderscan_program& e = o.extra[i];
		if (!e.ucode || !e.size) continue;
		if (e.kind == SHADERSCAN_FP)
			progs.add_fp(static_cast<const u8*>(e.ucode), e.size, e.fp_texcoord_mask, e.fp_centroid_mask, e.fp_shader_ctrl, "extra");
		else
			progs.add_vp(static_cast<const u32*>(e.ucode), e.size / 16, e.vp_entry, e.vp_output_mask, "extra");
	}
	res.fp_programs = unsigned(progs.fps.size());
	res.vp_programs = unsigned(progs.vps.size());
	if (progs.fps.empty() || progs.vps.empty())
	{
		// unsupported game or no game files: stamp it so a launcher does not retry on every start
		if (!o.dry_run)
		{
			std::error_code ec;
			fs::create_directories(out, ec);
			std::ofstream stamp(out / stamp_name, std::ios::trunc);
			stamp << "shaderscan " << SHADERSCAN_VERSION << "\nnothing-found\n";
		}
		c.fail("no programs found (%zu FP, %zu VP)", progs.fps.size(), progs.vps.size());
		return SHADERSCAN_NOTHING_FOUND;
	}
	for (size_t i = 0; i < progs.fps.size(); i++) be->emit_program(c, { progs, SHADERSCAN_FP, i });
	for (size_t i = 0; i < progs.vps.size(); i++) be->emit_program(c, { progs, SHADERSCAN_VP, i });
	for (unsigned i = 0; i < o.pipeline_count; i++)
		if (o.pipelines[i].data && o.pipelines[i].size) be->emit_pipeline(c, { o.pipelines[i].data, o.pipelines[i].size });

	if (const int rc = be->finish(c, progs)) return rc;
	res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	return 0;
}

extern "C" int shaderscan_verify(const shaderscan_opts* opts, const char* dir)
{
	if (!opts || !dir) return SHADERSCAN_FAILED;
	std::string why;
	const auto be = select_backend(opts->backend, opts->game_dir, dir, &why);
	if (!be)
	{
		std::fprintf(stderr, "shaderscan: %s\n", why.c_str());
		return SHADERSCAN_FAILED;
	}
	return be->verify(*opts, to_path(dir), stdout);
}
