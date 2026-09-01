// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The model seam's private call contract.
//
// EeFpuModelCall-arm64.h narrows the vector half of the EE FPU / EFU model
// stubs per target: each stub protects q0 up to the declared kVec* extent and
// nothing above it. EEFPU_MODEL_CALL already binds the callee for q8-q31, but
// nothing binds it below q8 -- that half is caller-saved and the extent is a
// claim about what the compiler happened to allocate. A register allocator
// that reaches one q higher on some future build turns a stub into silent
// state corruption in whichever VF or Q/P slot the JIT had parked there, on a
// path that only runs at eeClampMode/vuClampMode 4.
//
// So the claim is checked against the code it is about: walk each target's
// reachable closure with vixl's own disassembler and take the highest of
// q0-q7 that appears in it, read or written.

#include "harness/EeRecTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "common/Pcsx2Defs.h"

#include "vixl/aarch64/decoder-aarch64.h"
#include "vixl/aarch64/disasm-aarch64.h"

#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>
#include <vector>

// iCOP2-arm64.cpp / microVU-arm64.cpp (PCSX2_RECOMPILER_TESTS builds only).
// Global scope -- defined outside namespaces in the arm64 sources.
int cop2TestGetModelStubCount();
const u8* cop2TestGetModelStub(int kind);
const void* cop2TestGetModelStubTarget(int kind);
int cop2TestGetModelStubVecEnd(int kind);
int mVUTestProbe_ModelStubCount();
const u8* mVUTestProbe_ModelStub(int index, int stub);
const void* mVUTestProbe_ModelStubTarget(int index, int stub);
int mVUTestProbe_ModelStubVecEnd(int index, int stub);

namespace recompiler_tests {
namespace {

using vixl::aarch64::Instruction;

struct Closure
{
	int highestLowVec = -1; // of q0-q7; -1 when the closure touches none
	int instructions = 0;
	const Instruction* indirect = nullptr; // first br/blr, which ends the walk
};

// The disassembly is scanned for register tokens rather than decoded per
// instruction form: a mention is a mention whichever operand slot it sits in,
// and reads count too, so the answer can only be too wide.
void ScanRegisterTokens(const char* text, Closure& out)
{
	for (const char* p = text; *p;)
	{
		const auto isTokenChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
		if (!isTokenChar(*p))
		{
			p++;
			continue;
		}
		const char* start = p;
		bool underscored = false;
		while (isTokenChar(*p))
		{
			underscored |= (*p == '_');
			p++;
		}
		// System register names (s3_3_c4_c4_0) read as a vector register otherwise.
		if (underscored)
			continue;
		const char c = *start;
		if (c != 'v' && c != 'q' && c != 'd' && c != 's' && c != 'h' && c != 'b')
			continue;
		const char* d = start + 1;
		int n = 0, digits = 0;
		while (d < p && std::isdigit(static_cast<unsigned char>(*d)))
		{
			n = n * 10 + (*d++ - '0');
			digits++;
		}
		// A register is the letter, its number, and at most an arrangement.
		if (digits == 0 || digits > 2 || (d != p && *d != '.'))
			continue;
		if (n < 8 && n > out.highestLowVec)
			out.highestLowVec = n;
	}
}

// Reachable-instruction walk, not a linear scan: clang parks literal pools
// past an unconditional branch, and only the control flow says which words
// are code.
Closure WalkClosure(const void* entry)
{
	vixl::aarch64::Decoder decoder;
	vixl::aarch64::Disassembler disasm;
	decoder.AppendVisitor(&disasm);

	Closure out;
	std::set<const Instruction*> seen;
	std::vector<const Instruction*> work{reinterpret_cast<const Instruction*>(entry)};

	while (!work.empty() && !out.indirect)
	{
		const Instruction* pc = work.back();
		work.pop_back();
		if (!seen.insert(pc).second)
			continue;

		decoder.Decode(pc);
		ScanRegisterTokens(disasm.GetOutput(), out);
		out.instructions++;

		using namespace vixl::aarch64;
		const Instruction* next = pc->GetNextInstruction();
		if (pc->Mask(UnconditionalBranchToRegisterFMask) == UnconditionalBranchToRegisterFixed)
		{
			// ret ends a path; br/blr leaves the closure unbounded.
			if (pc->Mask(UnconditionalBranchToRegisterMask) != RET)
				out.indirect = pc;
			continue;
		}
		if (pc->Mask(ExceptionFMask) == ExceptionFixed)
			continue; // brk/hlt/udf: whatever follows is not on this path
		if (pc->IsUncondBranchImm())
		{
			work.push_back(pc->GetImmPCOffsetTarget());
			if (pc->Mask(UnconditionalBranchMask) == BL)
				work.push_back(next);
			continue;
		}
		if (pc->IsCondBranchImm() || pc->IsCompareBranch() || pc->IsTestBranch())
			work.push_back(pc->GetImmPCOffsetTarget());
		work.push_back(next);
	}
	return out;
}

void CheckTarget(const char* who, const void* fn, int vecEnd)
{
	ASSERT_NE(fn, nullptr) << who;
	ASSERT_GE(vecEnd, 0) << who;
	const Closure c = WalkClosure(fn);
	ASSERT_EQ(c.indirect, nullptr)
		<< who << ": closure leaves through an indirect branch at " << c.indirect
		<< ", so what it touches cannot be read off the code";
	EXPECT_GT(c.instructions, 0) << who;
	EXPECT_LT(c.highestLowVec, vecEnd)
		<< who << ": closure reaches q" << c.highestLowVec << " but the stub is generated to save "
		<< (vecEnd > 0 ? "q0-q" + std::to_string(vecEnd - 1) : std::string("no vector register"));
}

// Stp/Ldp of a q pair, and the frame's own sp adjustments.
constexpr u32 kLdStPairMask = 0xFFC00000u;
constexpr u32 kStpQ = 0xAD000000u, kLdpQ = 0xAD400000u;
constexpr u32 kStpX = 0xA9000000u, kLdpX = 0xA9400000u;
constexpr u32 kRet = 0xD65F03C0u;

void CheckStub(const char* who, const u8* stub, int vecEnd)
{
	ASSERT_NE(stub, nullptr) << who;
	const u32* words = reinterpret_cast<const u32*>(stub);
	int qStores = 0, qLoads = 0, xStores = 0, xLoads = 0, i = 0;
	for (; i < 128; i++)
	{
		const u32 w = words[i];
		if ((w & kLdStPairMask) == kStpQ)
			qStores++;
		else if ((w & kLdStPairMask) == kLdpQ)
			qLoads++;
		else if ((w & kLdStPairMask) == kStpX)
			xStores++;
		else if ((w & kLdStPairMask) == kLdpX)
			xLoads++;
		else if (w == kRet)
			break;
	}
	ASSERT_LT(i, 128) << who << ": no Ret in the scan window";
	EXPECT_EQ(qStores, vecEnd / 2) << who << ": vector half does not match the declaration";
	EXPECT_EQ(qLoads, qStores) << who << ": vector half is not restored";
	EXPECT_EQ(xStores, 4) << who << ": x2-x8 plus x30 is four pairs";
	EXPECT_EQ(xLoads, xStores) << who << ": gpr half is not restored";
}

std::string Cop2Name(int kind)
{
	return "cop2 model stub " + std::to_string(kind);
}
std::string MvuName(int index, int stub)
{
	return "VU" + std::to_string(index) + " model stub " + std::to_string(stub);
}

} // namespace

TEST(ModelCallContract, TargetsStayInsideTheirDeclaredVectorHalf)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	// Any Run() (re)generates the dispatchers, and the stubs with them.
	EeRecTestHarness h;
	h.LoadProgram({mips::NOP});
	h.Run();

	ASSERT_GT(cop2TestGetModelStubCount(), 0);
	for (int kind = 0; kind < cop2TestGetModelStubCount(); kind++)
	{
		CheckTarget(Cop2Name(kind).c_str(), cop2TestGetModelStubTarget(kind),
			cop2TestGetModelStubVecEnd(kind));
	}

	ASSERT_GT(mVUTestProbe_ModelStubCount(), 0);
	for (int index = 0; index < 2; index++)
	{
		for (int stub = 0; stub < mVUTestProbe_ModelStubCount(); stub++)
		{
			CheckTarget(MvuName(index, stub).c_str(), mVUTestProbe_ModelStubTarget(index, stub),
				mVUTestProbe_ModelStubVecEnd(index, stub));
		}
	}
}

TEST(ModelCallContract, StubsSaveExactlyWhatTheyDeclare)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	// Any Run() (re)generates the dispatchers, and the stubs with them.
	EeRecTestHarness h;
	h.LoadProgram({mips::NOP});
	h.Run();

	for (int kind = 0; kind < cop2TestGetModelStubCount(); kind++)
		CheckStub(Cop2Name(kind).c_str(), cop2TestGetModelStub(kind), cop2TestGetModelStubVecEnd(kind));

	for (int index = 0; index < 2; index++)
	{
		for (int stub = 0; stub < mVUTestProbe_ModelStubCount(); stub++)
		{
			CheckStub(MvuName(index, stub).c_str(), mVUTestProbe_ModelStub(index, stub),
				mVUTestProbe_ModelStubVecEnd(index, stub));
		}
	}
}

} // namespace recompiler_tests
