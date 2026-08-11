// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

// The draw-lowering compiler's first rung: decide from the raw register snapshot
// whether one draw can be realized natively, and if so under what write claims. Pure
// function over PODs — no GS state, no device — so the predicate sweeps in ctest.
//
// The M2 native envelope is opaque untextured draws: triangles and sprites, no
// blending, no texture, no per-pixel test that survives to the pixel (alpha test
// off-or-ALWAYS, DATE on formats where the GS itself has no destination alpha), frame
// formats whose cells upload and read back losslessly (CT32/CT24), depth formats that
// round-trip exactly through a float32 depth buffer (Z24/Z16/Z16S — a 24-bit integer
// is exact in float32, Z32 is not and floors). Everything else takes the SW floor,
// tagged with the first reason that disqualified it; the per-title distribution of
// that tag is the renderer's primary health metric.
//
// Two facts here are semantic, not convenience, and are pinned by the unit suite:
//   - ZTE=0 disables the z write as well as the z test (GSRendererSW derives
//     zm = ZMSK || !ZTE — the floor and the native path must claim identically);
//   - claims are BYTE coverage, not meaning: an unmasked color write claims the Z
//     plane of its pages too, because a depth reading of those bytes overlaps every
//     byte the write touched (the mirror of gsTilePlanesInvalidatedByWrite).

enum class GSTileFloorReason : u8
{
	None = 0, ///< native
	// Register-predicate reasons (decided by gsTileLowerDraw):
	PrimClass, ///< points/lines — not in the M2 envelope
	SpriteExpandUnavailable, ///< sprite draw without VS expand support
	Textured,
	Blend,
	CoverageAA1,
	Fog,
	Fba,
	ScanMask,
	AlphaTest, ///< ATE with an effective test (anything but ALWAYS)
	DateTest, ///< DATE on a format with real destination alpha
	ZTestNever, ///< draw can write nothing — let the floor no-op it
	FrameMaskPartial, ///< FBMSK masks part of a byte — needs the fbmask shader
	NothingWritten, ///< every channel masked and no z write
	FramePsm, ///< frame format outside CT32/CT24
	ZbufPsm, ///< depth format that cannot round-trip through float32 (Z32)
	StrideZero, ///< FBW=0 — degenerate stride
	// Geometry reasons (decided by the route, which knows the footprints):
	FootprintExtent, ///< draw wider than the stride or deep enough to wrap page space
	FrameZOverlap, ///< FRAME and ZBUF footprints share pages
	ResourceFailure, ///< target pool allocation failed
	Count
};

struct GSTileDrawInput
{
	u32 prim_class; ///< GS_PRIM_CLASS
	GIFRegTEST TEST;
	GIFRegFRAME FRAME;
	GIFRegZBUF ZBUF;
	u8 scanmsk; ///< SCANMSK.MSK
	// Source alpha range of the draw (the vertex-trace fact; exact for untextured
	// draws, which are the only ones that reach the alpha-test logic — tme floors
	// first). Callers without the fact pass the full 0..255 range.
	u8 alpha_min;
	u8 alpha_max;
	bool tme;
	bool abe;
	bool aa1;
	bool fge;
	bool fba;
	bool vs_expand; ///< device supports VS sprite expansion
};

struct GSTileDrawPlan
{
	bool native = false;
	GSTileFloorReason reason = GSTileFloorReason::None;

	// Effective write facts. Valid whenever the register combination is coherent,
	// so the floor path may consult them too.
	u8 colormask = 0; ///< wrgba after whole-byte FBMSK exemptions
	u8 fb_claims = 0; ///< planes a native draw claims on FRAME pages (byte coverage)
	u8 z_claims = 0; ///< planes claimed on ZBUF pages when z_write
	bool z_write = false;
	bool z_test = false; ///< fixed-function test does something (ZTST > ALWAYS)
	u8 ztst = ZTST_ALWAYS;
};

inline GSTileDrawPlan gsTileLowerDraw(const GSTileDrawInput& in)
{
	GSTileDrawPlan p;

	// SW-floor z semantics: ZTE=0 turns off the test AND the write.
	p.z_write = in.TEST.ZTE && !in.ZBUF.ZMSK;
	p.z_test = in.TEST.ZTE && in.TEST.ZTST > ZTST_ALWAYS;
	p.ztst = static_cast<u8>(in.TEST.ZTE ? in.TEST.ZTST : static_cast<u32>(ZTST_ALWAYS));

	// Whole-byte FBMSK exemptions; a partially masked byte disqualifies below.
	const u32 fbmsk = in.FRAME.FBMSK;
	bool partial_byte = false;
	u8 cm = 0;
	for (u32 b = 0; b < 4; b++)
	{
		const u32 byte = (fbmsk >> (b * 8)) & 0xFFu;
		if (byte == 0)
			cm |= static_cast<u8>(1u << b);
		else if (byte != 0xFFu)
			partial_byte = true;
	}
	if (in.FRAME.PSM == PSMCT24)
		cm &= 0x7; // no alpha byte in storage

	// An alpha test with a PROVABLE outcome is not a test. Provably passing, it
	// disappears; provably failing, AFAIL is a deterministic write-mask edit (the
	// GS idiom "ATST=NEVER, AFAIL=FB_ONLY" is how games write color without depth).
	// The z TEST is untouched either way — AFAIL suppresses only the z write, and
	// the surviving writes are still depth-gated (the SW scanline's fm/zm shape).
	// Only a genuinely dynamic test floors.
	bool atst_dynamic = false;
	if (in.TEST.ATE && in.TEST.ATST != ATST_ALWAYS)
	{
		enum class Outcome
		{
			Pass,
			Fail,
			Dynamic
		};
		Outcome outcome = Outcome::Dynamic;
		const u32 aref = in.TEST.AREF;
		const u32 lo = in.alpha_min;
		const u32 hi = in.alpha_max;
		switch (in.TEST.ATST)
		{
			case ATST_NEVER:
				outcome = Outcome::Fail;
				break;
			case ATST_LESS:
				outcome = (hi < aref) ? Outcome::Pass : ((lo >= aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_LEQUAL:
				outcome = (hi <= aref) ? Outcome::Pass : ((lo > aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_EQUAL:
				outcome = (lo == aref && hi == aref) ? Outcome::Pass :
						  ((aref < lo || aref > hi) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_GEQUAL:
				outcome = (lo >= aref) ? Outcome::Pass : ((hi < aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_GREATER:
				outcome = (lo > aref) ? Outcome::Pass : ((hi <= aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_NOTEQUAL:
				outcome = (aref < lo || aref > hi) ? Outcome::Pass :
						  ((lo == aref && hi == aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			default:
				break;
		}
		if (outcome == Outcome::Fail)
		{
			switch (in.TEST.GetAFAIL(in.FRAME.PSM))
			{
				case AFAIL_KEEP:
					cm = 0;
					p.z_write = false; // nothing written at all -> NothingWritten below
					break;
				case AFAIL_FB_ONLY:
					p.z_write = false;
					break;
				case AFAIL_ZB_ONLY:
					cm = 0;
					break;
				case AFAIL_RGB_ONLY:
					cm &= 0x7;
					p.z_write = false;
					break;
			}
		}
		else if (outcome == Outcome::Dynamic)
		{
			atst_dynamic = true;
		}
	}

	p.colormask = cm;
	u8 fb_claims = 0;
	if (cm & 0x7)
		fb_claims |= GSTilePlaneRGB;
	if (cm & 0x8)
		fb_claims |= kGSTilePlanesAlpha;
	if (fb_claims != 0)
		fb_claims |= GSTilePlaneZ; // byte coverage: written bytes invalidate any depth reading
	p.fb_claims = fb_claims;
	p.z_claims = p.z_write ? gsTilePlanesInvalidatedByWrite(in.ZBUF.PSM) : 0;

	const auto floored = [&p](GSTileFloorReason r) {
		p.native = false;
		p.reason = r;
		return p;
	};

	if (in.prim_class != GS_TRIANGLE_CLASS && in.prim_class != GS_SPRITE_CLASS)
		return floored(GSTileFloorReason::PrimClass);
	if (in.prim_class == GS_SPRITE_CLASS && !in.vs_expand)
		return floored(GSTileFloorReason::SpriteExpandUnavailable);
	if (in.tme)
		return floored(GSTileFloorReason::Textured);
	if (in.abe)
		return floored(GSTileFloorReason::Blend);
	if (in.aa1 && in.prim_class == GS_TRIANGLE_CLASS)
		return floored(GSTileFloorReason::CoverageAA1);
	if (in.fge)
		return floored(GSTileFloorReason::Fog);
	if (in.fba)
		return floored(GSTileFloorReason::Fba);
	if (in.scanmsk != 0)
		return floored(GSTileFloorReason::ScanMask);
	if (atst_dynamic)
		return floored(GSTileFloorReason::AlphaTest);
	// The GS has no destination alpha on a 24-bit frame; both existing renderers
	// treat DATE there as disabled, so it stays native.
	if (in.TEST.DATE && in.FRAME.PSM != PSMCT24)
		return floored(GSTileFloorReason::DateTest);
	if (in.TEST.ZTE && in.TEST.ZTST == ZTST_NEVER)
		return floored(GSTileFloorReason::ZTestNever);
	if (partial_byte)
		return floored(GSTileFloorReason::FrameMaskPartial);
	if (p.colormask == 0 && !p.z_write)
		return floored(GSTileFloorReason::NothingWritten);
	if (p.colormask != 0 && in.FRAME.PSM != PSMCT32 && in.FRAME.PSM != PSMCT24)
		return floored(GSTileFloorReason::FramePsm);
	if ((p.z_write || p.z_test) &&
		in.ZBUF.PSM != PSMZ24 && in.ZBUF.PSM != PSMZ16 && in.ZBUF.PSM != PSMZ16S)
		return floored(GSTileFloorReason::ZbufPsm);
	if (in.FRAME.FBW == 0)
		return floored(GSTileFloorReason::StrideZero);

	p.native = true;
	return p;
}
