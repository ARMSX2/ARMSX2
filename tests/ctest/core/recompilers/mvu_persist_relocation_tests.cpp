// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Persisted-JIT VU program cache: payloads are relocatable across processes.
//
// A payload must hydrate in a process whose emulator image, heap and code
// placement differ from the recording process's (ASLR on a PIE build, or the
// emulator as an Android shared library). These tests check:
//
//   (a) every host address in a recorded chunk is covered by a fixup (the
//       audit decodes the chunk independently of the emit helpers);
//   (b) a payload recorded by another process, whose image base differs,
//       hydrates, runs without compiling, and matches a fresh compile;
//   (c) the header's recorded bases do not matter;
//   (d) a payload whose fixup cannot be encoded at the new placement is
//       rejected, with nothing installed, before or after the blocks are
//       registered.
//
// All tests drive VU0, whose compile and execution stay on the test thread.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef __linux__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper)
{
	return IBit(VuOp{VLitZero(), upper});
}

inline VuOp LowerOnly(u32 lower)
{
	return VuOp{lower, VNOP_U()};
}

// fixup target ids, as TestEditFirstFixup takes them
constexpr u8 kTargetBlockObject = 0;
constexpr u8 kTargetImage = 3;

struct Program
{
	const char* name;
	std::function<void(VuTestHarness&)> setup;
	std::vector<VuOp> pairs;
	// Optional: what the run must leave behind, whichever engine or copy of the code ran it.
	std::function<void(const VuSnapshot&, const char* who)> check;
};

// The programs the cross-process test records in a child and hydrates in the
// parent. Between them: an E-bit end (end-flags stub, image globals), a
// conditional branch with both arms compiled (block entries, block-object
// pointers) and a JR whose target compiles in a second episode through a C
// call (image function address, jump cache).
const std::vector<Program>& Programs()
{
	static const std::vector<Program> progs = {
		{"straight",
			[](VuTestHarness& h) {
				h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
				h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
			},
			{
				UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
				UpperOnly(VMUL_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
				UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf4, vf::vf1)),
			}},
		{"branch",
			[](VuTestHarness& h) {
				h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
				h.SetVf(2, 0.5f, 0.5f, 0.5f, 0.5f);
				h.SetVi(1, 1);
			},
			{
				LowerOnly(VIBNE_L(vi::vi1, vi::vi0, 3)),
				UpperOnly(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
				UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)),
				NopPair(),
				UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf6, vf::vf1, vf::vf2)),
			}},
		{"jr",
			[](VuTestHarness& h) {
				h.SetVf(1, 2.0f, 4.0f, 8.0f, 16.0f);
				h.SetVf(2, 1.0f, 1.0f, 1.0f, 1.0f);
			},
			{
				LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 4)),
				LowerOnly(VJR_L(vi::vi1)),
				NopPair(),
				NopPair(),
				UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
			}},
	};
	return progs;
}

// VU0's window onto VU1's register file: an address index with bit 0x400 set reaches 64 quadwords of VU1's
// registers, VF0..VF31 then VI0..VI31. VU1's registers are in the emulator image and VU0's data memory is in
// the data arena, so the code that turns the index into an address spans two regions that move apart from one
// process to the next. The program loads a VF and a VI quadword through the window and stores one of each.
constexpr u32 kWindow = 0x400;
constexpr u32 kWindowVi = kWindow + 32;

constexpr u32 Vu1VfPattern(u32 reg, u32 lane) { return 0xE0000000u | (reg << 8) | (lane << 4) | 3u; }
constexpr u32 Vu1ViPattern(u32 reg) { return 0xF000u | (reg << 4) | 5u; }
constexpr u32 kStoredVf[4] = {0x11110001u, 0x22220002u, 0x33330003u, 0x44440004u};
constexpr u32 kStoredVi = 0x1234u;

const Program& WindowProgram()
{
	static const Program prog = {"vu1-window",
		[](VuTestHarness& h) {
			h.SetVi(vi::vi1, kWindow + 7); // VU1's VF[7], loaded
			h.SetVi(vi::vi2, kWindowVi + 5); // VU1's VI[5], loaded
			h.SetVi(vi::vi3, kWindow + 9); // VU1's VF[9], stored
			h.SetVi(vi::vi4, kWindowVi + 6); // VU1's VI[6], stored
			h.SetVi(vi::vi5, kStoredVi);
			h.SetVfBits(1, kStoredVf[0], kStoredVf[1], kStoredVf[2], kStoredVf[3]);
			// The harness does not drive VU1's registers; it snapshots and restores them as VU0 state.
			for (u32 r = 0; r < 32; r++)
			{
				for (u32 l = 0; l < 4; l++)
					vuRegs[1].VF[r].UL[l] = Vu1VfPattern(r, l);
				vuRegs[1].VI[r].UL = Vu1ViPattern(r);
			}
		},
		{
			LowerOnly(VLQ_L(mask::xyzw, vf::vf2, vi::vi1, 0)),
			LowerOnly(VILW_L(mask::x, vi::vi6, vi::vi2, 0)),
			LowerOnly(VSQ_L(mask::xyzw, vf::vf1, vi::vi3, 0)),
			LowerOnly(VISW_L(mask::x, vi::vi5, vi::vi4, 0)),
			EBitNopPair(),
		},
		[](const VuSnapshot& s, const char* who) {
			for (u32 l = 0; l < 4; l++)
				EXPECT_EQ(s.regs.VF[2].UL[l], Vu1VfPattern(7, l)) << who << ": LQ of VU1's VF[7], lane " << l;
			EXPECT_EQ(s.regs.VI[6].UL & 0xffffu, Vu1ViPattern(5)) << who << ": ILW of VU1's VI[5]";

			ASSERT_EQ(s.vu1_window.size(), 64u * 16u) << who;
			u32 vf9[4], vi6;
			std::memcpy(vf9, s.vu1_window.data() + 9 * 16, sizeof(vf9));
			std::memcpy(&vi6, s.vu1_window.data() + (32 + 6) * 16, sizeof(vi6));
			for (u32 l = 0; l < 4; l++)
				EXPECT_EQ(vf9[l], kStoredVf[l]) << who << ": SQ into VU1's VF[9], lane " << l;
			EXPECT_EQ(vi6 & 0xffffu, kStoredVi) << who << ": ISW into VU1's VI[6]";
			// The neighbours on each side of both stores are untouched.
			for (const u32 r : {8u, 10u})
			{
				u32 x;
				std::memcpy(&x, s.vu1_window.data() + r * 16, sizeof(x));
				EXPECT_EQ(x, Vu1VfPattern(r, 0)) << who << ": VU1's VF[" << r << "] changed";
			}
			for (const u32 r : {5u, 7u})
			{
				u32 x;
				std::memcpy(&x, s.vu1_window.data() + (32 + r) * 16, sizeof(x));
				EXPECT_EQ(x, Vu1ViPattern(r)) << who << ": VU1's VI[" << r << "] changed";
			}
		}};
	return prog;
}

// Programs() and the window program: everything the cross-process test moves.
std::vector<const Program*> AllPrograms()
{
	std::vector<const Program*> all;
	for (const Program& p : Programs())
		all.push_back(&p);
	all.push_back(&WindowProgram());
	return all;
}

void Load(VuTestHarness& h, const Program& p)
{
	p.setup(h);
	h.LoadProgram(p.pairs);
	// LoadProgram writes Micro directly, bypassing the vtlb path that clears
	// the program cache in production.
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
}

std::vector<u8> RecordProgram(const Program& p)
{
	VuTestHarness h(0);
	Load(h, p);
	h.Run();
	h.RunJitPreserveBlockCache();
	std::vector<u8> image;
	EXPECT_TRUE(mVUPersist::TestSerializeNewestProgram(0, image)) << p.name;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	return image;
}

// Hydrate `image` for `p` and check it against a fresh compile in this
// process: accepted, no compile at hydration or dispatch, identical state.
void HydrateAndCompare(const Program& p, const std::vector<u8>& image)
{
	VuTestHarness h(0);
	Load(h, p);
	h.Run(); // fresh compile in this process, and the pre-state for the replay below
	const VuSnapshot fresh = h.JitSnapshot();
	if (p.check)
	{
		p.check(h.InterpSnapshot(), "interpreter");
		p.check(fresh, "fresh compile");
	}
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);

	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	ASSERT_TRUE(mVUPersist::TestHydrate(0, image.data(), image.size())) << p.name << ": payload rejected";
	EXPECT_TRUE(mVUPersist::TestVerifyHydratedCode(0, image.data(), image.size())) << p.name;
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(0), compiles_before)
		<< p.name << ": the hydrated program compiled";
	if (p.check)
		p.check(h.JitSnapshot(), "hydrated");
	const auto diffs = DiffVu(fresh, h.JitSnapshot(), VuDiffMode::Strict);
	EXPECT_TRUE(diffs.empty()) << [&] {
		std::string s = std::string(p.name) + ": hydrated run diverged from a fresh compile:\n";
		for (const auto& d : diffs)
			s += "  " + d + "\n";
		return s;
	}();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
}

class MvuPersistRelocation : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
		mVUPersist::SetRecordingEnabled(true);
	}

	void TearDown() override
	{
		mVUPersist::TestSetAuditEnabled(false);
		mVUPersist::SetRecordingEnabled(false);
		RecompilerTestEnvironment::ResetVuBlockCache(0);
	}
};

constexpr const char* kChildEnv = "PCSX2_TEST_PERSIST_XPROC_OUT";
constexpr u32 kXprocMagic = 0x58505631u; // 'XPV1'

} // namespace

// (a) Every address in a recorded chunk has a fixup. In this PIE test binary
// the image sits above 4GB, so an unrecorded image address could only be a
// 64-bit immediate run, which the audit decodes.
TEST_F(MvuPersistRelocation, RecordedChunksCarryNoUncoveredAddress)
{
	mVUPersist::TestSetAuditEnabled(true);
	for (const Program* p : AllPrograms())
		RecordProgram(*p);
	EXPECT_EQ(mVUPersist::TestAuditUncoveredCount(), 0u) << mVUPersist::TestAuditReport();
	EXPECT_GT(mVUPersist::TestAuditCoveredCount("movseq->image"), 0u)
		<< "no image address was recorded; the probes no longer reach a C helper or global\n"
		<< mVUPersist::TestAuditReport();
	EXPECT_GT(mVUPersist::TestAuditCoveredCount("movseq->block-object"), 0u) << mVUPersist::TestAuditReport();
}

// The child half of the cross-process test: runs only when the parent sets
// the environment variable, records every program and writes the payloads.
TEST_F(MvuPersistRelocation, CrossProcessChildRecord)
{
	const char* out = std::getenv(kChildEnv);
	if (!out)
		GTEST_SKIP() << "runs as the child of HydratesPayloadFromAnotherProcess";

	u64 image_base = 0, data_base = 0;
	mVUPersist::TestGetRegionBases(image_base, data_base);
	std::FILE* f = std::fopen(out, "wb");
	ASSERT_NE(f, nullptr);
	const std::vector<const Program*> programs = AllPrograms();
	const u32 count = static_cast<u32>(programs.size());
	std::fwrite(&kXprocMagic, sizeof(kXprocMagic), 1, f);
	std::fwrite(&image_base, sizeof(image_base), 1, f);
	std::fwrite(&count, sizeof(count), 1, f);
	for (const Program* pp : programs)
	{
		const Program& p = *pp;
		const std::vector<u8> image = RecordProgram(p);
		const u32 size = static_cast<u32>(image.size());
		std::fwrite(&size, sizeof(size), 1, f);
		std::fwrite(image.data(), 1, image.size(), f);
	}
	std::fclose(f);
}

// (b) Record in a second process (this binary, re-executed: a fresh ASLR draw
// for the image, the heap and the stack), hydrate here.
TEST_F(MvuPersistRelocation, HydratesPayloadFromAnotherProcess)
{
#ifndef __linux__
	GTEST_SKIP() << "spawns this test binary through /proc/self/exe";
#else
	if (std::getenv(kChildEnv))
		GTEST_SKIP() << "already the child";

	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	ASSERT_GT(n, 0);
	exe[n] = 0;

	const std::string out = ::testing::TempDir() + "mvu_persist_xproc_" + std::to_string(getpid()) + ".bin";
	std::remove(out.c_str());

	std::vector<std::string> env_store;
	for (char** e = environ; *e; e++)
		env_store.emplace_back(*e);
	env_store.push_back(std::string(kChildEnv) + "=" + out);
	std::vector<char*> envp;
	for (std::string& e : env_store)
		envp.push_back(e.data());
	envp.push_back(nullptr);

	std::string filter = "--gtest_filter=MvuPersistRelocation.CrossProcessChildRecord";
	char* argv[] = {exe, filter.data(), nullptr};
	pid_t pid = 0;
	ASSERT_EQ(posix_spawn(&pid, exe, nullptr, nullptr, argv, envp.data()), 0);
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child failed, status " << status;

	std::FILE* f = std::fopen(out.c_str(), "rb");
	ASSERT_NE(f, nullptr) << "child wrote no payloads";
	u32 magic = 0, count = 0;
	u64 child_image_base = 0;
	ASSERT_EQ(std::fread(&magic, sizeof(magic), 1, f), 1u);
	ASSERT_EQ(magic, kXprocMagic);
	ASSERT_EQ(std::fread(&child_image_base, sizeof(child_image_base), 1, f), 1u);
	ASSERT_EQ(std::fread(&count, sizeof(count), 1, f), 1u);
	const std::vector<const Program*> programs = AllPrograms();
	ASSERT_EQ(count, programs.size());
	std::vector<std::vector<u8>> images(count);
	for (auto& image : images)
	{
		u32 size = 0;
		ASSERT_EQ(std::fread(&size, sizeof(size), 1, f), 1u);
		image.resize(size);
		ASSERT_EQ(std::fread(image.data(), 1, size, f), size);
	}
	std::fclose(f);
	std::remove(out.c_str());

	u64 image_base = 0, data_base = 0;
	mVUPersist::TestGetRegionBases(image_base, data_base);
	std::printf("[persist-xproc] image base here %#llx, in the recording process %#llx\n",
		(unsigned long long)image_base, (unsigned long long)child_image_base);
	if (image_base == child_image_base)
		GTEST_SKIP() << "both processes loaded the image at the same base (ASLR off?); "
		                "the test proves nothing about relocation";

	for (size_t i = 0; i < count; i++)
		HydrateAndCompare(*programs[i], images[i]);
#endif
}

// (c) The recorded bases in the header are informational.
TEST_F(MvuPersistRelocation, HeaderBasesDoNotGateHydration)
{
	for (const Program* pp : AllPrograms())
	{
		const Program& p = *pp;
		std::vector<u8> image = RecordProgram(p);
		ASSERT_TRUE(mVUPersist::TestOverwriteHeaderBases(image, 0x0123456789ab0000ull));
		HydrateAndCompare(p, image);
	}
}

// (d) An image address the recorded run cannot hold is caught before
// anything is installed.
TEST_F(MvuPersistRelocation, RejectsUnencodableImageAddressBeforeInstalling)
{
	const Program& p = Programs()[2]; // jr: calls a C helper
	std::vector<u8> image = RecordProgram(p);
	// Push the target past 2^48: no three-instruction run holds it.
	ASSERT_TRUE(mVUPersist::TestEditFirstFixup(image, kTargetImage, 0, s64{1} << 48));

	VuTestHarness h(0);
	Load(h, p);
	const auto before = mVUPersist::GetStats(0);
	EXPECT_FALSE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	const auto after = mVUPersist::GetStats(0);
	EXPECT_EQ(after.hydrationRejects, before.hydrationRejects + 1);
	EXPECT_EQ(after.programsHydrated, before.programsHydrated);

	h.Run();
	EXPECT_FLOAT_EQ(h.GetVfJit(3, 'x'), 3.0f);
}

// (d) A block pointer is known only once the blocks are registered; a run too
// narrow for it takes the hydrated program back out.
TEST_F(MvuPersistRelocation, RejectsUnencodableBlockPointerAfterInstalling)
{
	const Program& p = Programs()[1]; // branch: block-object pointers
	std::vector<u8> image = RecordProgram(p);
	// A one-instruction run cannot hold a heap pointer.
	ASSERT_TRUE(mVUPersist::TestEditFirstFixup(image, kTargetBlockObject, 1, 0));

	VuTestHarness h(0);
	Load(h, p);
	const auto before = mVUPersist::GetStats(0);
	EXPECT_FALSE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	const auto after = mVUPersist::GetStats(0);
	EXPECT_EQ(after.hydrationRejects, before.hydrationRejects + 1);
	EXPECT_EQ(after.programsHydrated, before.programsHydrated);

	// The discarded program must not be served: the run compiles afresh and
	// is correct.
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.Run();
	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before);
	EXPECT_FLOAT_EQ(h.GetVfJit(6, 'x'), 0.5f);
}

// (e) The image fixups of a payload are offsets into the module of the build that recorded it. Another build
// puts the same helpers elsewhere, so the payload is refused whole, before anything is installed.
TEST_F(MvuPersistRelocation, RejectsPayloadFromAnotherBuild)
{
	const Program& p = Programs()[2]; // jr: calls a C helper
	const std::vector<u8> image = RecordProgram(p);

	VuTestHarness h(0);
	Load(h, p);
	mVUPersist::TestOverrideBuildId(true, 0x1234, 0x5678);
	const auto before = mVUPersist::GetStats(0);
	EXPECT_FALSE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	const auto after = mVUPersist::GetStats(0);
	mVUPersist::TestOverrideBuildId(false, 0, 0);
	EXPECT_EQ(after.hydrationRejects, before.hydrationRejects + 1);
	EXPECT_EQ(after.programsHydrated, before.programsHydrated);

	// The same payload, back in the build that recorded it.
	h.Run();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	EXPECT_TRUE(mVUPersist::TestHydrate(0, image.data(), image.size()));
	RecompilerTestEnvironment::ResetVuBlockCache(0);
}


} // namespace recompiler_tests
