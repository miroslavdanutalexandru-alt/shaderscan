// SPDX-License-Identifier: GPL-2.0-only
#include "psarc.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#define _fseeki64 fseeko
#endif

namespace shaderscan
{
	static uint64_t be_n(const uint8_t* p, int n)
	{
		uint64_t v = 0;
		for (int i = 0; i < n; i++) v = (v << 8) | p[i];
		return v;
	}

	psarc::~psarc()
	{
		if (m_file) std::fclose(m_file);
	}

	bool psarc::open(const std::filesystem::path& path)
	{
#ifdef _WIN32
		m_file = _wfopen(path.c_str(), L"rb");
#else
		m_file = std::fopen(path.c_str(), "rb");
#endif
		if (!m_file) return false;
		uint8_t h[32];
		if (std::fread(h, 1, 32, m_file) != 32 || std::memcmp(h, "PSAR", 4) != 0) return false;
		if (std::memcmp(h + 8, "zlib", 4) != 0) return false; // lzma archives are not used by the supported titles
		const uint32_t toc_len = uint32_t(be_n(h + 12, 4));
		const uint32_t es = uint32_t(be_n(h + 16, 4));
		const uint32_t n = uint32_t(be_n(h + 20, 4));
		m_block_size = uint32_t(be_n(h + 24, 4));
		if (toc_len < 32 || es < 30 || n == 0 || !m_block_size) return false;
		std::vector<uint8_t> toc(toc_len - 32);
		if (std::fread(toc.data(), 1, toc.size(), m_file) != toc.size()) return false;
		if (size_t(n) * es > toc.size()) return false;
		m_entries.resize(n);
		for (uint32_t i = 0; i < n; i++)
		{
			const uint8_t* e = &toc[size_t(i) * es];
			m_entries[i] = { uint32_t(be_n(e + 16, 4)), be_n(e + 20, 5), be_n(e + 25, 5) };
		}
		const int w = m_block_size <= 0x10000 ? 2 : (m_block_size <= 0x1000000 ? 3 : 4);
		for (size_t o = size_t(n) * es; o + w <= toc.size(); o += w) m_blocks.push_back(uint32_t(be_n(&toc[o], w)));

		std::vector<uint8_t> names;
		if (!read_entry(0, names)) return false;
		size_t s = 0;
		for (size_t i = 0; i <= names.size(); i++)
		{
			if (i == names.size() || names[i] == '\n')
			{
				m_names.emplace_back(reinterpret_cast<const char*>(names.data()) + s, i - s);
				s = i + 1;
			}
		}
		m_names.resize(n - 1);
		return true;
	}

	bool psarc::read_entry(size_t k, std::vector<uint8_t>& out)
	{
		if (k >= m_entries.size()) return false;
		const entry& e = m_entries[k];
		out.clear();
		out.reserve(size_t(e.size));
		if (_fseeki64(m_file, int64_t(e.offset), SEEK_SET) != 0) return false;
		std::vector<uint8_t> in;
		uint32_t b = e.block;
		while (out.size() < e.size)
		{
			if (b >= m_blocks.size()) return false;
			const uint32_t z = m_blocks[b++];
			const size_t want = size_t(std::min<uint64_t>(m_block_size, e.size - out.size()));
			const size_t len = z ? z : m_block_size;
			in.resize(len);
			if (std::fread(in.data(), 1, len, m_file) != len) return false;
			const size_t at = out.size();
			if (z && len != want && in[0] == 0x78)
			{
				out.resize(at + m_block_size);
				mz_ulong dl = mz_ulong(m_block_size);
				if (mz_uncompress(out.data() + at, &dl, in.data(), mz_ulong(len)) == MZ_OK)
				{
					out.resize(at + dl);
					continue;
				}
				out.resize(at);
			}
			out.insert(out.end(), in.begin(), in.end());
		}
		out.resize(size_t(e.size));
		return true;
	}
}
