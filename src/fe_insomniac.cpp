// Insomniac Games engine front end (Resistance 1-3 / Ratchet & Clank Future generation).
// SPDX-License-Identifier: GPL-2.0-only
//
// Programs are stored with a 0x20-byte big-endian header, both statically in the EBOOT (engine shaders and every
// vertex program) and in built\levels\<lvl>\shaders.dat inside level_cached.psarc (material fragment programs).
//
// Fragment program header:                      Vertex program header:
//   +0x02 u16 ucode size (bytes, x16)              +0x00 u16 instruction count
//   +0x04 u32 centroid mask << 16 | tc2d mask      +0x02 u16 0
//   +0x08 u32 0x84xx | shader control (low byte)   +0x04 u32 input mask
//   +0x0C u32 0                                    +0x08 u32 output mask
//   +0x10 u32 ucode offset from header             +0x0C u16 0x20, +0x0E u16 0xFFFF
//   (constant patch table between header/ucode)    +0x10 u32 ucode offset (BE instruction words)
//
// R3 keeps every program in one of these blocks. R2's shaders.dat holds material parameters only (no ucode); its
// programs are all found in the EBOOT.
//
// Two ways to find the level programs (--discovery):
//  - scan: byte-scan shaders.dat for the headers above (and the EBOOT, always).
//  - tables: follow the level's own index, as the engine does. Both files are IGHW containers (big-endian):
//      header +0x00 "IGHW", +0x04 u16 major (1), +0x06 u16 minor, +0x08 u32 section count, section records at +0x20,
//      16 bytes each: u32 id, u32 offset, u32 count word, u32 length. Count word bit 28 set = array of
//      (count & 0x0FFFFFFF) items of 'length' bytes; otherwise 'length' is the section's byte size.
//    assetlookup.dat section 0x1D100: {u64 tuid, u32 offset, u32 size} per material blob in shaders.dat.
//    Each blob is itself an IGHW; when it owns programs it has section 0x33001 (headed fragment programs, back to
//    back) and 0x33002: {u64 hash, u32 size, u32 offset into 0x33001} per program, size = ucode offset + ucode size.
//  Every offset is bounds-checked; a container whose tables are missing or inconsistent is byte-scanned instead.
#include "frontend.h"

#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace shaderscan
{
	namespace
	{
		constexpr u32 ighw_magic = 0x49474857;   // "IGHW"
		constexpr u32 sec_shader_ptrs = 0x1D100; // assetlookup.dat: shader blobs in shaders.dat
		constexpr u32 sec_fp_data = 0x33001;     // shader blob: headed fragment programs
		constexpr u32 sec_fp_table = 0x33002;    // shader blob: program table into sec_fp_data
		constexpr size_t max_table_ucode = 0x10000;

		bool ends_with(const std::string& s, const char* tail)
		{
			const size_t n = std::char_traits<char>::length(tail);
			return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
		}

		std::string lower_slash(std::string s)
		{
			for (char& c : s)
			{
				if (c >= 'A' && c <= 'Z') c = char(c + 32);
				if (c == '\\') c = '/';
			}
			return s;
		}

		std::string dir_of(const std::string& lower_path)
		{
			const size_t k = lower_path.rfind('/');
			return k == std::string::npos ? std::string() : lower_path.substr(0, k);
		}

		u64 be64(const u8* p) { return (u64(be32(p)) << 32) | be32(p + 4); }

		struct ighw_section
		{
			u32 id = 0;
			size_t offset = 0;
			size_t size = 0;
		};

		// Section records of an IGHW v1 container in [d, d + n). False when it is not one or any section lies
		// outside the image.
		bool ighw_parse(const u8* d, size_t n, std::vector<ighw_section>& out)
		{
			out.clear();
			if (n < 0x20 || be32(d) != ighw_magic || be16(d + 4) != 1) return false;
			const u32 count = be32(d + 8);
			if (count == 0 || count > 0x1000 || 0x20 + size_t(count) * 16 > n) return false;
			for (u32 i = 0; i < count; i++)
			{
				const u8* e = d + 0x20 + size_t(i) * 16;
				const u32 cw = be32(e + 8), len = be32(e + 12);
				const u64 size = (cw & 0x10000000) ? u64(cw & 0x0FFFFFFF) * len : u64(len);
				const u64 off = be32(e + 4);
				if (off > n || size > n - off) return false;
				out.push_back({ be32(e), size_t(off), size_t(size) });
			}
			return true;
		}

		const ighw_section* find_section(const std::vector<ighw_section>& s, u32 id)
		{
			for (const auto& x : s)
				if (x.id == id) return &x;
			return nullptr;
		}

		const char* mode_name(discovery_mode m)
		{
			return m == discovery_mode::tables ? "tables" : m == discovery_mode::scan ? "scan" : "both";
		}

		class insomniac_frontend final : public frontend
		{
		public:
			const char* name() const override { return "insomniac_shaders_dat"; }

			bool wants_file(const std::string& p) const override
			{
				return ends_with(p, "shaders.dat") || (m_mode != discovery_mode::scan && ends_with(p, "assetlookup.dat"));
			}

			void set_discovery(discovery_mode m) override { m_mode = m; }

			void begin_group(const std::string& name) override
			{
				m_pending.clear();
				m_group = name;
			}

			size_t end_group(program_set& out) override
			{
				size_t added = 0;
				for (auto& [dir, p] : m_pending)
				{
					if (!p.has_shaders) continue;
					m_st.containers++;
					if (m_mode == discovery_mode::tables)
					{
						m_st.fallback++;
						note(p.shaders_source, "no assetlookup.dat, byte-scanned");
						added += scan_bytes(p.shaders.data(), p.shaders.size(), p.shaders_source, out, true);
					}
					else note(p.shaders_source, "no assetlookup.dat (byte-scan only)");
				}
				m_pending.clear();
				return added;
			}

			size_t scan(const u8* d, size_t n, const std::string& source, program_set& out) override
			{
				const std::string lp = lower_slash(source);
				const bool is_shaders = ends_with(lp, "shaders.dat");
				const bool is_lookup = ends_with(lp, "assetlookup.dat");
				if (m_mode == discovery_mode::scan || (!is_shaders && !is_lookup))
				{
					m_st.containers += is_shaders;
					return scan_bytes(d, n, source, out, is_shaders);
				}

				size_t added = 0;
				if (is_shaders && m_mode == discovery_mode::both) added += scan_bytes(d, n, source, out, true);

				const std::string dir = dir_of(lp);
				pending& p = m_pending[dir];
				if (is_lookup)
				{
					p.lookup.assign(d, d + n);
					p.has_lookup = true;
					if (!p.has_shaders) return added;
				}
				else
				{
					p.shaders_source = source;
					p.has_shaders = true;
					if (!p.has_lookup)
					{
						p.shaders.assign(d, d + n); // wait for the index (it follows shaders.dat in level_cached.psarc)
						return added;
					}
				}
				m_st.containers++;
				const pending done = std::move(p);
				m_pending.erase(dir);
				if (is_lookup) added += walk_tables(done.lookup.data(), done.lookup.size(), done.shaders.data(), done.shaders.size(), done.shaders_source, out);
				else added += walk_tables(done.lookup.data(), done.lookup.size(), d, n, done.shaders_source, out);
				return added;
			}

			size_t finish(program_set& out) override
			{
				size_t added = 0;
				for (const auto& t : m_table_progs)
				{
					const bool known = out.fp_index.count(t.hash) != 0;
					if (!out.add_fp(t.image.data(), t.image.size(), t.tc, t.cm, t.ctrl, t.source)) continue;
					if (known) continue;
					added++;
					if (!m_st.scan_fp.count(t.hash)) m_st.new_only.push_back({ t.hash, t.source });
				}
				m_table_progs.clear();
				m_table_keys.clear();
				return added;
			}

			std::string report() const override
			{
				if (!m_st.containers) return {};
				size_t common = 0;
				for (u64 h : m_st.table_fp) common += m_st.scan_fp.count(h);
				char line[512];
				std::string r;
				if (m_mode == discovery_mode::scan)
					std::snprintf(line, sizeof(line), "discovery scan: %u shaders.dat byte-scanned (tables not consulted)\n", m_st.containers);
				else
					std::snprintf(line, sizeof(line),
						"discovery %s: %u shaders.dat, %u via assetlookup tables, %u without usable tables (byte-scanned)\n", mode_name(m_mode),
						m_st.containers, m_st.tabled, m_st.fallback);
				r += line;
				if (m_mode != discovery_mode::scan)
				{
					std::snprintf(line, sizeof(line),
						"  tables:    %zu unique FP from %u table entries (%u blobs, %u with programs; %u entries rejected, %u of them "
						"ending before the declared size)\n",
						m_st.table_fp.size(), m_st.entries, m_st.blobs, m_st.program_blobs, m_st.rejected, m_st.inexact);
					r += line;
				}
				if (m_mode != discovery_mode::tables || m_st.fallback)
				{
					std::snprintf(line, sizeof(line), "  byte-scan: %zu unique FP, %zu VP in shaders.dat\n", m_st.scan_fp.size(), m_st.scan_vp.size());
					r += line;
				}
				if (m_mode == discovery_mode::both)
				{
					std::snprintf(line, sizeof(line), "  both %zu, tables only %zu, byte-scan only %zu; new to the program set from tables: %zu\n",
						common, m_st.table_fp.size() - common, m_st.scan_fp.size() - common, m_st.new_only.size());
					r += line;
				}
				else if (m_mode == discovery_mode::tables && !m_st.new_only.empty())
				{
					std::snprintf(line, sizeof(line), "  added by tables: %zu\n", m_st.new_only.size());
					r += line;
				}
				if (m_mode == discovery_mode::both)
				{
					for (size_t i = 0; i < m_st.new_only.size() && i < 32; i++)
					{
						std::snprintf(line, sizeof(line), "    tables only: %016llX.fp  %s\n", static_cast<unsigned long long>(m_st.new_only[i].first),
							m_st.new_only[i].second.c_str());
						r += line;
					}
					if (m_st.new_only.size() > 32) r += "    ...\n";
				}
				for (size_t i = 0; i < m_st.notes.size() && i < 16; i++) r += "  " + m_st.notes[i] + "\n";
				if (m_st.notes.size() > 16)
				{
					std::snprintf(line, sizeof(line), "  ... %zu more notes\n", m_st.notes.size() - 16);
					r += line;
				}
				return r;
			}

		private:
			struct pending
			{
				std::vector<u8> shaders, lookup;
				std::string shaders_source;
				bool has_shaders = false, has_lookup = false;
			};

			struct table_prog
			{
				std::vector<u8> image; // from the ucode offset, 'ucode size' bytes
				u64 hash;
				u32 tc, cm, ctrl;
				std::string source;
			};

			struct stats
			{
				unsigned containers = 0, tabled = 0, fallback = 0;
				unsigned blobs = 0, program_blobs = 0, entries = 0, rejected = 0, inexact = 0;
				std::set<u64> table_fp, scan_fp, scan_vp;
				std::vector<std::pair<u64, std::string>> new_only;
				std::vector<std::string> notes;
			};

			discovery_mode m_mode = discovery_mode::both;
			std::string m_group;
			std::map<std::string, pending> m_pending; // per directory, inside the current group
			std::vector<table_prog> m_table_progs;     // queued for finish()
			std::set<std::tuple<u64, u32, u32, u32, u64>> m_table_keys;
			stats m_st;

			void note(const std::string& source, const char* what)
			{
				// loose files are named without their directory: prefix the group (the directory)
				const bool bare = source.find('/') == std::string::npos && source.find('\\') == std::string::npos;
				m_st.notes.push_back((bare && !m_group.empty() ? m_group + "\\" : std::string()) + source + ": " + what);
			}

			// Exact extents from assetlookup.dat -> shaders.dat -> 0x33002. Programs are queued for finish(), so the
			// byte-scan order (and with it the output of --discovery both) is unchanged by what the tables add.
			size_t walk_tables(const u8* al, size_t al_n, const u8* sh, size_t sh_n, const std::string& source, program_set& out)
			{
				std::vector<ighw_section> secs, bs;
				const ighw_section* ptrs = nullptr;
				if (!ighw_parse(al, al_n, secs) || !(ptrs = find_section(secs, sec_shader_ptrs)) || ptrs->size % 16)
					return fallback(source, "assetlookup.dat has no usable shader table (0x1D100)", sh, sh_n, out);

				unsigned blobs = 0, program_blobs = 0, entries = 0, rejected = 0, bad_blobs = 0, inexact = 0;
				std::set<u64> found;
				std::vector<table_prog> progs;
				for (size_t i = 0; i < ptrs->size / 16; i++)
				{
					const u8* e = al + ptrs->offset + i * 16;
					const u64 off = be32(e + 8), len = be32(e + 12);
					if (!len || off > sh_n || len > sh_n - off)
					{
						bad_blobs++;
						continue;
					}
					const u8* blob = sh + off;
					blobs++;
					if (!ighw_parse(blob, size_t(len), bs))
					{
						bad_blobs++;
						continue;
					}
					const ighw_section* data = find_section(bs, sec_fp_data);
					const ighw_section* table = find_section(bs, sec_fp_table);
					if (!table) continue; // material without programs of its own
					if (!data || table->size % 16)
					{
						bad_blobs++;
						continue;
					}
					program_blobs++;
					for (size_t k = 0; k < table->size / 16; k++)
					{
						const u8* t = blob + table->offset + k * 16;
						const u64 size = be32(t + 8), poff = be32(t + 12);
						entries++;
						if (poff % 16 || size < 0x20 || poff > data->size || size > data->size - poff)
						{
							rejected++;
							continue;
						}
						const u8* h = blob + data->offset + poff;
						const u32 usz = be16(h + 2), uo = be32(h + 0x10), w4 = be32(h + 4), w8 = be32(h + 8);
						if ((w8 & 0xFFFF00) != 0x008400 || be32(h + 12) != 0 || !usz || usz % 16 || uo < 0x20 || uo % 16 ||
							u64(uo) + usz != size || usz > max_table_ucode)
						{
							rejected++;
							continue;
						}
						fp_info info;
						if (!fp_analyse(h + uo, usz, info) || info.unknown_opcode)
						{
							rejected++;
							continue;
						}
						// the table's size is the exact extent: a program that ends early is not what the entry describes
						if (info.start + info.length != usz)
						{
							inexact++;
							rejected++;
							continue;
						}
						const u64 hash = fp_hash(h + uo + info.start, info.length);
						found.insert(hash);
						const u32 tc = w4 & 0x3FF, cm = (w4 >> 16) & 0x3FF, ctrl = w8 & 0xCE;
						// the same program recurs in every level: keep one copy per distinct image and state
						const auto key = std::make_tuple(hash, tc, cm, ctrl, fnv_bytes(h + uo, info.start + info.length));
						if (!m_table_keys.insert(key).second) continue;
						progs.push_back({ std::vector<u8>(h + uo, h + uo + info.start + info.length), hash, tc, cm, ctrl, source });
					}
				}

				m_st.blobs += blobs;
				m_st.program_blobs += program_blobs;
				m_st.entries += entries;
				m_st.rejected += rejected;
				m_st.inexact += inexact;
				if (!program_blobs)
				{
					// R2: material-only blobs; anything a byte-scan finds here is not in a table
					return fallback(source, bad_blobs ? "shader table inconsistent and no program tables" : "no program tables (0x33002) in any shader blob",
						sh, sh_n, out);
				}
				m_st.tabled++;
				m_st.table_fp.insert(found.begin(), found.end());
				for (auto& p : progs) m_table_progs.push_back(std::move(p));
				if (bad_blobs || rejected)
				{
					const bool rescan = m_mode == discovery_mode::tables;
					char why[200];
					std::snprintf(why, sizeof(why), "%u bad blob pointer(s), %u rejected table entries%s", bad_blobs, rejected,
						rescan ? "; byte-scanned as well" : "");
					note(source, why);
					if (rescan) return scan_bytes(sh, sh_n, source, out, true);
				}
				return 0;
			}

			size_t fallback(const std::string& source, const char* why, const u8* sh, size_t sh_n, program_set& out)
			{
				m_st.fallback++;
				if (m_mode != discovery_mode::tables)
				{
					note(source, why);
					return 0; // --discovery both already byte-scanned it
				}
				note(source, (std::string(why) + "; byte-scanned").c_str());
				return scan_bytes(sh, sh_n, source, out, true);
			}

			static u64 fnv_bytes(const u8* p, size_t n)
			{
				u64 h = fnv_seed;
				for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * fnv_prime;
				return h;
			}

			// The original header scan. 'level' = the image is a shaders.dat (counted for the discovery report).
			size_t scan_bytes(const u8* d, size_t n, const std::string& source, program_set& out, bool level)
			{
				size_t added = 0;
				std::vector<u32> words;
				for (size_t o = 0; o + 0x40 <= n; o += 4)
				{
					const u8* h = d + o;
					const u32 w8 = be32(h + 8);
					if ((w8 & 0xFFFF00) == 0x008400 && be32(h + 12) == 0)
					{
						const u32 sz = be16(h + 2);
						const u32 uo = be32(h + 0x10);
						if (sz && !(sz % 16) && !(uo % 16) && uo >= 0x20 && uo <= 0x1000 && o + uo + sz <= n)
						{
							const size_t before = out.fps.size();
							const u32 w4 = be32(h + 4);
							fp_info info;
							// The declared size is the program's exact extent; this rejects header look-alikes in
							// unrelated data (--deep found some in Bink movies).
							if (fp_analyse(h + uo, sz, info) && info.start + info.length == sz &&
								out.add_fp(h + uo, sz, w4 & 0x3FF, (w4 >> 16) & 0x3FF, w8 & 0xCE, source))
							{
								if (out.fps.size() > before) added++;
								if (level) m_st.scan_fp.insert(fp_hash(h + uo + info.start, info.length));
							}
						}
					}
					if (be16(h + 0xE) == 0xFFFF && be16(h + 0xC) == 0x20 && be16(h + 2) == 0)
					{
						const u32 cnt = be16(h);
						const u32 uo = be32(h + 0x10);
						if (cnt && cnt <= max_vp_instructions && !(uo % 16) && uo >= 0x20 && uo <= 0x400 && o + uo + size_t(cnt) * 16 <= n &&
							(be32(h + uo + size_t(cnt - 1) * 16 + 12) & 1))
						{
							words.resize(size_t(cnt) * 4);
							for (size_t i = 0; i < words.size(); i++) words[i] = be32(h + uo + i * 4);
							const size_t before = out.vps.size();
							if (out.add_vp(words.data(), cnt, 0, be32(h + 8), source))
							{
								if (out.vps.size() > before) added++;
								vertex_program vp;
								if (level && vp_analyse(words.data(), cnt, 0, vp))
								{
									vp.output_mask = be32(h + 8);
									m_st.scan_vp.insert(vp_hash(vp));
								}
							}
						}
					}
				}
				return added;
			}
		};
	}

	std::unique_ptr<frontend> make_insomniac_frontend()
	{
		return std::make_unique<insomniac_frontend>();
	}
}
