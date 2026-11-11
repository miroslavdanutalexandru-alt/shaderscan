// PSARC (PlayStation archive) reader: zlib blocks, used by Insomniac's PS3 titles and many others.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace shaderscan
{
	class psarc
	{
	public:
		~psarc();
		bool open(const std::filesystem::path& path);
		size_t count() const { return m_names.size(); }
		const std::string& name(size_t i) const { return m_names[i]; }
		uint64_t size(size_t i) const { return m_entries[i + 1].size; }
		bool read(size_t i, std::vector<uint8_t>& out) { return read_entry(i + 1, out); }

	private:
		struct entry { uint32_t block; uint64_t size; uint64_t offset; };
		bool read_entry(size_t k, std::vector<uint8_t>& out);
		std::FILE* m_file = nullptr;
		uint32_t m_block_size = 0;
		std::vector<entry> m_entries;
		std::vector<uint32_t> m_blocks;
		std::vector<std::string> m_names;
	};
}
