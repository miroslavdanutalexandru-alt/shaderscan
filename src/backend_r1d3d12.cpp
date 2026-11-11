// shaderscan back end "r1d3d12": Resistance: Fall of Man port's own D3D12 renderer (ResistancePort).
// SPDX-License-Identifier: GPL-2.0-only
//
// Runtime cache (rsx_d3d12_backend.c shc_compile_ex): <port>\shadercache\<key>.dxbc, key = FNV-1a 64 over
// "rfom-shc-v1", target\0, "main"\0, HLSL; the HLSL is kept next to it as <key>.hlsl (the launcher's "Rebuild shader
// cache" recompiles a .dxbc from it). No version/driver check on .dxbc files: the key is the whole validation. FXC
// (d3dcompiler_47 D3DCompile, flags 0, ps_5_0 / vs_5_0). Pipelines (pso_manifest_*2.bin) are recorded by the game and
// left alone.
//
// The HLSL is produced by R1's own translator (rsx_fp_decompiler.c / rsx_vp_decompiler.c compiled into this tool)
// plus r1d3d12/r1_glue.c (the post-processing of vp_get_fp_pso). A fragment program's text also depends on draw state
// (shadow compare, sign expansion, texel-addressed sizes, clamp / mirror addressing): those "variants" are learned
// from recorded .hlsl files (parsed back into the state that produced them, then confirmed by key), from the
// embedded profile, and the stateless base variant is always generated.
#include "backend.h"

#include "r1d3d12/r1_glue.h"
#include "rpcs3_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>

#if __has_include("../profiles/r1_profile.inc")
#include "../profiles/r1_profile.inc"
#define SHADERSCAN_HAVE_R1_PROFILE 1
#endif

namespace fs = std::filesystem;

namespace shaderscan
{
	namespace
	{
		constexpr const char* k_backend_tag = "backend r1d3d12";
		// Translator settings that change the generated text (rsx_fp_decompiler.c / vp_get_fp_pso getenv calls).
		const char* const k_env_keys[] = { "RSX_RES_SCALE", "RSX_FP_KSLOTS", "RSX_ALPHATEST", "FP_SWAP_RB", "RSX_FP_DEBUG", "RSX_FSR", "RSX_FSR_REACTIVE" };

		// ---- the game's environment: run.bat's / settings.bat's `set "K=V"` lines win over this process's ----------

		void read_bat(const fs::path& p, std::map<std::string, std::string>& env)
		{
			std::ifstream f(p);
			for (std::string l; std::getline(f, l);)
			{
				size_t i = l.find_first_not_of(" \t");
				if (i == std::string::npos || lower(l.substr(i, 4)) != "set ") continue;
				std::string kv = l.substr(i + 4);
				kv.erase(0, kv.find_first_not_of(" \t"));
				if (!kv.empty() && kv.front() == '"')
				{
					const size_t q = kv.find('"', 1);
					kv = kv.substr(1, q == std::string::npos ? std::string::npos : q - 1);
				}
				const size_t eq = kv.find('=');
				if (eq == std::string::npos || eq == 0) continue;
				std::string k = kv.substr(0, eq);
				if (k.rfind("CHILD_", 0) == 0) k = k.substr(6); // run.bat passes CHILD_X to the game process as X
				env[k] = kv.substr(eq + 1);
			}
		}

		std::map<std::string, std::string> game_env(const fs::path& root)
		{
			std::map<std::string, std::string> bat, env;
			read_bat(root / "run.bat", bat);
			read_bat(root / "settings.bat", bat);
			for (const char* k : k_env_keys)
			{
				if (auto it = bat.find(k); it != bat.end()) env[k] = it->second;
				else if (const char* v = std::getenv(k)) env[k] = v;
			}
			return env;
		}

		std::string env_fingerprint(const std::map<std::string, std::string>& env)
		{
			std::string s;
			for (const char* k : k_env_keys)
			{
				auto it = env.find(k);
				s += std::string(k) + "=" + (it != env.end() ? it->second : "") + ";";
			}
			return s;
		}

		float fsr_reactive(const std::map<std::string, std::string>& env)
		{
			// rsx_fsr.inc fsr_rx_val
			auto it = env.find("RSX_FSR_REACTIVE");
			float v = it != env.end() ? float(std::atof(it->second.c_str())) : 0.6f;
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			return v;
		}

		fs::path port_root(const shaderscan_opts& o, const fs::path& out)
		{
			if (o.game_dir && *o.game_dir) return to_path(o.game_dir);
			return out.parent_path();
		}

		double res_inv_of(const std::map<std::string, std::string>& env)
		{
			// rsx_fp_decompiler.c fp_res_inv
			auto it = env.find("RSX_RES_SCALE");
			double v = it != env.end() ? std::atof(it->second.c_str()) : 1.0;
			if (!(v >= 1.0)) v = 1.0;
			if (v > 4.0) v = 4.0;
			return 1.0 / v;
		}

		std::string g9(double d)
		{
			char b[48];
			std::snprintf(b, sizeof(b), "%.9g", d);
			return b;
		}

		// ---- keys / compiler ----------------------------------------------------------------------------------------

		u64 shc_key(const char* target, const std::string& src) { return r1_shc_key(target, "main", src.data(), src.size()); }

		std::string key_name(u64 k)
		{
			char b[24];
			std::snprintf(b, sizeof(b), "%016llX", static_cast<unsigned long long>(k));
			return b;
		}

		bool parse_key(const std::string& stem, u64& k)
		{
			if (stem.size() != 16) return false;
			char* end = nullptr;
			k = std::strtoull(stem.c_str(), &end, 16);
			return end && !*end;
		}

		using d3dcompile_fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
			ID3DBlob**, ID3DBlob**);

		d3dcompile_fn load_fxc()
		{
			// the game links d3dcompiler_47 from the system; load the same one
			static d3dcompile_fn fn = [] {
				HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
				if (!m) m = LoadLibraryW(L"d3dcompiler_47.dll");
				return m ? reinterpret_cast<d3dcompile_fn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
			}();
			return fn;
		}

		// shc_compile_ex: D3DCompile(src, len, name, NULL, NULL, "main", target, 0 (RSX_FXC_OPT=1), 0)
		bool fxc(const std::string& src, const char* name, const char* target, std::vector<u8>& out)
		{
			const d3dcompile_fn fn = load_fxc();
			if (!fn) return false;
			ID3DBlob* code = nullptr;
			ID3DBlob* err = nullptr;
			const HRESULT hr = fn(src.data(), src.size(), name, nullptr, nullptr, "main", target, 0, 0, &code, &err);
			if (err) err->Release();
			if (FAILED(hr) || !code) return false;
			const u8* p = static_cast<const u8*>(code->GetBufferPointer());
			out.assign(p, p + code->GetBufferSize());
			code->Release();
			return true;
		}

		const char* source_name(const char* target, const std::string& src)
		{
			if (std::strcmp(target, "ps_5_0") == 0) return "guest_fp";
			return src.find("ByteAddressBuffer vp_raw") != std::string::npos ? "guest_vp_gf" : "guest_vp2";
		}

		bool write_atomic(const fs::path& p, const void* data, size_t n)
		{
			fs::path tmp = p;
			tmp += ".shaderscan.tmp";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				if (!f || !f.write(static_cast<const char*>(data), std::streamsize(n))) return false;
			}
			std::error_code ec;
			fs::rename(tmp, p, ec);
			if (!ec) return true;
			fs::remove(tmp, ec);
			return false;
		}

		std::string read_text(const fs::path& p)
		{
			std::ifstream f(p, std::ios::binary);
			std::ostringstream s;
			s << f.rdbuf();
			return s.str();
		}

		// ---- fragment-program variants --------------------------------------------------------------------------------

		r1_fp_variant zero_variant()
		{
			r1_fp_variant v;
			std::memset(&v, 0, sizeof(v));
			return v;
		}

		// Units whose state changes a sampled texture's code (clamp / mirror bits on unsampled units only add helpers).
		u32 variant_units(const r1_fp_variant& v)
		{
			u32 m = v.unnorm_mask | v.cube_mask;
			for (int u = 0; u < 16; u++)
				if (v.shadow[u][0] || v.expand[u]) m |= 1u << u;
			return m;
		}

		std::string format_variant(const r1_fp_variant& v)
		{
			std::string s = "x" + std::to_string(v.exp32);
			char b[96];
			if (v.cube_mask) { std::snprintf(b, sizeof(b), " c%X", v.cube_mask); s += b; }
			for (int u = 0; u < 16; u++)
				if (v.shadow[u][0]) { std::snprintf(b, sizeof(b), " s%d.%u.%u.%u", u, v.shadow[u][1], v.shadow[u][2], v.shadow[u][3]); s += b; }
			for (int u = 0; u < 16; u++)
				if (v.expand[u]) { std::snprintf(b, sizeof(b), " e%d.%u", u, v.expand[u]); s += b; }
			for (int u = 0; u < 16; u++)
				if (v.unnorm_mask & (1u << u))
				{
					std::snprintf(b, sizeof(b), " n%d.%u.%u.%u.%u", u, v.unnorm_dim[u][0], v.unnorm_dim[u][1], v.unnorm_decl[u][0], v.unnorm_decl[u][1]);
					s += b;
				}
			if (v.clamp_mask) { std::snprintf(b, sizeof(b), " k%X", v.clamp_mask); s += b; }
			if (v.mirror_mask)
			{
				std::snprintf(b, sizeof(b), " M%X", v.mirror_mask);
				s += b;
				for (int u = 0; u < 16; u++)
					if (v.mirror[u]) { std::snprintf(b, sizeof(b), " m%d.%u", u, v.mirror[u]); s += b; }
			}
			return s;
		}

		bool parse_variant_text(const std::string& text, r1_fp_variant& v)
		{
			v = zero_variant();
			std::istringstream in(text);
			for (std::string t; in >> t;)
			{
				unsigned a[5] = {};
				const char c = t[0];
				const char* p = t.c_str() + 1;
				switch (c)
				{
				case 'x': v.exp32 = u8(std::strtoul(p, nullptr, 10)); break;
				case 'c': v.cube_mask = u32(std::strtoul(p, nullptr, 16)); break;
				case 'k': v.clamp_mask = u32(std::strtoul(p, nullptr, 16)); break;
				case 'M': v.mirror_mask = u32(std::strtoul(p, nullptr, 16)); break;
				case 's':
					if (std::sscanf(p, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4 || a[0] > 15) return false;
					v.shadow[a[0]][0] = 1; v.shadow[a[0]][1] = u8(a[1]); v.shadow[a[0]][2] = u8(a[2]); v.shadow[a[0]][3] = u8(a[3]);
					break;
				case 'e':
					if (std::sscanf(p, "%u.%u", &a[0], &a[1]) != 2 || a[0] > 15) return false;
					v.expand[a[0]] = u8(a[1]);
					break;
				case 'n':
					if (std::sscanf(p, "%u.%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3], &a[4]) != 5 || a[0] > 15) return false;
					v.unnorm_mask |= 1u << a[0];
					v.unnorm_dim[a[0]][0] = a[1]; v.unnorm_dim[a[0]][1] = a[2]; v.unnorm_decl[a[0]][0] = a[3]; v.unnorm_decl[a[0]][1] = a[4];
					break;
				case 'm':
					if (std::sscanf(p, "%u.%u", &a[0], &a[1]) != 2 || a[0] > 15) return false;
					v.mirror[a[0]] = u8(a[1]);
					break;
				default: return false;
				}
			}
			return true;
		}

		size_t match_paren(const std::string& h, size_t open)
		{
			int depth = 0;
			for (size_t q = open; q < h.size(); q++)
			{
				if (h[q] == '(') depth++;
				else if (h[q] == ')' && !--depth) return q;
			}
			return std::string::npos;
		}

		bool ends_with(const std::string& s, size_t end, const std::string& suf)
		{
			return end >= suf.size() && s.compare(end - suf.size(), suf.size(), suf) == 0;
		}

		// The draw state that turned the translator's output into this recorded fragment HLSL (exp32 aside: tried both).
		r1_fp_variant learn_variant(const std::string& h)
		{
			r1_fp_variant v = zero_variant();
			static const char* const cmp[] = { "0.0", "reference > depth", "reference == depth", "reference >= depth", "reference < depth",
				"reference != depth", "reference <= depth", "1.0" };
			u32 sampled = 0;
			for (int u = 0; u < 16; u++)
			{
				const std::string us = std::to_string(u);
				// shadow compare (fp_shadow_helper)
				const std::string pre = "float _rsx_shadow_cmp" + us + "(float reference, float depth) { return (float)(";
				if (size_t p = h.find(pre); p != std::string::npos)
				{
					const size_t e = h.find("); }\n", p + pre.size());
					const std::string c = h.substr(p + pre.size(), e == std::string::npos ? 0 : e - p - pre.size());
					for (u8 f = 0; f < 8; f++)
						if (c == cmp[f]) v.shadow[u][1] = f;
					v.shadow[u][0] = 1;
					const size_t b = h.find("float4 _rsx_shadow" + us + "(float3 coordinate)");
					const size_t be = b == std::string::npos ? std::string::npos : h.find("\n}\n", b);
					const std::string blk = b == std::string::npos ? std::string() : h.substr(b, be == std::string::npos ? std::string::npos : be - b);
					v.shadow[u][2] = blk.find("float c0 = _rsx_shadow_cmp") != std::string::npos;
					v.shadow[u][3] = blk.find("coordinate.xy / (size * rsx_shscale.xy)") != std::string::npos;
					sampled |= 1u << u;
				}
				if (h.find("TextureCube  rsx_tex" + us + " ") != std::string::npos || h.find("TextureCube rsx_tex" + us + " ") != std::string::npos ||
					h.find("TextureCube  rsx_tex" + us + ":") != std::string::npos || h.find("TextureCube rsx_tex" + us + ":") != std::string::npos)
					v.cube_mask |= 1u << u;

				// texture call sites: plain, separate declaration, clamped (rsx_sampc), mirrored (rsx_sampm)
				const std::string forms[5] = {
					"rsx_tex[" + us + "].Sample(rsx_samp[" + us + "],",
					"rsx_tex" + us + ".Sample(rsx_samp[" + us + "],",
					"rsx_sampc(rsx_tex[" + us + "], rsx_samp[" + us + "],",
					"rsx_sampc(rsx_tex" + us + ", rsx_samp[" + us + "],",
					"rsx_sampm(rsx_tex[" + us + "], rsx_samp[" + us + "],",
				};
				for (int fi = 0; fi < 5; fi++)
					for (size_t at = h.find(forms[fi]); at != std::string::npos; at = h.find(forms[fi], at + 1))
					{
						sampled |= 1u << u;
						if (fi == 2 || fi == 3) v.clamp_mask |= 1u << u;
						const size_t close = match_paren(h, h.find('(', at));
						if (close == std::string::npos) continue;
						size_t end = close; // body = [at, end)
						if (fi == 4)
						{
							v.mirror_mask |= 1u << u;
							const size_t t = h.rfind(", float4(", close);
							int a = 0, b = 0, c = 0, d = 0;
							if (t != std::string::npos && t > at && std::sscanf(h.c_str() + t, ", float4(%d, %d, %d, %d)", &a, &b, &c, &d) == 4)
							{
								v.mirror[u] = u8((a & 1) | ((b & 1) << 1) | ((c & 1) << 2) | ((d & 1) << 3));
								end = t;
							}
						}
						// " * float2(1/w, 1/h)" appended inside the call (rsx_fp_apply_unnorm_scale)
						if (end > at && h[end - 1] == ')')
						{
							const size_t f2 = h.rfind(" * float2(", end);
							double a = 0, b = 0;
							if (f2 != std::string::npos && f2 > at && h.find(')', f2) == end - 1 &&
								std::sscanf(h.c_str() + f2, " * float2(%lf, %lf)", &a, &b) == 2 && a > 0 && b > 0)
							{
								v.unnorm_mask |= 1u << u;
								v.unnorm_dim[u][0] = u32(std::lround(1.0 / a));
								v.unnorm_dim[u][1] = u32(std::lround(1.0 / b));
								const std::string body = h.substr(at, f2 - at);
								const size_t dc = body.rfind("), float2(0.5, 0.5), float2(");
								double x = 0, y = 0;
								if (dc != std::string::npos && std::sscanf(body.c_str() + dc, "), float2(0.5, 0.5), float2(%lf, %lf)", &x, &y) == 2)
								{
									v.unnorm_decl[u][0] = u32(std::lround(x + 0.5));
									v.unnorm_decl[u][1] = u32(std::lround(y + 0.5));
								}
							}
						}
					}
			}
			// sign expansion: _rsx_expand_input(<fetch>, <mask>u, false)
			for (size_t p = h.find("_rsx_expand_input("); p != std::string::npos; p = h.find("_rsx_expand_input(", p + 1))
			{
				const size_t i = p + 18;
				if (h.compare(i, 9, "float4 v,") == 0) continue; // the helper itself
				int depth = 0;
				size_t q = i;
				for (; q < h.size(); q++)
				{
					if (h[q] == '(') depth++;
					else if (h[q] == ')') { if (!depth) break; depth--; }
					else if (h[q] == ',' && !depth) break;
				}
				if (q >= h.size() || h[q] != ',') continue;
				unsigned mask = 0;
				char bx2[8] = {};
				if (std::sscanf(h.c_str() + q, ", %uu, %5[a-z]", &mask, bx2) != 2 || std::strcmp(bx2, "false") != 0) continue;
				const std::string fetch = h.substr(i, q - i);
				int unit = -1;
				if (size_t t = fetch.find("rsx_tex["); t != std::string::npos) unit = std::atoi(fetch.c_str() + t + 8);
				else
					for (size_t t = fetch.find("rsx_tex"); t != std::string::npos && unit < 0; t = fetch.find("rsx_tex", t + 1))
						if (t + 7 < fetch.size() && fetch[t + 7] >= '0' && fetch[t + 7] <= '9') unit = std::atoi(fetch.c_str() + t + 7);
				if (unit >= 0 && unit < 16) { v.expand[unit] = u8(mask); sampled |= 1u << unit; }
			}
			// a clamp / mirror mask that only names unsampled units still adds its helper function
			auto unused_unit = [&]() -> u32 {
				for (int u = 15; u >= 0; u--)
					if (!(sampled & (1u << u)) && h.find("rsx_tex[" + std::to_string(u) + "]") == std::string::npos) return 1u << u;
				return 0;
			};
			if (!v.clamp_mask && h.find("float4 rsx_sampc(Texture2D t") != std::string::npos) v.clamp_mask = unused_unit();
			if (!v.mirror_mask && h.find("float4 rsx_sampm(Texture2D t") != std::string::npos) v.mirror_mask = unused_unit();
			return v;
		}

		// RSX_RES_SCALE baked into a recorded fragment HLSL: 0 = none (scale-independent text)
		double recorded_res_inv(const std::string& h)
		{
			const size_t w = h.find("#define _rsx_wpos(p) float4((p).x");
			if (w != std::string::npos)
			{
				double x = 1.0;
				return std::sscanf(h.c_str() + w, "#define _rsx_wpos(p) float4((p).x * %lf", &x) == 1 ? x : 1.0;
			}
			const size_t s = h.find("    size *= ");
			if (s != std::string::npos)
			{
				double x = 1.0;
				if (std::sscanf(h.c_str() + s, "    size *= %lf", &x) == 1) return x;
			}
			if (h.find("coordinate.xy / (size * rsx_shscale.xy)") != std::string::npos) return 1.0; // unnormalized shadow at 1x
			return 0.0;
		}

		// The text the same draw state gives at another RSX_RES_SCALE (the WPOS define and unnormalized shadow sizes).
		std::string rescale_text(std::string h, double inv)
		{
			const size_t w = h.find("#define _rsx_wpos(p) float4((p).x");
			if (w != std::string::npos)
			{
				const size_t e = h.find('\n', w);
				const std::string g = g9(inv);
				const std::string line = inv == 1.0
					? "#define _rsx_wpos(p) float4((p).x, (rsx_shscale.z > 0.5) ? (rsx_shscale.w - (p).y) : (p).y, (p).z, _rsx_wpos_w(p))"
					: "#define _rsx_wpos(p) float4((p).x * " + g + ", (rsx_shscale.z > 0.5) ? (rsx_shscale.w - (p).y * " + g + ") : (p).y * " + g +
						", (p).z, _rsx_wpos_w(p))";
				h.replace(w, e == std::string::npos ? std::string::npos : e - w, line);
			}
			static const std::string sz = "    float2 size = float2(width, height);\n";
			for (size_t p = h.find(sz); p != std::string::npos; p = h.find(sz, p + 1))
			{
				const size_t after = p + sz.size();
				if (h.compare(after, 12, "    size *= ") == 0) h.erase(after, h.find('\n', after) + 1 - after);
				const size_t blk_end = h.find("\n}\n", after);
				const size_t un = h.find("coordinate.xy / (size * rsx_shscale.xy)", after);
				if (inv != 1.0 && un != std::string::npos && un < blk_end) h.insert(after, "    size *= " + g9(inv) + ";\n");
			}
			return h;
		}

		u64 image_hash(const void* p, size_t n)
		{
			return r1_shc_key("img", "", static_cast<const char*>(p), n);
		}

		struct job
		{
			u64 key;
			const char* target;
			std::string src;
			bool write_hlsl;
		};

		// --emit-profile runs a dry scan; finish() hands the learned profile to this sink.
		thread_local std::FILE* g_profile_sink = nullptr;
		thread_local const char* g_profile_symbol = nullptr;

		class r1d3d12_backend final : public backend
		{
			std::map<std::string, std::string> env;
			fs::path root;
			double res_inv = 1.0;
			std::set<u64> dxbc, hlsl, generated; // keys present in out / written by earlier runs
			struct recorded_fp { u64 key; u64 norm_key; std::string text; };
			std::vector<recorded_fp> rec_ps;      // learn source: recorded fragment HLSL
			std::set<u64> rec_vs;                 // learn source: recorded vertex keys
			unsigned rec_other_scale = 0;
			std::map<std::string, unsigned> generic; // learned variant text (no x) -> recorded uses
			std::map<u64, std::set<std::string>> exact; // FP image hash -> variant texts (with x) seen with it

			void apply_env()
			{
				for (const char* k : k_env_keys)
				{
					auto it = env.find(k);
					_putenv_s(k, it != env.end() ? it->second.c_str() : "");
				}
				res_inv = res_inv_of(env);
			}

			void load_dir(const fs::path& dir, bool as_out)
			{
				std::error_code ec;
				for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
				{
					u64 k;
					if (!parse_key(u8str(it->path().stem()), k)) continue;
					const std::string ext = lower(u8str(it->path().extension()));
					if (as_out && ext == ".dxbc") dxbc.insert(k);
					if (ext == ".hlsl")
					{
						if (as_out) hlsl.insert(k);
						if (generated.count(k)) continue;
						const std::string t = read_text(it->path());
						if (shc_key("ps_5_0", t) == k)
							rec_ps.push_back({ k, k, t });
						else if (shc_key("vs_5_0", t) == k)
							rec_vs.insert(k);
					}
				}
			}

		public:
			const char* name() const override { return "r1d3d12"; }

			int begin(backend_ctx& c) override
			{
				const shaderscan_opts& o = c.o;
				if (!load_fxc()) return c.fail("d3dcompiler_47.dll not found");
				{ std::error_code ae; root = fs::absolute(port_root(o, c.out), ae); }
				env = game_env(root);
				apply_env();
				{
					std::ifstream m(c.out / manifest_name);
					for (std::string l; std::getline(m, l);)
					{
						u64 k;
						if (l.size() > 2 && parse_key(u8str(fs::path(l.substr(2)).stem()), k)) generated.insert(k);
					}
				}
				const bool no_learn = o.learn_dir && std::strcmp(o.learn_dir, "-") == 0;
				load_dir(c.out, true);
				if (o.learn_dir && *o.learn_dir && !no_learn)
				{
					rec_ps.clear();
					rec_vs.clear();
					load_dir(to_path(o.learn_dir), false);
				}
				else if (no_learn)
				{
					rec_ps.clear();
					rec_vs.clear();
				}
				for (auto& r : rec_ps)
				{
					const double ri = recorded_res_inv(r.text);
					if (ri != 0.0 && g9(ri) != g9(res_inv))
					{
						r.norm_key = shc_key("ps_5_0", rescale_text(r.text, res_inv));
						rec_other_scale++;
					}
					r1_fp_variant v = learn_variant(r.text);
					generic[format_variant(v).substr(2)]++;
				}
#ifdef SHADERSCAN_HAVE_R1_PROFILE
				if (!(o.profile && std::strcmp(o.profile, "none") == 0))
					for (const char* const* l = k_profile_r1; *l; l++)
					{
						const std::string s = *l;
						if (s.size() > 19 && s[0] == 'p')
						{
							u64 h = std::strtoull(s.c_str() + 2, nullptr, 16);
							exact[h].insert(s.substr(19));
						}
						else if (s.size() > 2 && s[0] == 'g')
						{
							const size_t sp = s.find(' ', 2);
							if (sp != std::string::npos) generic[s.substr(sp + 1)] += unsigned(std::atoi(s.c_str() + 2));
						}
					}
#endif
				c.log("r1d3d12: RSX_RES_SCALE %s, recorded %zu fragment (%u at another scale) / %zu vertex sources, %zu learned states\n",
					g9(1.0 / res_inv).c_str(), rec_ps.size(), rec_other_scale, rec_vs.size(), generic.size());
				return 0;
			}

			int finish(backend_ctx& c, program_set& progs) override
			{
				const shaderscan_opts& o = c.o;
				shaderscan_result& res = c.r;
				const int K = o.variants > 0 ? o.variants : 4;

				struct fp_img { const u8* p; size_t n; u64 h; u16 textures; std::set<u8> exp; };
				std::vector<fp_img> fimgs;
				for (const auto& fp : progs.fps)
				{
					std::set<u8> exp;
					for (const auto& s : fp.states) exp.insert((s.shader_ctrl & 0x40) ? 1 : 0);
					for (const auto& im : fp.images) fimgs.push_back({ im.data(), im.size(), image_hash(im.data(), im.size()), fp.textures, exp });
				}

				static char buf[R1_HLSL_CAP];
				std::vector<u8> pad;
				auto fp_text = [&](const fp_img& f, const r1_fp_variant& v, std::string& out) -> bool {
					pad.assign(f.p, f.p + f.n);
					pad.resize(f.n + 4096, 0);
					if (r1_fp_hlsl(pad.data(), &v, buf, sizeof(buf)) <= 0) return false;
					out = buf;
					return true;
				};

				// ---- learn: which extracted program + state reproduces each recorded fragment source
				std::unordered_map<u64, size_t> rec_by_norm;
				for (size_t i = 0; i < rec_ps.size(); i++) rec_by_norm[rec_ps[i].norm_key] = i;
				std::set<u64> ps_hit, vs_hit;
				{
					std::set<std::string> tried;
					unsigned n = 0;
					for (const auto& r : rec_ps)
					{
						r1_fp_variant v = learn_variant(r.text);
						const std::string vt = format_variant(v).substr(2);
						if ((++n & 15) == 0 || n == rec_ps.size()) c.step("learn", n, unsigned(rec_ps.size()));
						if (!tried.insert(vt).second) continue;
						const u32 units = variant_units(v);
						for (const auto& f : fimgs)
						{
							if (units & ~u32(f.textures)) continue;
							for (u8 x = 0; x < 2; x++)
							{
								v.exp32 = x;
								std::string t;
								if (!fp_text(f, v, t)) continue;
								if (rec_by_norm.count(shc_key("ps_5_0", t))) exact[f.h].insert(format_variant(v));
							}
						}
					}
				}

				// ---- generate
				std::vector<std::pair<unsigned, std::string>> ranked;
				for (const auto& [t, n] : generic) ranked.push_back({ n, t });
				std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
				std::vector<r1_fp_variant> gvars;
				for (const auto& [n, t] : ranked)
				{
					r1_fp_variant v;
					if (parse_variant_text("x0 " + t, v) && variant_units(v)) gvars.push_back(v);
				}

				std::vector<job> jobs;
				std::unordered_set<u64> keys;
				std::set<std::string> fp_variants;
				auto add = [&](const char* target, std::string&& src) {
					const u64 k = shc_key(target, src);
					if (!keys.insert(k).second) return;
					if (std::strcmp(target, "ps_5_0") == 0) { if (rec_by_norm.count(k)) ps_hit.insert(k); }
					else if (rec_vs.count(k)) vs_hit.insert(k);
					if (dxbc.count(k)) { res.entries_existing++; return; }
					jobs.push_back({ k, target, std::move(src), !hlsl.count(k) });
				};
				// rsx_fsr.inc fsr_mode: FSR / DLSS / XeSS on -> every VS also gets a motion-vector form
				const bool fsr = [&] { auto it = env.find("RSX_FSR"); return it != env.end() && std::atoi(it->second.c_str()) > 0; }();
				unsigned n = 0;
				for (const auto& f : fimgs)
				{
					std::vector<r1_fp_variant> vs;
					for (u8 x : f.exp)
					{
						r1_fp_variant v = zero_variant();
						v.exp32 = x;
						vs.push_back(v);
					}
					if (auto it = exact.find(f.h); it != exact.end())
						for (const auto& t : it->second)
						{
							r1_fp_variant v;
							if (parse_variant_text(t, v)) vs.push_back(v);
						}
					int g = 0;
					for (const auto& gv : gvars)
					{
						if (g >= K) break;
						if (variant_units(gv) & ~u32(f.textures)) continue;
						for (u8 x : f.exp)
						{
							r1_fp_variant v = gv;
							v.exp32 = x;
							vs.push_back(v);
						}
						g++;
					}
					for (const auto& v : vs)
					{
						std::string t;
						if (!fp_text(f, v, t)) continue;
						fp_variants.insert(std::to_string(f.h) + format_variant(v));
						add("ps_5_0", std::move(t));
					}
					if ((++n & 15) == 0 || n == fimgs.size()) c.step("translate", n, unsigned(fimgs.size()));
				}
				for (const auto& vp : progs.vps)
					for (const auto& im : vp.images)
					{
						const int r = r1_vp_hlsl(reinterpret_cast<const uint8_t*>(im.data()), uint32_t(im.size() * 4), buf, sizeof(buf));
						if (r <= 0) continue;
						std::string t = buf;
						size_t gl = 0;
						if (char* g = r1_gf_text(buf, &gl))
						{
							if (fsr)
								if (char* m = r1_mv_vs_text(g)) { add("vs_5_0", m); std::free(m); }
							add("vs_5_0", std::string(g, gl));
							std::free(g);
						}
						if (fsr)
							if (char* m = r1_mv_vs_text(t.c_str())) { add("vs_5_0", m); std::free(m); }
						add("vs_5_0", std::move(t));
					}
				if (fsr)
				{
					// rsx_fsr.inc mv_init: the motion-vector pixel shader and its reactive-mask form
					char ps[2048];
					if (size_t l = r1_mv_ps_text(-1.0f, ps, sizeof(ps))) add("ps_5_0", std::string(ps, l));
					if (size_t l = r1_mv_ps_text(fsr_reactive(env), ps, sizeof(ps))) add("ps_5_0", std::string(ps, l));
				}
				res.fp_variants = unsigned(fp_variants.size());
				res.cache_fp = unsigned(rec_ps.size());
				res.cache_fp_covered = unsigned(ps_hit.size());
				res.cache_vp = unsigned(rec_vs.size());
				res.cache_vp_covered = unsigned(vs_hit.size());
				res.cache_pipelines = res.cache_fp + res.cache_vp;
				res.cache_fp_state_hits = res.cache_fp_covered + res.cache_vp_covered;
				res.entries_total = unsigned(jobs.size()) + res.entries_existing;
				c.log("r1d3d12: %zu fragment images, %zu fragment states learned exactly, %zu generic states; %zu shaders to compile\n", fimgs.size(),
					exact.size(), gvars.size(), jobs.size());

				if (g_profile_sink) write_profile(g_profile_sink, g_profile_symbol);

				// ---- compile + write (FXC is thread-safe; the translator above is not)
				if (!o.dry_run)
				{
					std::error_code ec;
					fs::create_directories(c.out, ec);
					std::ofstream manifest(c.out / manifest_name, std::ios::app);
					std::mutex mu;
					std::atomic<size_t> next{ 0 };
					std::atomic<unsigned> done{ 0 }, written{ 0 }, failed{ 0 };
					std::atomic<bool> io_error{ false };
					const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
					auto work = [&]() {
						std::vector<u8> code;
						for (size_t i; (i = next++) < jobs.size();)
						{
							const job& j = jobs[i];
							if (fxc(j.src, source_name(j.target, j.src), j.target, code))
							{
								const std::string base = key_name(j.key);
								{
									std::lock_guard<std::mutex> l(mu);
									if (j.write_hlsl) manifest << "H " << base << ".hlsl" << std::endl; // listed before it exists
									manifest << "D " << base << ".dxbc" << std::endl;
								}
								bool ok = !j.write_hlsl || write_atomic(c.out / (base + ".hlsl"), j.src.data(), j.src.size());
								ok = ok && write_atomic(c.out / (base + ".dxbc"), code.data(), code.size());
								if (ok) written++;
								else io_error = true;
							}
							else failed++;
							const unsigned d = ++done;
							if ((d & 15) == 0)
							{
								std::lock_guard<std::mutex> l(mu);
								c.step("write", d, unsigned(jobs.size()));
							}
						}
					};
					std::vector<std::thread> pool;
					for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
					work();
					for (auto& t : pool) t.join();
					c.step("write", unsigned(jobs.size()), unsigned(jobs.size()));
					manifest.close();
					res.entries_written = written;
					if (failed) c.log("r1d3d12: %u shaders failed to compile (the game would fail them too)\n", unsigned(failed));
					if (io_error) return c.fail("cannot write to %s", u8str(c.out).c_str());

					const fs::path st_tmp = c.out / "shaderscan.stamp.tmp";
					{
						std::ofstream stamp(st_tmp, std::ios::trunc);
						stamp << "shaderscan " << SHADERSCAN_VERSION << "\n" << k_backend_tag << "\nenv " << env_fingerprint(env) << "\nroot " << u8str(root) << "\nfp " << res.fp_programs
							  << "\nvp " << res.vp_programs << "\nvariants " << res.fp_variants << "\nentries " << res.entries_total << "\n";
					}
					fs::rename(st_tmp, c.out / stamp_name, ec);
					if (ec) return c.fail("cannot write %s", u8str(c.out / stamp_name).c_str());
				}
				else res.entries_written = unsigned(jobs.size());
				return 0;
			}

			void write_profile(std::FILE* f, const char* symbol)
			{
				std::fprintf(f, "// Generated by shaderscan --backend r1d3d12 --emit-profile. Renderer draw-state statistics only: per fragment\n"
								"// program image (FNV-1a hash) the translator states it was drawn with; contains no game code or data.\n");
				std::fprintf(f, "static const char* const %s[] = {\n", symbol && *symbol ? symbol : "k_profile_r1");
				for (const auto& [h, ts] : exact)
					for (const auto& t : ts) std::fprintf(f, "\t\"p %016llX %s\",\n", static_cast<unsigned long long>(h), t.c_str());
				for (const auto& [t, n] : generic) std::fprintf(f, "\t\"g %u %s\",\n", n, t.c_str());
				std::fprintf(f, "\tnullptr,\n};\n");
			}

			int clean(backend_ctx& c) override
			{
				std::ifstream m(c.out / manifest_name);
				std::vector<std::string> lines;
				for (std::string l; std::getline(m, l);)
					if (l.size() > 2) lines.push_back(l);
				m.close();
				std::error_code ec;
				unsigned n = 0;
				for (const auto& l : lines)
				{
					fs::remove(c.out / fs::path(l.substr(2)), ec);
					if ((++n & 255) == 0) c.step("clean", n, unsigned(lines.size()));
				}
				fs::remove(c.out / manifest_name, ec);
				fs::remove(c.out / stamp_name, ec);
				c.step("clean", n, unsigned(lines.size()));
				c.log("removed %u generated files\n", n);
				return 0;
			}

			int needs_run(const fs::path& out) const override
			{
				std::ifstream f(out / stamp_name);
				std::string tool, l;
				int ver = 0;
				if (!(f >> tool >> ver) || ver != SHADERSCAN_VERSION) return 1;
				std::getline(f, l);
				std::string env_line, root_line;
				while (std::getline(f, l))
				{
					if (l == "nothing-found") return 0;
					if (l.rfind("env ", 0) == 0) env_line = l.substr(4);
					if (l.rfind("root ", 0) == 0) root_line = l.substr(5);
				}
				// --check only knows --out: use the game root the run recorded (default <out>\..)
				shaderscan_opts o{};
				std::error_code ec;
				const fs::path root = !root_line.empty() && fs::is_directory(to_path(root_line.c_str()), ec) ? to_path(root_line.c_str()) : port_root(o, out);
				return env_line != env_fingerprint(game_env(root));
			}

			int verify(const shaderscan_opts& o, const fs::path& dir, std::FILE* rep) override
			{
				// keys need no translator; the recompile check needs the compiler only
				std::vector<std::pair<u64, fs::path>> srcs;
				std::set<u64> bins;
				unsigned bad_name = 0;
				std::error_code ec;
				for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
				{
					u64 k;
					const std::string ext = lower(u8str(it->path().extension()));
					if (ext != ".hlsl" && ext != ".dxbc") continue;
					if (!parse_key(u8str(it->path().stem()), k)) { bad_name++; continue; }
					if (ext == ".hlsl") srcs.push_back({ k, it->path() });
					else bins.insert(k);
				}
				std::sort(srcs.begin(), srcs.end());
				struct row { u64 key; std::string text; const char* target; };
				std::vector<row> rows;
				unsigned ps = 0, vs = 0, mismatch = 0;
				std::vector<std::string> examples;
				for (const auto& [k, p] : srcs)
				{
					std::string t = read_text(p);
					const char* target = shc_key("ps_5_0", t) == k ? "ps_5_0" : shc_key("vs_5_0", t) == k ? "vs_5_0" : nullptr;
					if (!target)
					{
						if (examples.size() < 8) examples.push_back(key_name(k) + ".hlsl: key does not re-derive");
						mismatch++;
						continue;
					}
					(target[0] == 'p' ? ps : vs)++;
					rows.push_back({ k, std::move(t), target });
				}
				unsigned no_src = 0;
				for (u64 k : bins)
					if (!std::binary_search(srcs.begin(), srcs.end(), std::make_pair(k, fs::path()),
							[](auto& a, auto& b) { return a.first < b.first; }))
						no_src++;

				// recompile each source as the runtime would and compare with the stored bytecode
				std::atomic<size_t> next{ 0 };
				std::atomic<unsigned> same{ 0 }, differ{ 0 }, fail{ 0 }, nobin{ 0 };
				std::mutex mu;
				auto work = [&]() {
					std::vector<u8> code, disk;
					for (size_t i; (i = next++) < rows.size();)
					{
						const row& r = rows[i];
						if (!bins.count(r.key)) { nobin++; continue; }
						if (!fxc(r.text, source_name(r.target, r.text), r.target, code)) { fail++; continue; }
						if (read_file(dir / (key_name(r.key) + ".dxbc"), disk) && disk == code) same++;
						else
						{
							differ++;
							std::lock_guard<std::mutex> l(mu);
							if (examples.size() < 8) examples.push_back(key_name(r.key) + ".dxbc differs from a fresh compile of its .hlsl");
						}
					}
				};
				const bool have_fxc = load_fxc() != nullptr;
				if (have_fxc)
				{
					std::vector<std::thread> pool;
					const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
					for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
					work();
					for (auto& t : pool) t.join();
				}
				(void)o;
				std::fprintf(rep, "verify %s (r1d3d12 shadercache)\n", u8str(dir).c_str());
				std::fprintf(rep, "  sources %zu: keys re-derived from content %u ps_5_0 + %u vs_5_0, %u disagree; %u bad file names\n", srcs.size(), ps,
					vs, mismatch, bad_name);
				std::fprintf(rep, "  bytecode %zu: %u without a source\n", bins.size(), no_src);
				if (have_fxc)
					std::fprintf(rep, "  recompiled with d3dcompiler_47 (flags 0): %u byte-identical, %u differ, %u fail, %u source without bytecode\n",
						unsigned(same), unsigned(differ), unsigned(fail), unsigned(nobin));
				else std::fprintf(rep, "  d3dcompiler_47.dll not found: bytecode not checked\n");
				for (const auto& e : examples) std::fprintf(rep, "  ! %s\n", e.c_str());
				const bool ok = !mismatch && !bad_name && !differ && !fail;
				std::fprintf(rep, "%s\n", ok ? "OK" : "FAILED");
				return ok ? 0 : 1;
			}

			int emit_profile(const shaderscan_opts& o, std::FILE* f, const char* symbol) override
			{
				shaderscan_opts d = o;
				d.dry_run = 1;
				d.backend = "r1d3d12";
				if (!d.out_dir) d.out_dir = o.learn_dir;
				if (!d.out_dir || !d.game_dir) return -1;
				g_profile_sink = f;
				g_profile_symbol = symbol;
				shaderscan_result r{};
				const int rc = shaderscan_run(&d, nullptr, nullptr, &r);
				g_profile_sink = nullptr;
				if (rc) return -1;
				std::printf("profile: %u/%u recorded fragment sources reproduced", r.cache_fp_covered, r.cache_fp);
				return 0;
			}
		};
	}

	std::unique_ptr<backend> make_r1d3d12_backend()
	{
		return std::make_unique<r1d3d12_backend>();
	}
}
