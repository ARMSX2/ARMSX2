// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Shared plumbing for the batch-2 console corpus (`autocases_vulat.h`), used
// by both suites built on it:
//
//   vu_pipeline_console_conformance_tests.cpp        Q2, pipeline visibility
//   vu_memory_xgkick_console_conformance_tests.cpp   Q4/Q5, memory and XGKICK
//
// The corpus ships console observations only; each suite rebuilds its own
// microprograms from VuEncode.h. PrepareConsoleCase below is what keeps the
// two in step.

#pragma once

#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "autocases_vulat.h"

#include "VU.h"

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace recompiler_tests
{
namespace vulat_common
{
using vulat::Case;
using vulat::kCases;
using vulat::kNumCases;
using vu::VuOp;

constexpr u32 kSettle = 70; // pairs between the old producer and the new one
constexpr u32 kNSamp = 31;  // one sampler per register above zero

constexpr u32 kVu0Quadwords = 256;  // 4 KB
constexpr u32 kVu1Quadwords = 1024; // 16 KB

// IADDIU vi0, vi0, 0. VI0 is hardwired and _vuIADDIU returns early for
// _It_ == 0, so nothing is written on either side. Deliberately not
// VuEncode.h's NopPair(), which sets the I bit and clobbers VI[REG_I].
inline VuOp LNop()
{
	return VuOp{vu::VIADDIU_L(0, 0, 0), vu::VNOP_U()};
}

inline void PushNops(std::vector<VuOp>& p, u32 n)
{
	for (u32 i = 0; i < n; ++i)
		p.push_back(LNop());
}

inline void PushETail(std::vector<VuOp>& p)
{
	p.push_back(vu::EBit(LNop()));
	p.push_back(LNop());
}

// IADDIU's immediate is 15 bits unsigned, so the top of the u16 range is only
// reachable by subtracting from the hardwired zero register. Getting this
// wrong is not a compile error: it silently tests index 0x7FFF instead of
// 0xFFFF.
inline VuOp SetIndex(u32 vi, u32 index)
{
	if (index <= 0x7FFF)
		return VuOp{vu::VIADDIU_L(vi, 0, index), vu::VNOP_U()};
	return VuOp{vu::VISUBIU_L(vi, 0, 0x10000u - index), vu::VNOP_U()};
}

// VU1 has no CTC2/LQC2 path from the EE, so every VU1 program seeds its own
// registers out of data memory. The sentinel fill comes first, so a sampler
// slot the program never writes reads back as 0xDEADBEEF rather than as
// whatever the previous program left in that register.
inline std::vector<VuOp> Vu1Preamble()
{
	std::vector<VuOp> p;
	for (u32 n = 1; n < 32; ++n)
		p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, n, 0, static_cast<s16>(vulat::kVu1SentQw)), vu::VNOP_U()});
	for (u32 n : {4u, 5u, 6u, 7u, 8u, 9u})
		p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, n, 0, static_cast<s16>(vulat::kVu1ProfQw + n)), vu::VNOP_U()});
	PushNops(p, 4);
	return p;
}

inline u32 Fnv1a(const std::vector<VuOp>& p)
{
	u32 h = 0x811C9DC5u;
	const auto byte = [&h](u8 b) { h = (h ^ b) * 0x01000193u; };
	for (const VuOp& op : p)
	{
		for (int i = 0; i < 4; ++i)
			byte(static_cast<u8>(op.lower >> (8 * i)));
		for (int i = 0; i < 4; ++i)
			byte(static_cast<u8>(op.upper >> (8 * i)));
	}
	return h;
}

inline std::vector<const Case*> CasesOfKind(u8 kind)
{
	std::vector<const Case*> out;
	for (u32 i = 0; i < kNumCases; ++i)
	{
		if (kCases[i].kind == kind)
			out.push_back(&kCases[i]);
	}
	return out;
}

inline const Case& CaseByTag(const char* tag)
{
	for (u32 i = 0; i < kNumCases; ++i)
	{
		if (std::string(kCases[i].tag) == tag)
			return kCases[i];
	}
	ADD_FAILURE() << "no console case tagged " << tag;
	return kCases[0];
}

// Rebuild the console's pre-state, and refuse the case unless the rebuilt
// program hashes to `prog_fnv`, the FNV-1a of the words hardware ran -- a C++
// builder that drifts from the Python generator fails loudly instead of
// quietly scoring a different program. Does not execute; the caller picks
// Run/RunNoDiff/RunInterpOnly.
//
// LoadProgram wants the last pair handed to it to carry the E bit and appends
// the delay-slot NOP itself, so the corpus's trailing NOP is dropped here.
inline bool PrepareConsoleCase(const Case& c, VuTestHarness& h, std::vector<VuOp> prog)
{
	if (prog.empty())
		return false;
	EXPECT_EQ(prog.size(), c.n_pairs) << c.tag;
	EXPECT_EQ(Fnv1a(prog), c.prog_fnv)
		<< c.tag << ": the rebuilt program is not the one the console ran";
	if (prog.size() != c.n_pairs || Fnv1a(prog) != c.prog_fnv)
		return false;

	const u32 quadwords = c.vu ? kVu1Quadwords : kVu0Quadwords;
	const u32 pattern = c.vu ? vulat::kVu1Pattern : vulat::kVu0Pattern;
	for (u32 qw = 0; qw < quadwords; ++qw)
	{
		const u32 v = pattern | qw;
		h.WriteMemU128(qw * 16, v, v, v, v);
	}

	if (c.vu)
	{
		h.WriteMemU128(vulat::kVu1SentQw * 16, vulat::kSentinel, vulat::kSentinel,
		               vulat::kSentinel, vulat::kSentinel);
		for (u32 i = 0; i < 32; ++i)
		{
			h.WriteMemU128((vulat::kVu1ProfQw + i) * 16, vulat::kProfile[c.prof][i][0],
			               vulat::kProfile[c.prof][i][1], vulat::kProfile[c.prof][i][2],
			               vulat::kProfile[c.prof][i][3]);
		}
		for (u32 i = 0; i <= kNSamp; ++i)
		{
			h.WriteMemU128((vulat::kVu1SampQw + i) * 16, vulat::kSentinel,
			               vulat::kSentinel, vulat::kSentinel, vulat::kSentinel);
		}
		h.TrackMemWindow(vulat::kVu1SampQw * 16, (kNSamp + 1) * 16);
	}
	else
	{
		for (u32 n = 1; n < 32; ++n)
		{
			h.SetVfBits(n, vulat::kProfile[c.prof][n][0], vulat::kProfile[c.prof][n][1],
			            vulat::kProfile[c.prof][n][2], vulat::kProfile[c.prof][n][3]);
		}
		for (u32 i = 0; i < 15; ++i)
			h.SetVi(i + 1, c.vi_seed[i]);
		h.SetVi(REG_STATUS_FLAG, 0);
		h.SetVi(REG_CLIP_FLAG, 0);
		h.SetQ(vulat::kSeedQ);
		// P is seeded to zero, not to kSeedP: the probe wrote kSeedP through
		// CTC2 and hardware ignored it (Q2_SEED_QP_LIVENESS), so zero is the
		// console's real pre-state.
		h.SetP(0);
	}
	for (u32 i = 0; i < c.memw_count; ++i)
	{
		const vulat::MemWrite& m = vulat::kMemWrites[c.memw_first + i];
		h.WriteMemU128(m.qw * 16, m.v[0], m.v[1], m.v[2], m.v[3]);
	}

	prog.pop_back();
	h.LoadProgram(std::move(prog));
	return true;
}

// The common case: seed, then execute both engines without diffing them
// against each other -- each is scored against hardware separately.
inline bool RunConsoleCase(const Case& c, VuTestHarness& h, std::vector<VuOp> prog)
{
	if (!PrepareConsoleCase(c, h, std::move(prog)))
		return false;
	h.RunNoDiff();
	return true;
}

} // namespace vulat_common
} // namespace recompiler_tests
