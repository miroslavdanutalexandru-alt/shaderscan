// SPDX-License-Identifier: GPL-2.0-only
#include "frontend.h"

#include <algorithm>

namespace shaderscan
{
	bool program_set::add_fp(const u8* p, size_t avail, u32 texcoord_mask, u32 centroid_mask, u32 shader_ctrl, const std::string& source)
	{
		fp_info info;
		if (!fp_analyse(p, avail, info) || info.unknown_opcode) return false;
		const u8* start = p + info.start;
		const u64 h = fp_hash(start, info.length);
		const fp_header_state st{ texcoord_mask & 0x3FF, centroid_mask & 0x3FF, shader_ctrl & 0xCE };
		std::vector<u8> image(p, start + info.length);
		if (auto it = fp_index.find(h); it != fp_index.end())
		{
			auto& states = fps[it->second].states;
			bool known = false;
			for (const auto& s : states) known |= (s == st);
			if (!known) states.push_back(st);
			auto& images = fps[it->second].images;
			if (std::find(images.begin(), images.end(), image) == images.end()) images.push_back(std::move(image));
			return true;
		}
		fragment_program fp;
		fp.ucode.assign(start, start + info.length);
		fp.hash = h;
		fp.textures = info.textures;
		fp.bx2 = info.bx2;
		fp.states.push_back(st);
		fp.source = source;
		fp.images.push_back(std::move(image));
		fp_index.emplace(h, fps.size());
		fps.push_back(std::move(fp));
		return true;
	}

	bool program_set::add_vp(const u32* words, u32 count, u32 entry, u32 output_mask, const std::string& source)
	{
		if (!count || count > max_vp_instructions || entry >= count) return false;
		vertex_program vp;
		if (!vp_analyse(words, count, entry, vp)) return false;
		vp.output_mask = output_mask;
		vp.output_masks.push_back(output_mask);
		vp.hash = vp_hash(vp);
		vp.source = source;
		std::vector<u32> image(words, words + size_t(count) * 4);
		if (auto it = vp_index.find(vp.hash); it != vp_index.end())
		{
			auto& masks = vps[it->second].output_masks;
			if (std::find(masks.begin(), masks.end(), output_mask) == masks.end()) masks.push_back(output_mask);
			auto& images = vps[it->second].images;
			if (std::find(images.begin(), images.end(), image) == images.end()) images.push_back(std::move(image));
			return true;
		}
		vp.images.push_back(std::move(image));
		vp_index.emplace(vp.hash, vps.size());
		vps.push_back(std::move(vp));
		return true;
	}
}
