// Engine-agnostic front end: finds headerless RSX microcode by instruction-stream signature.
// SPDX-License-Identifier: GPL-2.0-only
//
// Fragment programs: a 4-byte aligned run of 16-byte slots that decodes as valid RSX FP instructions (known
// opcodes, legal source register types, legal input attribute) up to an END bit. Vertex programs: a run of valid
// big-endian VP instructions (known vector/scalar opcodes, legal source types) ending with an END bit. Register
// state that lives outside the microcode (texcoord control, centroid mask, VP output mask) is unknown here and is
// approximated, so these entries match the runtime cache key less often than an engine front end's.
#include "frontend.h"

#include <algorithm>

namespace shaderscan
{
	namespace
	{
		bool fp_slot_plausible(const u8* s)
		{
			const u32 w0 = le32(s);
			const u32 d0 = ((w0 & 0x00FF00FFu) << 8) | ((w0 & 0xFF00FF00u) >> 8);
			if (((d0 >> 13) & 0xF) > 14) return false; // input attribute
			for (int k = 1; k < 4; k++)
				if (((le32(s + k * 4) >> 8) & 3) == 3) return false; // reg type 3 is unused
			return true;
		}

		bool vp_slot_plausible(const u32* d)
		{
			const u32 vec = (d[1] >> 22) & 0x1F, sca = (d[1] >> 27) & 0x1F;
			if (!(vec <= 0x16 || vec == 0x19) || sca > 0x14) return false;
			if (!vec && !sca) return false;
			if (vec && (((d[2] >> 23) & 3) == 0)) return false; // src0 type
			return true;
		}

		// RPCS3 output_mask bit per VP output register (o[1]..o[15]); o[0] (position) is always written
		u32 vp_output_bit(u32 reg)
		{
			static const u32 bits[16] = { 0, 1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 14, 1u << 15, 1u << 16, 1u << 17,
				1u << 18, 1u << 19, 1u << 20, 1u << 21, 0 };
			return reg < 16 ? bits[reg] : 0;
		}

		class signature_frontend final : public frontend
		{
		public:
			const char* name() const override { return "signature"; }
			bool wants_file(const std::string&) const override { return true; }

			size_t scan(const u8* d, size_t n, const std::string& source, program_set& out) override
			{
				size_t added = 0;
				// fragment programs
				for (size_t o = 0; o + 32 <= n;)
				{
					fp_info info;
					const size_t avail = std::min<size_t>(n - o, 0x4000);
					if (((le32(d + o) >> 16) & 0x3F) && fp_analyse(d + o, avail, info) && info.start == 0 && !info.unknown_opcode &&
						info.instructions >= 3)
					{
						bool ok = true;
						for (u32 i = 0; ok && i < info.length / 16;)
						{
							const u8* s = d + o + size_t(i) * 16;
							ok = fp_slot_plausible(s);
							i += fp_slot_has_constant(s) ? 2 : 1;
						}
						if (ok)
						{
							// texcoord control / centroid unknown: assume no 2D-only texcoords, no centroid
							const size_t before = out.fps.size();
							out.add_fp(d + o, info.length, 0, 0, 0, source);
							if (out.fps.size() > before) added++;
							o += info.length;
							continue;
						}
					}
					o += 4;
				}
				// vertex programs (big-endian words)
				std::vector<u32> w;
				for (size_t o = 0; o + 32 <= n; o += 4)
				{
					if (o >= 16)
					{
						u32 prev[4] = { be32(d + o - 16), be32(d + o - 12), be32(d + o - 8), be32(d + o - 4) };
						if (vp_slot_plausible(prev) && !(prev[3] & 1)) continue; // not the start of a program
					}
					w.clear();
					u32 outputs = 0;
					bool ok = false;
					for (size_t s = o; s + 16 <= n && w.size() / 4 < max_vp_instructions; s += 16)
					{
						const u32 ins[4] = { be32(d + s), be32(d + s + 4), be32(d + s + 8), be32(d + s + 12) };
						if (!vp_slot_plausible(ins)) break;
						w.insert(w.end(), ins, ins + 4);
						// vec_result (d0 bit 30) set and dst != 0x1F: writes output register d3.dst
						const u32 dst = (ins[3] >> 2) & 0x1F;
						if (dst != 0x1F) outputs |= vp_output_bit(dst);
						if (ins[3] & 1) { ok = true; break; }
					}
					if (!ok || w.size() / 4 < 3) continue;
					const size_t before = out.vps.size();
					if (out.add_vp(w.data(), u32(w.size() / 4), 0, outputs, source) && out.vps.size() > before) added++;
					o += w.size() * 4 - 4;
				}
				return added;
			}
		};
	}

	std::unique_ptr<frontend> make_signature_frontend()
	{
		return std::make_unique<signature_frontend>();
	}
}
