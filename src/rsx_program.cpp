// SPDX-License-Identifier: GPL-2.0-only
// Mirrors rpcs3/Emu/RSX/Program/ProgramStateCache.cpp (analyse_* / get_*_ucode_hash).
#include "rsx_program.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stack>

namespace shaderscan
{
	bool fp_slot_has_constant(const u8* s)
	{
		return (le32(s + 4) & 0x300) == 0x200 || (le32(s + 8) & 0x300) == 0x200 || (le32(s + 12) & 0x300) == 0x200;
	}

	static bool fp_opcode_known(u32 op)
	{
		return op <= 0x2F || op == 0x31 || (op >= 0x33 && op <= 0x3E);
	}

	bool fp_analyse(const u8* p, size_t avail, fp_info& out)
	{
		out = {};
		const size_t slots = avail / 16;
		size_t index = 0;
		for (;; ++index)
		{
			if (index >= slots) return false;
			const u32 w0 = le32(p + index * 16);
			if ((w0 >> 16) & 0x3F) break;
			if ((w0 >> 8) & 1)
			{
				out.start = u32(index * 16);
				out.length = 16;
				out.nop = true;
				return true;
			}
		}
		out.start = u32(index * 16);

		for (;;)
		{
			if (index >= slots || index > 8192) return false;
			const u8* s = p + index * 16;
			const u32 w0 = le32(s);
			const u32 d0 = ((w0 & 0x00FF00FFu) << 8) | ((w0 & 0xFF00FF00u) >> 8); // OPDEST::from_be32
			const u32 op = (d0 >> 24) & 0x3F;
			switch (op)
			{
			case 0x17: case 0x18: case 0x19: case 0x2F: case 0x31: case 0x33: case 0x34: // TEX TXP TXD TXL TXB TEXBEM TXPBEM
			{
				const u32 unit = (d0 >> 17) & 0xF;
				out.textures |= u16(1u << unit);
				if ((d0 >> 21) & 1) out.bx2 |= u16(1u << unit);
				break;
			}
			default: break;
			}
			if (!fp_opcode_known(op)) out.unknown_opcode = true;
			out.instructions++;

			if (fp_slot_has_constant(s))
			{
				index++;
			}
			index++;
			if ((w0 >> 8) & 1) break;
		}
		if (index > slots) return false;
		out.length = u32(index * 16 - out.start);
		return true;
	}

	u64 fp_hash(const u8* p, size_t len)
	{
		u64 acc0 = 0, acc1 = 0;
		const int n = int(len / 16);
		for (int i = 0; i < n; i++)
		{
			const u8* s = p + size_t(i) * 16;
			acc0 += rotr64(le64(s), unsigned(i * 2));
			acc1 += rotr64(le64(s + 8), unsigned(i * 2 + 1));
			if (fp_slot_has_constant(s)) i++;
		}
		return acc0 + acc1;
	}

	namespace
	{
		struct bits544
		{
			u64 w[9]{};
			bool test(u32 i) const { return (w[i >> 6] >> (i & 63)) & 1; }
			void set(u32 i) { w[i >> 6] |= u64(1) << (i & 63); }
		};
	}

	bool vp_analyse(const u32* data, u32 slot_count, u32 entry, vertex_program& dst)
	{
		bits544 mask, patch;
		u32 range_first = ~0u, range_second = 0;
		bool has_branch = false, failed = false;
		std::stack<u32, std::vector<u32>> call_stack;
		u16 textures = 0;

		auto inst = [&](u32 i, u32 k) -> u32 { return i < slot_count ? data[i * 4 + k] : 0u; };

		std::function<void(u32, bool)> walk = [&](u32 start, bool fast_exit)
		{
			u32 cur = start;
			std::set<u32> conditional_targets;
			while (true)
			{
				if (cur >= max_vp_instructions) { failed = true; return; }
				if (mask.test(cur)) break;

				const u32 d0 = inst(cur, 0), d1 = inst(cur, 1), d2 = inst(cur, 2), d3 = inst(cur, 3);
				mask.set(cur);
				range_first = std::min(range_first, cur);
				range_second = std::max(range_second, cur);

				const u32 vec_op = (d1 >> 22) & 0x1F;
				const u32 sca_op = (d1 >> 27) & 0x1F;
				if (vec_op == 0x19) textures |= u16(1u << ((d2 >> 8) & 3)); // TXL

				bool static_jump = false, function_call = true, handled = false;
				switch (sca_op)
				{
				case 0x09: // BRI
					static_jump = ((d0 >> 10) & 7) == 7;
					[[fallthrough]];
				case 0x11: // BRB
					function_call = false;
					[[fallthrough]];
				case 0x0A: case 0x0B: case 0x12: // CAL CLI CLB
				{
					patch.set(cur);
					has_branch = true;
					const u32 target = (((d0 >> 23) & 1) << 9) | ((d2 & 0x3F) << 3) | ((d3 >> 29) & 7);
					if (function_call)
					{
						call_stack.push(cur + 1);
						cur = target;
						handled = true;
					}
					else if (static_jump)
					{
						cur = target;
						handled = true;
					}
					else
					{
						conditional_targets.insert(target);
						range_second = std::max(range_second, target);
					}
					break;
				}
				case 0x0C: // RET
					if (!call_stack.empty())
					{
						cur = call_stack.top();
						call_stack.pop();
						handled = true;
					}
					break;
				default: break;
				}
				if (handled) continue;

				if (d3 & 1)
				{
					if (!has_branch || fast_exit || cur >= range_second) break;
				}
				else if (cur + 1 == max_vp_instructions)
				{
					break;
				}
				cur++;
			}
			for (const u32 t : conditional_targets)
			{
				if (failed) return;
				if (t >= max_vp_instructions) { failed = true; return; }
				if (!mask.test(t)) walk(t, true);
			}
		};

		walk(entry, false);
		if (failed || range_first > range_second) return false;

		const u32 count = range_second - range_first + 1;
		dst.base = range_first;
		dst.entry = entry;
		dst.textures = textures;
		dst.data.assign(size_t(count) * 4, 0);
		for (int i = 0; i < 9; i++) dst.instruction_mask[i] = 0;
		for (u32 i = range_first; i <= range_second; i++)
		{
			if (mask.test(i)) dst.instruction_mask[(i - range_first) >> 6] |= u64(1) << ((i - range_first) & 63);
		}

		std::set<u32> jump_table;
		for (u32 i = range_first, n = 0; i <= range_second; ++i, ++n)
		{
			if (!mask.test(i)) continue; // zeroed
			u32* d = &dst.data[size_t(n) * 4];
			for (int k = 0; k < 4; k++) d[k] = inst(i, k);
			if (has_branch && patch.test(i))
			{
				u32 address = (((d[0] >> 23) & 1) << 9) | ((d[2] & 0x3F) << 3) | ((d[3] >> 29) & 7);
				address -= range_first;
				d[0] = (d[0] & ~(1u << 23)) | (((address >> 9) & 1) << 23);
				d[2] = (d[2] & ~0x3Fu) | ((address >> 3) & 0x3F);
				d[3] = (d[3] & ~(7u << 29)) | ((address & 7) << 29);
				jump_table.insert(address);
			}
		}
		if (has_branch && range_first < entry) jump_table.insert(entry - range_first);
		dst.jump_table.assign(jump_table.begin(), jump_table.end());
		return true;
	}

	u64 vp_hash(const vertex_program& vp)
	{
		u64 acc0 = 0, acc1 = 0;
		const u32 n = u32(vp.data.size() / 4);
		u32 i = 0;
		do
		{
			if ((vp.instruction_mask[i >> 6] >> (i & 63)) & 1)
			{
				const u32* d = &vp.data[size_t(i) * 4];
				const u64 lo = u64(d[0]) | (u64(d[1]) << 32);
				const u64 hi = u64(d[2]) | (u64(d[3]) << 32);
				acc0 += rotr64(lo, i * 2);
				acc1 += rotr64(hi, i * 2 + 1);
			}
			i++;
		} while (i < n);
		return acc0 + acc1;
	}
}
