// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Which value of a VI does a conditional branch read when integer ops wrote
// that VI shortly before it?
//
// The console reads the VI as it stood before the EARLIEST write inside the
// last four cycles, provided the pair directly before the branch is one of
// those writes (autocases_vubranch_vidw.h has the console rows; this file
// sweeps the same rule across more setters, writers, fillers and delay
// slots, scoring both engines against it and against each other).
//
// microVU reads that value because analyzeBranchVI (microVU_Analyze.inl,
// shared by the x86 and the ARM64 recompilers) walks back up to four cycles
// from the branch and backs the VI up at the earliest write to it. The
// interpreter used to read the value before the LAST write when anything but
// another write to the same VI sat between two writes; _vuBackupVI now keeps
// the last three writes and picks the earliest one inside the window.
//
// So with   setter ; <lead pairs> ; writer ; branch   and the setter inside the
// window (one or two lead pairs), the branch reads the VI as it stood before
// the setter. A setter next to the writer (no lead pair) gives the same value;
// a setter outside the window (three or more) is visible, so the branch reads
// the setter's value.
//
// The shapes below are straight-line code with a forward branch:
//
//     setter   vi1 = X          (memory held P)
//     lead     0..4 pairs
//     writer   vi1 = f(vi1, k)  (X -> Y)
//     gap      0 or 1 pair
//     branch   on vi1
//     delay slot
//     fall-through: vi3 = 1, end
//     target:       vi4 = 1, end
//
// and the post-state of microVU is diffed against the interpreter's on both
// VUs. A shape is kept only when the branch decides differently on P and X
// (or, with no setter, on X and Y), so that reading the wrong value shows in
// vi3/vi4.

#include "harness/VuTestHarness.h"
#include "harness/VuEncode.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp Nop() { return U(VNOP_U()); }

// The VI the branch tests.
constexpr u32 kV = vi::vi1;

// ---- the branch -----------------------------------------------------------

struct Branch
{
	const char* name;
	u32 (*mk)(s16 imm);
	bool (*taken)(u16 v);
};

const Branch kBranches[] = {
	{"IBNE vi1,vi0", [](s16 i) { return VIBNE_L(vi::vi1, vi::vi0, i); }, [](u16 v) { return v != 0; }},
	{"IBNE vi0,vi1", [](s16 i) { return VIBNE_L(vi::vi0, vi::vi1, i); }, [](u16 v) { return v != 0; }},
	{"IBEQ vi1,vi0", [](s16 i) { return VIBEQ_L(vi::vi1, vi::vi0, i); }, [](u16 v) { return v == 0; }},
	{"IBEQ vi0,vi1", [](s16 i) { return VIBEQ_L(vi::vi0, vi::vi1, i); }, [](u16 v) { return v == 0; }},
	{"IBLTZ", [](s16 i) { return VIBLTZ_L(vi::vi1, i); }, [](u16 v) { return static_cast<s16>(v) < 0; }},
	{"IBGTZ", [](s16 i) { return VIBGTZ_L(vi::vi1, i); }, [](u16 v) { return static_cast<s16>(v) > 0; }},
	{"IBLEZ", [](s16 i) { return VIBLEZ_L(vi::vi1, i); }, [](u16 v) { return static_cast<s16>(v) <= 0; }},
	{"IBGEZ", [](s16 i) { return VIBGEZ_L(vi::vi1, i); }, [](u16 v) { return static_cast<s16>(v) >= 0; }},
};

// ---- the setter: an instruction that leaves vi1 = X -----------------------

struct Setter
{
	const char* name;
	bool (*ok)(u16 x);
	VuOp (*mk)(u16 x);
	void (*seed)(VuTestHarness& h, u16 x); // operands the setter reads
};

const Setter kSetters[] = {
	{"IADDIU", [](u16 x) { return x <= 0x7fff; }, [](u16 x) { return L(VIADDIU_L(kV, vi::vi0, x)); }, nullptr},
	{"IADDI", [](u16 x) { return x <= 15 || x >= 0xfff0; }, [](u16 x) { return L(VIADDI_L(kV, vi::vi0, static_cast<s16>(x))); }, nullptr},
	{"IADD", [](u16) { return true; }, [](u16) { return L(VIADD_L(kV, vi::vi5, vi::vi6)); },
		[](VuTestHarness& h, u16 x) { h.SetVi(5, static_cast<u16>(x - 5)); h.SetVi(6, 5); }},
	{"IAND", [](u16) { return true; }, [](u16) { return L(VIAND_L(kV, vi::vi5, vi::vi6)); },
		[](VuTestHarness& h, u16 x) { h.SetVi(5, x | 0x4000); h.SetVi(6, x | 0x2000); }},
	{"IOR", [](u16) { return true; }, [](u16) { return L(VIOR_L(kV, vi::vi5, vi::vi6)); },
		[](VuTestHarness& h, u16 x) { h.SetVi(5, x & 0xaaaa); h.SetVi(6, x & 0x5555); }},
	{"ISUB", [](u16) { return true; }, [](u16) { return L(VISUB_L(kV, vi::vi5, vi::vi6)); },
		[](VuTestHarness& h, u16 x) { h.SetVi(5, static_cast<u16>(x + 7)); h.SetVi(6, 7); }},
};

// ---- the writer: the instruction directly before the branch ---------------

struct Writer
{
	const char* name;
	bool (*kOk)(u32 k);
	VuOp (*mk)(u32 k);
	u16 (*apply)(u16 x, u32 k); // vi1 after the writer
	void (*seed)(VuTestHarness& h, u32 k);
};

const Writer kWriters[] = {
	{"ISUBIU", [](u32 k) { return k <= 0x7fff; }, [](u32 k) { return L(VISUBIU_L(kV, kV, k)); },
		[](u16 x, u32 k) { return static_cast<u16>(x - k); }, nullptr},
	{"IADDIU", [](u32 k) { return k <= 0x7fff; }, [](u32 k) { return L(VIADDIU_L(kV, kV, k)); },
		[](u16 x, u32 k) { return static_cast<u16>(x + k); }, nullptr},
	{"IADDI", [](u32 k) { return k <= 15; }, [](u32 k) { return L(VIADDI_L(kV, kV, static_cast<s32>(k))); },
		[](u16 x, u32 k) { return static_cast<u16>(x + k); }, nullptr},
	{"IADD", [](u32) { return true; }, [](u32) { return L(VIADD_L(kV, kV, vi::vi7)); },
		[](u16 x, u32 k) { return static_cast<u16>(x + k); }, [](VuTestHarness& h, u32 k) { h.SetVi(7, k); }},
	{"ISUB", [](u32) { return true; }, [](u32) { return L(VISUB_L(kV, kV, vi::vi7)); },
		[](u16 x, u32 k) { return static_cast<u16>(x - k); }, [](VuTestHarness& h, u32 k) { h.SetVi(7, k); }},
	{"IAND", [](u32) { return true; }, [](u32) { return L(VIAND_L(kV, kV, vi::vi7)); },
		[](u16 x, u32 k) { return static_cast<u16>(x & k); }, [](VuTestHarness& h, u32 k) { h.SetVi(7, k); }},
	{"IOR", [](u32) { return true; }, [](u32) { return L(VIOR_L(kV, kV, vi::vi7)); },
		[](u16 x, u32 k) { return static_cast<u16>(x | k); }, [](VuTestHarness& h, u32 k) { h.SetVi(7, k); }},
	// Writers whose source is not vi1.
	{"IADDIU vi1,vi0", [](u32 k) { return k <= 0x7fff; }, [](u32 k) { return L(VIADDIU_L(kV, vi::vi0, k)); },
		[](u16, u32 k) { return static_cast<u16>(k); }, nullptr},
	{"IADD vi1,vi5,vi6", [](u32) { return true; }, [](u32) { return L(VIADD_L(kV, vi::vi5, vi::vi6)); },
		[](u16, u32 k) { return static_cast<u16>(k); }, [](VuTestHarness& h, u32 k) { h.SetVi(5, k); h.SetVi(6, 0); }},
};

// ---- the pairs between the setter and the writer --------------------------

enum class Filler
{
	Nop,        // a NOP pair
	OtherWrite, // a write to vi2
	ReadVi1,    // a read of vi1 into vi9
};

const char* FillerName(Filler f)
{
	switch (f)
	{
		case Filler::Nop: return "NOP";
		case Filler::OtherWrite: return "write vi2";
		case Filler::ReadVi1: return "read vi1";
	}
	return "?";
}

VuOp FillerPair(Filler f)
{
	switch (f)
	{
		case Filler::Nop: return Nop();
		case Filler::OtherWrite: return L(VIADDIU_L(vi::vi2, vi::vi2, 1));
		case Filler::ReadVi1: return L(VIADD_L(vi::vi9, kV, vi::vi0));
	}
	return Nop();
}

// ---- what sits between the writer and the branch --------------------------

enum class Gap
{
	None,    // the branch is the next pair after the writer
	OneNop,  // one NOP pair: the branch reads the new value
	OtherVi, // a write to another VI: the branch reads the new value
};

const char* GapName(Gap g)
{
	switch (g)
	{
		case Gap::None: return "no gap";
		case Gap::OneNop: return "one NOP pair";
		case Gap::OtherVi: return "write to vi3";
	}
	return "?";
}

// ---- the branch's delay slot ----------------------------------------------

struct Delay
{
	const char* name;
	VuOp op;
};

const Delay kDelays[] = {
	{"NOP", Nop()},
	{"write another VI", L(VIADDIU_L(vi::vi9, vi::vi0, 7))},
	{"rewrite vi1", L(VIADDIU_L(kV, kV, 3))},
	{"read vi1", L(VIADD_L(vi::vi9, kV, vi::vi0))},
	{"read and write vi1", L(VIADD_L(kV, kV, kV))},
};

// ---- program --------------------------------------------------------------

struct Shape
{
	const Setter* setter; // null: no setter, vi1 is X in memory
	u32 lead;             // pairs between the setter and the writer
	Filler filler;
	const Writer* writer;
	const Branch* branch;
	u16 p;                // vi1 in memory before the program
	u16 x;                // vi1 when the writer runs (equal to p when there is no setter)
	u32 k;
	Gap gap;
	const Delay* delay;
};

std::vector<VuOp> Build(const Shape& s)
{
	std::vector<VuOp> p;
	if (s.setter)
	{
		p.push_back(s.setter->mk(s.x));
		for (u32 i = 0; i < s.lead; i++)
			p.push_back(FillerPair(s.filler));
	}
	p.push_back(s.writer->mk(s.k));
	if (s.gap == Gap::OneNop)
		p.push_back(Nop());
	else if (s.gap == Gap::OtherVi)
		p.push_back(L(VIADDIU_L(vi::vi3, vi::vi0, 0)));
	// After the branch: its delay slot, the fall-through (E) and its E delay
	// pair, then the target (E), so the target is three pairs past the delay
	// slot.
	p.push_back(L(s.branch->mk(static_cast<s16>(3))));
	p.push_back(s.delay->op);
	p.push_back(EBit(L(VIADDIU_L(vi::vi3, vi::vi0, 1))));
	p.push_back(Nop());
	p.push_back(EBit(L(VIADDIU_L(vi::vi4, vi::vi0, 1))));
	return p;
}

std::string Describe(const Shape& s, int vu)
{
	char buf[320];
	if (s.setter)
		std::snprintf(buf, sizeof(buf), "vu%d  %s p=%04x x=%04x, %u x %s, %s k=%x, %s, %s, delay: %s", vu, s.setter->name, s.p, s.x,
			s.lead, FillerName(s.filler), s.writer->name, s.k, GapName(s.gap), s.branch->name, s.delay->name);
	else
		std::snprintf(buf, sizeof(buf), "vu%d  (vi1 from memory) x=%04x, %s k=%x, %s, %s, delay: %s", vu, s.x, s.writer->name, s.k,
			GapName(s.gap), s.branch->name, s.delay->name);
	return buf;
}

struct Result
{
	std::vector<std::string> diffs; // microVU against the interpreter
	bool modelOk;                   // both engines decided on the value the header says the console reads
	VuSnapshot jit, interp;
};

// The value the branch reads (see the header). The setter is inside the window
// when the writer is at most three cycles after it: with `lead` NOP-like pairs
// between them, that is lead <= 2.
u16 ExpectedSeen(const Shape& s)
{
	if (s.gap != Gap::None)
		return s.writer->apply(s.x, s.k);
	if (!s.setter)
		return s.x;
	return s.lead <= 2 ? s.p : s.x;
}

Result RunShape(int vu, const Shape& s)
{
	VuTestHarness h(vu);
	h.SetVi(kV, s.p);
	if (s.setter && s.setter->seed)
		s.setter->seed(h, s.x);
	if (s.writer->seed)
		s.writer->seed(h, s.k);
	h.LoadProgram(Build(s));
	h.RunNoDiff();
	Result r;
	r.jit = h.JitSnapshot();
	r.interp = h.InterpSnapshot();
	r.diffs = DiffVu(r.jit, r.interp);
	const bool took = s.branch->taken(ExpectedSeen(s));
	auto decided = [&](const VuSnapshot& snap) {
		return took ? (snap.regs.VI[4].UL == 1 && snap.regs.VI[3].UL == 0)
		            : (snap.regs.VI[3].UL == 1 && snap.regs.VI[4].UL == 0);
	};
	r.modelOk = decided(r.jit) && decided(r.interp);
	return r;
}

// Values to try.
const u16 kPs[] = {0, 1, 0x8000};
const u16 kXs[] = {0, 1, 2, 5, 0x7fff, 0x8000, 0xffff};
const u32 kKs[] = {1, 2};

template <typename F>
void ForEachShape(const Setter* setter, u32 lead, Filler filler, const Writer* writer, const Branch* branch, Gap gap, const Delay* delay, F&& f)
{
	for (u16 p : kPs)
		for (u16 x : kXs)
		{
			if (setter)
			{
				if (!setter->ok(x) || branch->taken(p) == branch->taken(x))
					continue;
			}
			else if (p != x) // vi1 is X in memory: run each X once
				continue;
			for (u32 k : kKs)
			{
				if (!writer->kOk(k))
					continue;
				if (!setter && branch->taken(x) == branch->taken(writer->apply(x, k)))
					continue;
				f(Shape{setter, lead, filler, writer, branch, p, x, k, gap, delay});
			}
		}
}

struct Tally
{
	u32 runs = 0, bad = 0, modelBad = 0;
	std::map<std::string, std::pair<u32, u32>> by; // "dimension: value" -> {runs that differ, runs}
	std::string report;
};

// Records the run against every dimension of its shape, so a failing test's
// message says which values of each dimension differ and which do not.
void Note(Tally& t, int vu, const Shape& s, const Result& r)
{
	t.runs++;
	if (!r.modelOk)
		t.modelBad++;
	const bool differs = !r.diffs.empty();
	const std::string keys[] = {
		"vu: " + std::to_string(vu),
		std::string("setter: ") + (s.setter ? s.setter->name : "(none, vi1 from memory)"),
		std::string("writer: ") + s.writer->name,
		std::string("branch: ") + s.branch->name,
		std::string("pairs between setter and writer: ") + (s.setter ? std::to_string(s.lead) : "-"),
		std::string("pair type between setter and writer: ") + (s.setter && s.lead ? FillerName(s.filler) : "-"),
		std::string("pair between writer and branch: ") + GapName(s.gap),
		std::string("delay slot: ") + s.delay->name,
	};
	for (const std::string& k : keys)
	{
		auto& e = t.by[k];
		e.second++;
		if (differs)
			e.first++;
	}
	if (!differs)
		return;
	t.bad++;
	if (t.bad <= 4)
	{
		t.report += Describe(s, vu) + ":\n";
		for (size_t i = 0; i < r.diffs.size() && i < 4; i++)
			t.report += "    " + r.diffs[i] + "\n";
	}
}

std::string Summary(const Tally& t, const char* what)
{
	std::string s = std::string(what) + ": " + std::to_string(t.bad) + " of " + std::to_string(t.runs) + " runs differ from the interpreter";
	if (t.modelBad)
		s += "; " + std::to_string(t.modelBad) + " runs where an engine read a value other than the one the file header states";
	s += "\n";
	for (const auto& kv : t.by)
		s += "  " + kv.first + ": " + std::to_string(kv.second.first) + " of " + std::to_string(kv.second.second) + "\n";
	return s + t.report;
}

} // namespace

// ---- the setter is one or two pairs before the writer ---------------------

// Each setter, before ISUBIU / IADDIU, every branch, with one and two filler
// pairs of each kind, NOP delay slot.
TEST(MvuViBackup, SetterOneOrTwoPairsBeforeTheWriter)
{
	Tally t;
	for (int vu = 0; vu < 2; vu++)
		for (const Setter& su : kSetters)
			for (u32 lead : {1u, 2u})
				for (Filler fl : {Filler::Nop, Filler::OtherWrite, Filler::ReadVi1})
					for (const Writer& w : {kWriters[0], kWriters[1]})
						for (const Branch& b : kBranches)
							ForEachShape(&su, lead, fl, &w, &b, Gap::None, &kDelays[0], [&](const Shape& s) {
								Note(t, vu, s, RunShape(vu, s));
							});
	EXPECT_EQ(t.modelBad, 0u) << Summary(t, "setter variants");
	EXPECT_GT(t.runs, 100u);
	EXPECT_EQ(t.bad, 0u) << Summary(t, "setter variants");
}

// Each writer before every branch; IADDIU setter, one NOP pair between.
TEST(MvuViBackup, EveryWriterAfterAGappedSetter)
{
	Tally t;
	for (int vu = 0; vu < 2; vu++)
		for (const Writer& w : kWriters)
			for (const Branch& b : kBranches)
				ForEachShape(&kSetters[0], 1, Filler::Nop, &w, &b, Gap::None, &kDelays[0], [&](const Shape& s) {
					Note(t, vu, s, RunShape(vu, s));
				});
	EXPECT_EQ(t.modelBad, 0u) << Summary(t, "writer variants");
	EXPECT_GT(t.runs, 100u);
	EXPECT_EQ(t.bad, 0u) << Summary(t, "writer variants");
}

// Each delay-slot content; IADDIU setter, one NOP pair between, ISUBIU or
// IADDIU as the writer.
TEST(MvuViBackup, EveryDelaySlotAfterAGappedSetter)
{
	Tally t;
	for (int vu = 0; vu < 2; vu++)
		for (const Delay& d : kDelays)
			for (const Branch& b : kBranches)
				for (const Writer& w : {kWriters[0], kWriters[1]})
					ForEachShape(&kSetters[0], 1, Filler::Nop, &w, &b, Gap::None, &d, [&](const Shape& s) {
						Note(t, vu, s, RunShape(vu, s));
					});
	EXPECT_EQ(t.modelBad, 0u) << Summary(t, "delay slot variants");
	EXPECT_GT(t.runs, 100u);
	EXPECT_EQ(t.bad, 0u) << Summary(t, "delay slot variants");
}

// The counted loop the fault was first seen in: vi1 set by IADDIU, a counter
// pair, then ISUBIU directly before IBNE. The setter is two cycles before the
// decrement, inside the window, so the branch reads vi1 as it stood before the
// IADDIU (zero) and the loop runs once on the console and in both engines. The
// interpreter used to read the IADDIU's value and run trip+1 iterations.
TEST(MvuViBackup, CountedLoopWithAPairBetweenSetterAndDecrement)
{
	for (int vu = 0; vu < 2; vu++)
		for (u32 trip : {1u, 2u, 3u})
		{
			SCOPED_TRACE(::testing::Message() << "vu" << vu << " trip " << trip);
			std::vector<VuOp> p;
			p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, trip)));
			const s32 head = static_cast<s32>(p.size());
			p.push_back(L(VIADDIU_L(vi::vi2, vi::vi2, 1))); // iteration count
			p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
			const s32 at = static_cast<s32>(p.size());
			p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1))));
			p.push_back(Nop());
			p.push_back(EBit(Nop()));

			VuTestHarness h(vu);
			h.LoadProgram(p);
			h.RunNoDiff();
			const VuSnapshot jit = h.JitSnapshot();
			const VuSnapshot interp = h.InterpSnapshot();
			EXPECT_EQ(interp.regs.VI[2].UL, 1u) << "interpreter iteration count";
			EXPECT_EQ(interp.regs.VI[1].UL & 0xffff, trip - 1) << "interpreter's final vi1";
			EXPECT_EQ(jit.regs.VI[2].UL, 1u) << "microVU iteration count";
			EXPECT_EQ(jit.regs.VI[1].UL & 0xffff, trip - 1) << "microVU's final vi1";
		}
}

// ---- the shapes the two engines always agreed on --------------------------

// No setter, a setter in the pair right before the writer, a setter outside
// the four-pair window, and a pair between the writer and the branch. The
// interpreter read the same value here before its window changed; these pin
// that the change did not move them.
TEST(MvuViBackup, ShapesTheEnginesAgreeOn)
{
	Tally t;
	for (int vu = 0; vu < 2; vu++)
		for (const Writer& w : {kWriters[0], kWriters[1]})
			for (const Branch& b : kBranches)
			{
				// vi1 is X in memory, the branch is directly after the writer.
				ForEachShape(nullptr, 0, Filler::Nop, &w, &b, Gap::None, &kDelays[0], [&](const Shape& s) {
					Note(t, vu, s, RunShape(vu, s));
				});
				for (const Setter& su : {kSetters[0], kSetters[2]})
				{
					// The setter in the pair right before the writer (one chain),
					// and three and four pairs before it (outside the window).
					for (u32 lead : {0u, 3u, 4u})
						ForEachShape(&su, lead, Filler::Nop, &w, &b, Gap::None, &kDelays[0], [&](const Shape& s) {
							Note(t, vu, s, RunShape(vu, s));
						});
					// A pair between the writer and the branch: the new value is read.
					for (Gap g : {Gap::OneNop, Gap::OtherVi})
						for (u32 lead : {0u, 1u, 2u})
							ForEachShape(&su, lead, Filler::OtherWrite, &w, &b, g, &kDelays[0], [&](const Shape& s) {
								Note(t, vu, s, RunShape(vu, s));
							});
				}
			}
	EXPECT_EQ(t.modelBad, 0u) << Summary(t, "agreeing shapes");
	EXPECT_GT(t.runs, 500u);
	EXPECT_EQ(t.bad, 0u) << Summary(t, "agreeing shapes");
}

// What each engine reads, by how many pairs sit between the setter and the
// writer: the value before the setter (P) up to two pairs, the setter's value
// (X) from three. P = 0 (memory), X = 1 (the setter), Y = 2 (the writer);
// IBEQ vi1,vi8 with vi8 = P, X, Y in turn tells the three apart.
TEST(MvuViBackup, ValueEachEngineReadsBySetterDistance)
{
	const char* const name[] = {"P", "X", "Y"};
	const int want[5] = {0, 0, 0, 1, 1};
	for (int vu = 0; vu < 2; vu++)
		for (u32 between = 0; between < 5; between++)
		{
			int seenInterp = -1, seenJit = -1;
			for (int which = 0; which < 3; which++)
			{
				std::vector<VuOp> p;
				p.push_back(L(VIADDIU_L(kV, vi::vi0, 1)));
				for (u32 i = 0; i < between; i++)
					p.push_back(Nop());
				p.push_back(L(VIADDIU_L(kV, kV, 1)));
				p.push_back(L(VIBEQ_L(kV, vi::vi8, 3)));
				p.push_back(Nop());
				p.push_back(EBit(L(VIADDIU_L(vi::vi3, vi::vi0, 1))));
				p.push_back(Nop());
				p.push_back(EBit(L(VIADDIU_L(vi::vi4, vi::vi0, 1))));
				VuTestHarness h(vu);
				h.SetVi(8, which);
				h.LoadProgram(p);
				h.RunNoDiff();
				if (h.InterpSnapshot().regs.VI[4].UL == 1)
					seenInterp = which;
				if (h.JitSnapshot().regs.VI[4].UL == 1)
					seenJit = which;
			}
			EXPECT_EQ(seenInterp, want[between])
				<< "vu" << vu << " interpreter, " << between << " pairs between setter and writer: reads "
				<< (seenInterp >= 0 ? name[seenInterp] : "none");
			EXPECT_EQ(seenJit, want[between])
				<< "vu" << vu << " microVU, " << between << " pairs between setter and writer: reads "
				<< (seenJit >= 0 ? name[seenJit] : "none");
		}
}

// Three and four writes to one VI, with 0..2 NOP pairs between them. The
// console rows have three writes only back to back; this sweeps the spacings,
// where the earliest write still inside the window is not the first one and
// the engine has to remember the VI before each of the last three writes.
// Writes are IADDIU vi1,vi0,v_i, so the value read says which write the
// branch backed up to: P = 0 (memory) before the first, v_i after write i.
// IBEQ vi1,vi8 with vi8 = each value in turn finds the one read.
TEST(MvuViBackup, SpacedWritesToOneVi)
{
	const u32 v[5] = {0, 11, 22, 33, 44}; // v[0] is P
	u32 shapes = 0;
	for (int vu = 0; vu < 2; vu++)
		for (u32 n = 2; n <= 4; n++)
		{
			u32 combos = 1;
			for (u32 i = 1; i < n; i++)
				combos *= 3;
			for (u32 c = 0; c < combos; c++)
			{
				u32 gap[4] = {0, 0, 0, 0}; // NOP pairs after write i, i < n - 1
				for (u32 i = 0, rest = c; i + 1 < n; i++, rest /= 3)
					gap[i] = rest % 3;

				// Cycle of each write, and the earliest one within 3 cycles of the last.
				u32 t[4];
				t[0] = 0;
				for (u32 i = 1; i < n; i++)
					t[i] = t[i - 1] + 1 + gap[i - 1];
				u32 first = 0;
				while (t[n - 1] - t[first] > 3)
					first++;
				const u32 want = v[first]; // the VI before write `first`

				int seenInterp = -1, seenJit = -1;
				for (u32 which = 0; which <= n; which++)
				{
					std::vector<VuOp> p;
					for (u32 i = 0; i < n; i++)
					{
						p.push_back(L(VIADDIU_L(kV, vi::vi0, v[i + 1])));
						if (i + 1 < n)
							for (u32 g = 0; g < gap[i]; g++)
								p.push_back(Nop());
					}
					p.push_back(L(VIBEQ_L(kV, vi::vi8, 3)));
					p.push_back(Nop());
					p.push_back(EBit(L(VIADDIU_L(vi::vi3, vi::vi0, 1))));
					p.push_back(Nop());
					p.push_back(EBit(L(VIADDIU_L(vi::vi4, vi::vi0, 1))));
					VuTestHarness h(vu);
					h.SetVi(8, v[which]);
					h.LoadProgram(p);
					h.RunNoDiff();
					if (h.InterpSnapshot().regs.VI[4].UL == 1)
						seenInterp = static_cast<int>(v[which]);
					if (h.JitSnapshot().regs.VI[4].UL == 1)
						seenJit = static_cast<int>(v[which]);
				}
				shapes++;
				EXPECT_EQ(seenInterp, static_cast<int>(want))
					<< "vu" << vu << " interpreter, " << n << " writes, gaps " << gap[0] << gap[1] << gap[2];
				EXPECT_EQ(seenJit, static_cast<int>(want))
					<< "vu" << vu << " microVU, " << n << " writes, gaps " << gap[0] << gap[1] << gap[2];
			}
		}
	EXPECT_EQ(shapes, 2u * (3 + 9 + 27));
}

// A write that is far outside the window must not be read as inside it. The
// interpreter keeps the cycle of each write in one byte, so a write 256 to 259
// cycles back would look 0 to 3 cycles back if its record outlived the window.
// Setter, N NOP pairs, writer, branch: the branch reads the value before the
// writer, which is the setter's, whatever N is.
TEST(MvuViBackup, AWriteLongAgoIsNotInTheWindow)
{
	const u32 v[3] = {0, 11, 22}; // P, after the setter, after the writer
	for (int vu = 0; vu < 2; vu++)
		for (u32 nops : {3u, 100u, 252u, 253u, 254u, 255u, 256u, 257u, 258u, 259u, 260u})
		{
			int seenInterp = -1, seenJit = -1;
			for (u32 which = 0; which < 3; which++)
			{
				std::vector<VuOp> p;
				p.push_back(L(VIADDIU_L(kV, vi::vi0, v[1])));
				for (u32 i = 0; i < nops; i++)
					p.push_back(Nop());
				p.push_back(L(VIADDIU_L(kV, vi::vi0, v[2])));
				p.push_back(L(VIBEQ_L(kV, vi::vi8, 3)));
				p.push_back(Nop());
				p.push_back(EBit(L(VIADDIU_L(vi::vi3, vi::vi0, 1))));
				p.push_back(Nop());
				p.push_back(EBit(L(VIADDIU_L(vi::vi4, vi::vi0, 1))));
				VuTestHarness h(vu);
				h.SetVi(8, v[which]);
				h.LoadProgram(p);
				h.RunNoDiff();
				if (h.InterpSnapshot().regs.VI[4].UL == 1)
					seenInterp = static_cast<int>(v[which]);
				if (h.JitSnapshot().regs.VI[4].UL == 1)
					seenJit = static_cast<int>(v[which]);
			}
			EXPECT_EQ(seenInterp, static_cast<int>(v[1])) << "vu" << vu << " interpreter, " << nops << " NOP pairs between the writes";
			EXPECT_EQ(seenJit, static_cast<int>(v[1])) << "vu" << vu << " microVU, " << nops << " NOP pairs between the writes";
		}
}

} // namespace recompiler_tests
