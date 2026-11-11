// shaderscan command line.
// SPDX-License-Identifier: GPL-2.0-only
#include "shaderscan.h"

#include "backend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

#ifndef SHADERSCAN_RELEASE
#define SHADERSCAN_RELEASE "dev"
#endif

static void usage()
{
	std::puts(
		"shaderscan - seed a recomp's shader cache from your own game files (offline, no playthrough)\n"
		"\n"
		"usage: shaderscan --game <port or USRDIR dir> --out <shader cache dir> [options]\n"
		"  --backend <name>     auto (default) | rpcs3 (RPCS3 Vulkan ...\\shadercache\\rpcs3\\shaders_cache)\n"
		"                       | r1d3d12 (Fall of Man D3D12 <port>\\shadercache)\n"
		"  --eboot <file>       decrypted EBOOT.elf (default <game>\\game\\EBOOT.elf)\n"
		"  --frontend <name>    insomniac (default) | signature | none\n"
		"  --discovery <mode>   how level containers are searched: both (default: byte-scan + index tables,\n"
		"                       deduplicated) | tables (index tables; byte-scan only where unusable) | scan\n"
		"  --profile <name>     embedded state profile: r3 | r2 | generic | auto (default, from PARAM.SFO)\n"
		"                       | none (r1d3d12: learned states only)\n"
		"  --learn <dir|->      shaders_cache to learn state templates from (default: --out; '-' = embedded only)\n"
		"  --variants <K>       state templates per fragment program (default 6; r1d3d12: 4)\n"
		"  --props <N>          pipeline states per fragment variant (default 1)\n"
		"  --td <M>             texture-dimension combinations per variant (default 2)\n"
		"  --cache-version <v>  pipeline directory version (default v1.95)\n"
		"  --deep               scan every archived file, not only shader containers\n"
		"  --dry-run            count only\n"
		"  --clean              remove entries written by earlier runs\n"
		"  --progress           machine-readable progress on stdout (@progress/@done/@error lines)\n"
		"  --check              exit 0 if the cache is already seeded by this version, 1 otherwise\n"
		"  --verify <dir>       load a cache like the renderer does and re-derive every key; exit 0 when consistent\n"
		"  --emit-profile <f> <symbol>   write the back end's profile learned from --learn as a C include\n"
		"  -q                   quiet\n"
		"  --version            print the release, the cache stamp version and the built-in back ends\n"
		"\n"
		"exit codes: 0 done, 1 failed, 2 usage, 3 no supported programs found (stamp written)");
}

namespace
{
	bool g_machine = false;

	void on_progress(const char* phase, unsigned done, unsigned total, void*)
	{
		if (g_machine)
		{
			// at most ~20 lines per second, but always the first and last line of a phase
			static std::string last_phase;
			static auto last = std::chrono::steady_clock::time_point{};
			const auto now = std::chrono::steady_clock::now();
			if (last_phase == phase && done != total && now - last < std::chrono::milliseconds(50)) return;
			last_phase = phase;
			last = now;
			std::printf("@progress %s %u %u\n", phase, done, total);
			std::fflush(stdout);
			return;
		}
		std::fprintf(stderr, "\r[%s] %u/%u   ", phase, done, total);
		if (done == total) std::fputc('\n', stderr);
	}

	void error_line(const char* msg)
	{
		if (g_machine)
		{
			std::string m = msg;
			for (char& c : m) if (c == '\n' || c == '\r') c = ' ';
			std::printf("@error %s\n", m.c_str());
			std::fflush(stdout);
		}
		std::fprintf(stderr, "shaderscan: %s\n", msg);
	}

	fs::path u8path(const char* s)
	{
		return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s)));
	}

	int run(int argc, char** argv)
	{
		shaderscan_opts o{};
		o.verbose = 1;
		const char* emit_file = nullptr;
		const char* emit_symbol = nullptr;
		const char* verify_dir = nullptr;
		bool check = false;
		for (int i = 1; i < argc; i++)
		{
			const std::string a = argv[i];
			auto next = [&]() -> const char* { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
			if (a == "--game") o.game_dir = next();
			else if (a == "--eboot") o.eboot = next();
			else if (a == "--out") o.out_dir = next();
			else if (a == "--learn") o.learn_dir = next();
			else if (a == "--frontend") o.frontend = next();
			else if (a == "--discovery") o.discovery = next();
			else if (a == "--backend") o.backend = next();
			else if (a == "--profile") o.profile = next();
			else if (a == "--cache-version") o.cache_version = next();
			else if (a == "--variants") o.variants = std::atoi(next());
			else if (a == "--props") o.props = std::atoi(next());
			else if (a == "--td") o.td_max = std::atoi(next());
			else if (a == "--deep") o.deep = 1;
			else if (a == "--dry-run") o.dry_run = 1;
			else if (a == "--clean") o.clean = 1;
			else if (a == "--check") check = true;
			else if (a == "--progress") g_machine = true;
			else if (a == "--verify") verify_dir = next();
			else if (a == "-q") o.verbose = 0;
			else if (a == "--emit-profile") { emit_file = next(); emit_symbol = next(); }
			else if (a == "-h" || a == "--help") { usage(); return 0; }
			else if (a == "--version")
			{
				// line 2 is the cache stamp version: a change makes --check ask for a rescan
				std::printf("shaderscan %s\nSHADERSCAN_VERSION %d\nback ends: rpcs3%s\n", SHADERSCAN_RELEASE, SHADERSCAN_VERSION,
#ifdef SHADERSCAN_HAVE_R1D3D12
					" r1d3d12"
#else
					""
#endif
				);
				return 0;
			}
			else { usage(); return 2; }
		}
		if (o.backend)
		{
			const std::string b = shaderscan::lower(o.backend);
			if (b != "auto" && b != "rpcs3" && b != "r1d3d12" && b != "r1")
			{
				usage();
				error_line("usage: --backend must be rpcs3, r1d3d12 or auto");
				return 2;
			}
		}

		if (o.discovery)
		{
			const std::string d = shaderscan::lower(o.discovery);
			if (d != "both" && d != "tables" && d != "scan")
			{
				usage();
				error_line("usage: --discovery must be tables, scan or both");
				return 2;
			}
		}

		if (check) return shaderscan_needs_run(o.out_dir);

		if (verify_dir)
		{
			const int rc = shaderscan_verify(&o, verify_dir);
			return rc < 0 ? SHADERSCAN_FAILED : rc;
		}

		if (emit_file)
		{
			if (!o.learn_dir && !o.out_dir) { usage(); return 2; }
			std::string why;
			const auto be = shaderscan::select_backend(o.backend, o.game_dir, o.learn_dir ? o.learn_dir : o.out_dir, &why);
			if (!be) { error_line(why.c_str()); return SHADERSCAN_FAILED; }
			std::FILE* f = std::fopen(emit_file, "w");
			if (!f) { std::perror(emit_file); return 1; }
			const int rc = be->emit_profile(o, f, emit_symbol);
			std::fclose(f);
			if (rc) { error_line("this back end has no profile to emit"); return SHADERSCAN_FAILED; }
			std::printf(" -> %s\n", emit_file);
			return 0;
		}
		if (!o.out_dir || (!o.clean && !o.game_dir && !o.eboot))
		{
			usage();
			if (g_machine) error_line("usage: --out and --game (or --eboot) are required");
			return 2;
		}

		shaderscan_result r{};
		const int rc = shaderscan_run(&o, (o.verbose || g_machine) ? on_progress : nullptr, nullptr, &r);
		if (rc == SHADERSCAN_NOTHING_FOUND)
		{
			std::fprintf(stderr, "shaderscan: %s\n", r.message);
			if (g_machine) { std::printf("@done 0 0\n"); std::fflush(stdout); }
			return rc;
		}
		if (rc)
		{
			error_line(r.message[0] ? r.message : "failed");
			return SHADERSCAN_FAILED;
		}
		if (o.clean)
		{
			if (g_machine) { std::printf("@done 0 0\n"); std::fflush(stdout); }
			return 0;
		}
		auto pct = [](unsigned a, unsigned b) { return b ? 100.0 * a / b : 0.0; };
		std::printf("programs: %u fragment, %u vertex; %u fragment variants\n", r.fp_programs, r.vp_programs, r.fp_variants);
		std::printf("recorded cache: FP %u/%u (%.1f%%), VP %u/%u (%.1f%%) found; %u/%u pipelines (%.1f%%) have a generated fragment state\n",
			r.cache_fp_covered, r.cache_fp, pct(r.cache_fp_covered, r.cache_fp), r.cache_vp_covered, r.cache_vp, pct(r.cache_vp_covered, r.cache_vp),
			r.cache_fp_state_hits, r.cache_pipelines, pct(r.cache_fp_state_hits, r.cache_pipelines));
		std::printf("pipeline entries: %u total, %u %s, %u already present (%.1f s)\n", r.entries_total, r.entries_written,
			o.dry_run ? "would be written" : "written", r.entries_existing, r.seconds);
		if (g_machine) std::printf("@done %u %u\n", r.entries_written, r.entries_total);
		std::fflush(stdout);
		return 0;
	}
}

#ifdef _WIN32
// Wide entry point: paths with characters outside the ANSI code page reach the API as UTF-8
int wmain(int argc, wchar_t** wargv)
{
	std::vector<std::string> args;
	std::vector<char*> argv;
	for (int i = 0; i < argc; i++)
	{
		const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
		if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
		args.push_back(std::move(s));
	}
	for (auto& s : args) argv.push_back(s.data());
	argv.push_back(nullptr);
	return run(argc, argv.data());
}
#else
int main(int argc, char** argv)
{
	return run(argc, argv);
}
#endif
