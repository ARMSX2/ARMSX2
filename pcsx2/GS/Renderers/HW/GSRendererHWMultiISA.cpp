// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSRendererHW.h"
#include "GS/Renderers/SW/GSLevelOfDetail.h"

#include "GS/Renderers/Common/GSSwPrimRender.h"
#include "GS/Renderers/SW/GSTextureCacheSW.h"
#include "GS/Renderers/SW/GSRasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

class CURRENT_ISA::GSRendererHWFunctions
{
public:
	static bool SwPrimRender(GSRendererHW& hw, bool invalidate_tc, bool add_ee_transfer);

	static void Populate(GSRendererHW& renderer)
	{
		renderer.SwPrimRender = SwPrimRender;
	}
};

// The scanline setup itself, which reads nothing a GSRenderer does not have. It lives here rather
// than in a file of its own because this is already the translation unit that may name
// GSSingleRasterizer -- it is compiled once per ISA, and the rasterizer's declaration is scoped to
// exactly that.
class CURRENT_ISA::GSSwPrimRenderFunctions
{
public:
	static bool Run(GSRenderer& renderer, GSSwPrimRenderState& sw, const GSVector4i& bbox);

private:
	static void PrepareTexture(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data);
	static void SetupMipLod(GSRenderer& hw, GSScanlineGlobalData& gd);
	static bool IsPaletteBlockCopy(const GSRasterizerData& data, bool uv, bool texel32 = false);
	static bool IsMaskedTexelCopy(const GSRasterizerData& data, bool uv, bool mip);
	static bool OnUVGrid(const GSRasterizerData& data);
	template <bool minify = false>
	static bool CollectSprites(GSSwPrimRenderState& sw, const GSRasterizerData& data, int lod = 0, bool divide_q = false);
	static bool DrawPaletteBlocks(GSSwPrimRenderState& sw, const GSRasterizerData& data);
	static bool DrawTexels32(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data);
	static bool DrawMaskedTexels(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data, bool mip);
};

MULTI_ISA_UNSHARED_IMPL;

void CURRENT_ISA::GSRendererHWPopulateFunctions(GSRendererHW& renderer)
{
	GSRendererHWFunctions::Populate(renderer);
}

bool CURRENT_ISA::GSSwPrimRenderRun(GSRenderer& renderer, GSSwPrimRenderState& sw, const GSVector4i& bbox)
{
	return GSSwPrimRenderFunctions::Run(renderer, sw, bbox);
}

// since there's no overlapping draws, we can just keep this intact
static GSVector4i s_dimx_storage[8];
static GIFRegDIMX s_last_dimx;

bool GSRendererHWFunctions::SwPrimRender(GSRendererHW& hw, bool invalidate_tc, bool add_ee_transfer)
{
	const GSVector4i bbox = GSSwPrimRenderBBox(hw.m_vt, hw.m_context->scissor.in);
	if (!GSSwPrimRenderFunctions::Run(hw, hw.m_sw_prim, bbox))
		return false;

	if (invalidate_tc)
	{
		const GSDrawingContext* context = hw.m_context;
		GSOffset frame_offs = context->offset.fb;

		if (GSLocalMemory::m_psm[context->FRAME.PSM].trbpp == 32 && context->FRAME.FBMSK)
		{
			if (context->FRAME.FBMSK == 0xFF000000)
				frame_offs = GSRendererHW::GetInstance()->m_mem.GetOffset(context->FRAME.Block(), context->FRAME.FBW, PSMCT24);
			else if (context->FRAME.FBMSK == 0x00FFFFFF)
				frame_offs = GSRendererHW::GetInstance()->m_mem.GetOffset(context->FRAME.Block(), context->FRAME.FBW, PSMT8H);
		}

		g_texture_cache->InvalidateVideoMem(frame_offs, bbox);
	}

	// Jak does sw prim render, then draws to the same target, and it needs to be uploaded.
	if (add_ee_transfer)
	{
		GSRendererHW::GSUploadQueue uq;
		uq.transfer_type = GSRendererHW::GetInstance()->EEGS_TransferType::EE_to_GS;
		uq.blit.U64 = 0;
		uq.blit.DBP = hw.m_cached_ctx.FRAME.Block();
		uq.blit.DBW = hw.m_cached_ctx.FRAME.FBW;
		uq.blit.DPSM = hw.m_cached_ctx.FRAME.PSM;
		uq.draw = hw.s_n;
		uq.rect = bbox;
		hw.m_draw_transfers.push_back(uq);
	}

	return true;
}

// Mark the pages a draw through the software scanline core can have written: the frame and the depth buffer,
// over the bounding box. Unconditional, whatever the masks and tests are (the solid sprite fill writes the
// frame on a partial mask without fwrite). One pixel more on every side: the box is the floor and ceiling of the
// vertex extent, and nothing shows that it holds every AA1 edge pixel.
static void MarkDrawWritten(GSLocalMemory& mem, const GSDrawingContext* context, const GSVector4i& bbox)
{
	const GSVector4i grown(std::max(bbox.x - 1, 0), std::max(bbox.y - 1, 0), bbox.z + 1, bbox.w + 1);
	mem.MarkPagesWritten(context->offset.fb, grown);
	mem.MarkPagesWritten(context->offset.zb, grown);
}

// The clamp and wrap limits of a texture, in the form the scanline reads them.
static void SetupTextureWrap(GSScanlineGlobalData& gd, const GSDrawingContext* context, const GIFRegTEX0& TEX0)
{
	u16 tw = 1u << TEX0.TW;
	u16 th = 1u << TEX0.TH;

	if (tw > 1024)
		tw = 1;

	if (th > 1024)
		th = 1;

	switch (context->CLAMP.WMS)
	{
		case CLAMP_REPEAT:
			gd.t.min.U16[0] = gd.t.minmax.U16[0] = tw - 1;
			gd.t.max.U16[0] = gd.t.minmax.U16[2] = 0;
			gd.t.mask.U32[0] = 0xffffffff;
			break;
		case CLAMP_CLAMP:
			gd.t.min.U16[0] = gd.t.minmax.U16[0] = 0;
			gd.t.max.U16[0] = gd.t.minmax.U16[2] = tw - 1;
			gd.t.mask.U32[0] = 0;
			break;
		case CLAMP_REGION_CLAMP:
			// REGION_CLAMP ignores the actual texture size
			gd.t.min.U16[0] = gd.t.minmax.U16[0] = context->CLAMP.MINU;
			gd.t.max.U16[0] = gd.t.minmax.U16[2] = context->CLAMP.MAXU;
			gd.t.mask.U32[0] = 0;
			break;
		case CLAMP_REGION_REPEAT:
			// MINU is restricted to MINU or texture size, whichever is smaller, MAXU is an offset in the texture.
			gd.t.min.U16[0] = gd.t.minmax.U16[0] = context->CLAMP.MINU & (tw - 1);
			gd.t.max.U16[0] = gd.t.minmax.U16[2] = context->CLAMP.MAXU;
			gd.t.mask.U32[0] = 0xffffffff;
			break;
		default:
			ASSUME(0);
	}

	switch (context->CLAMP.WMT)
	{
		case CLAMP_REPEAT:
			gd.t.min.U16[4] = gd.t.minmax.U16[1] = th - 1;
			gd.t.max.U16[4] = gd.t.minmax.U16[3] = 0;
			gd.t.mask.U32[2] = 0xffffffff;
			break;
		case CLAMP_CLAMP:
			gd.t.min.U16[4] = gd.t.minmax.U16[1] = 0;
			gd.t.max.U16[4] = gd.t.minmax.U16[3] = th - 1;
			gd.t.mask.U32[2] = 0;
			break;
		case CLAMP_REGION_CLAMP:
			// REGION_CLAMP ignores the actual texture size
			gd.t.min.U16[4] = gd.t.minmax.U16[1] = context->CLAMP.MINV;
			gd.t.max.U16[4] = gd.t.minmax.U16[3] = context->CLAMP.MAXV; // ffx anima summon scene, when the anchor appears (th = 256, maxv > 256)
			gd.t.mask.U32[2] = 0;
			break;
		case CLAMP_REGION_REPEAT:
			// MINV is restricted to MINV or texture size, whichever is smaller, MAXV is an offset in the texture.
			gd.t.min.U16[4] = gd.t.minmax.U16[1] = context->CLAMP.MINV & (th - 1); // skygunner main menu water texture 64x64, MINV = 127
			gd.t.max.U16[4] = gd.t.minmax.U16[3] = context->CLAMP.MAXV;
			gd.t.mask.U32[2] = 0xffffffff;
			break;
		default:
			ASSUME(0);
	}

	gd.t.min = gd.t.min.xxxxlh();
	gd.t.max = gd.t.max.xxxxlh();
	gd.t.mask = gd.t.mask.xxzz();
	gd.t.invmask = ~gd.t.mask;
}

// The level of detail of a mipmapped draw: the filter and level-of-detail bits of the selector, and the constant
// level when the draw has one (gd.lod). The snapshot of the mip levels follows in PrepareTexture(); a copy that
// reads the one level itself needs only this. Setting the same fields again changes nothing, so a copy that
// declines the draw leaves PrepareTexture() the same work.
void GSSwPrimRenderFunctions::SetupMipLod(GSRenderer& hw, GSScanlineGlobalData& gd)
{
	GSVertexTrace& vt = hw.m_vt;
	const GSDrawingContext* context = hw.m_context;

	// TEX1.MMIN
	// 000 p
	// 001 l
	// 010 p round
	// 011 p tri
	// 100 l round
	// 101 l tri

	if (vt.m_lod.x > 0)
	{
		gd.sel.ltf = context->TEX1.MMIN >> 2;
	}
	else
	{
		// TODO: isbilinear(mmag) != isbilinear(mmin) && vt.m_lod.x <= 0 && vt.m_lod.y > 0
	}

	gd.sel.mmin = (context->TEX1.MMIN & 1) + 1; // 1: round, 2: tri
	gd.sel.lcm = context->TEX1.LCM;

	int mxl = std::min<int>((int)context->TEX1.MXL, 6) << 16;
	int k = context->TEX1.K << 12;

	if ((int)vt.m_lod.x >= (int)context->TEX1.MXL)
	{
		k = (int)vt.m_lod.x << 16; // set lod to max level

		gd.sel.lcm = 1; // lod is constant
		gd.sel.mmin = 1; // tri-linear is meaningless
	}

	if (gd.sel.fst)
	{
		pxAssert(gd.sel.lcm == 1);
		//pxAssert(((vt.m_min.t.uph(vt.m_max.t) == GSVector4::zero()).mask() & 3) == 3); // ratchet and clank (menu)

		gd.sel.lcm = 1;
	}

	if (gd.sel.lcm)
	{
		int lod = std::max<int>(std::min<int>(k, mxl), 0);

		if (gd.sel.mmin == 1)
		{
			lod = (lod + 0x8000) & 0xffff0000; // rounding
		}

		gd.lod.i = GSVector4i(lod >> 16);
		gd.lod.f = GSVector4i(lod & 0xffff).xxxxl().xxzz();

		// TODO: lot to optimize when lod is constant
	}
	else
	{
		gd.mxl = GSVector4((float)mxl);
		gd.l = GSVector4((float)(-(0x10000 << context->TEX1.L)));
		gd.k = GSVector4((float)k);

		// The level of detail is a table read on Q's mantissa; see
		// GSLevelOfDetail.h. This path builds the same global data the
		// software renderer does and runs the same scanline, so it sets
		// the same four fields -- a scanline reading an unset `lodtab`
		// would be following a wild pointer per pixel.
		gd.lodtab = GSLevelOfDetailTable[context->TEX1.L];
		gd.lodk = context->TEX1.K;
		gd.lodshift = 4 + context->TEX1.L;
		gd.lodmxl = mxl;
	}
}

// The texture half of the scanline setup: the snapshot the scanline reads texels from (and its mip levels), the
// selector bits that follow from it, and the wrap limits. Run() calls it once every other selector bit is known.
void GSSwPrimRenderFunctions::PrepareTexture(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data)
{
	GSVertexTrace& vt = hw.m_vt;
	const GSDrawingContext* context = hw.m_context;
	const GSDrawingEnvironment& env = *hw.m_draw_env;
	const GS_PRIM_CLASS primclass = vt.m_primclass;
	GSScanlineGlobalData& gd = data.global;

	// GetTextureMinMax() reads whether the sprites tile the draw; nothing here reads whether
	// their union covers it.
	hw.CalculatePrimitiveCoversWithoutGaps(false);

	bool mipmap = hw.IsMipMapActive();

	GIFRegTEX0 TEX0 = context->GetSizeFixedTEX0(vt.m_min.t.xyxy(vt.m_max.t), vt.IsLinear(), mipmap);

	const GSVector4i r = hw.GetTextureMinMax(TEX0, context->CLAMP, gd.sel.ltf, true).coverage;

	if (!sw.texture[0])
		sw.texture[0] = std::make_unique<GSTextureCacheSW::Texture>(0, TEX0, env.TEXA);
	else
		sw.texture[0]->Reset(0, TEX0, env.TEXA);

	sw.texture[0]->Update(r);
	gd.tex[0] = sw.texture[0]->m_buff;

	gd.sel.tw = sw.texture[0]->m_tw - 3;

	if (mipmap)
	{
		SetupMipLod(hw, gd);

		GIFRegCLAMP MIP_CLAMP = context->CLAMP;

		GSVector4 tmin = vt.m_min.t;
		GSVector4 tmax = vt.m_max.t;

		size_t levels = 1;

		for (int i = 1, j = std::min<int>((int)context->TEX1.MXL, 6); i <= j; i++)
		{
			const GIFRegTEX0& MIP_TEX0 = hw.GetTex0Layer(i);

			MIP_CLAMP.MINU >>= 1;
			MIP_CLAMP.MINV >>= 1;
			MIP_CLAMP.MAXU >>= 1;
			MIP_CLAMP.MAXV >>= 1;

			vt.m_min.t *= 0.5f;
			vt.m_max.t *= 0.5f;

			if (!sw.texture[i])
				sw.texture[i] = std::make_unique<GSTextureCacheSW::Texture>(gd.sel.tw + 3, MIP_TEX0, env.TEXA);
			else
				sw.texture[i]->Reset(gd.sel.tw + 3, MIP_TEX0, env.TEXA);

			GSVector4i r = hw.GetTextureMinMax(MIP_TEX0, MIP_CLAMP, gd.sel.ltf, true).coverage;
			sw.texture[i]->Update(r);
			gd.tex[i] = sw.texture[i]->m_buff;
			levels = i + 1;
		}

		// The dummy level the trilinear ceiling reads; see the software
		// renderer's UpdateSource and GSScanlineEnvironment.h.
		gd.tex[levels] = gd.tex[levels - 1];

		vt.m_min.t = tmin;
		vt.m_max.t = tmax;
	}
	else
	{
		// skip per pixel division if q is constant. Sprite uses flat
		// q, so it's always constant by primitive.
		// Note: the 'q' division was done in GSRendererSW::ConvertVertexBuffer
		gd.sel.fst |= (vt.m_eq.q || primclass == GS_SPRITE_CLASS);

		if (gd.sel.ltf && gd.sel.fst)
		{
			// if q is constant we can do the half pel shift for bilinear sampling on the vertices

			// TODO: but not when mipmapping is used!!!

			const GSVector4 half(0x8000, 0x8000);

			GSVertexSW* RESTRICT v = data.vertex;
			for (int i = 0, j = data.vertex_count; i < j; i++)
			{
				const GSVector4 t = v[i].t;
				v[i].t = (t - half).xyzw(t);
			}
		}
	}

	SetupTextureWrap(gd, context, TEX0);
}

bool GSSwPrimRenderFunctions::Run(GSRenderer& hw, GSSwPrimRenderState& sw, const GSVector4i& bbox)
{
	GSVertexTrace& vt = hw.m_vt;
	const GIFRegPRIM* PRIM = hw.PRIM;
	const GSDrawingContext* context = hw.m_context;
	const GSDrawingEnvironment& env = *hw.m_draw_env;
	const GS_PRIM_CLASS primclass = vt.m_primclass;

	GSRasterizerData data;
	GSScanlineGlobalData& gd = data.global;

	sw.vertex_buffer.resize(((hw.m_vertex->next + 1) & ~1));

	data.primclass = vt.m_primclass;
	data.buff = nullptr;
	data.vertex = sw.vertex_buffer.data();
	data.vertex_count = hw.m_vertex->next;
	data.index = hw.m_index->buff;
	data.index_count = hw.m_index->tail;
	data.scanmsk_value = env.SCANMSK.MSK;

	// Skip per pixel division if q is constant.
	// Optimize the division by 1 with a nop. It also means that GS_SPRITE_CLASS must be processed when !vt.m_eq.q.
	// If you have both GS_SPRITE_CLASS && vt.m_eq.q, it will depends on the first part of the 'OR'.
	const u32 q_div = !hw.IsMipMapActive() && ((vt.m_eq.q && vt.m_min.t.z != 1.0f) || (!vt.m_eq.q && vt.m_primclass == GS_SPRITE_CLASS));
	GSVertexSW::s_cvb[vt.m_primclass][PRIM->TME][PRIM->FST][q_div](context, data.vertex, hw.m_vertex->buff, hw.m_vertex->next);

	data.scissor = context->scissor.in;
	data.bbox = bbox;
	data.frame = g_perfmon.GetFrame();

	gd.vm = hw.m_mem.m_vm8;

	gd.fbo = context->offset.fb;
	gd.zbo = context->offset.zb;
	gd.fzbr = context->offset.fzb4->row;
	gd.fzbc = context->offset.fzb4->col;

	gd.sel.key = 0;

	gd.sel.fpsm = 3;
	gd.sel.zpsm = 3;
	gd.sel.atst = ATST_ALWAYS;
	gd.sel.tfx = TFX_NONE;
	gd.sel.ababcd = 0xff;
	gd.sel.prim = primclass;

	u32 fm = context->FRAME.FBMSK;
	u32 zm = context->ZBUF.ZMSK || context->TEST.ZTE == 0 ? 0xffffffff : 0;
	const u32 fm_mask = GSLocalMemory::m_psm[context->FRAME.PSM].fmsk;

	// When the format is 24bit (Z or C), DATE ceases to function.
	// It was believed that in 24bit mode all pixels pass because alpha doesn't exist
	// however after testing this on a PS2 it turns out nothing passes, it ignores the draw.
	if ((context->FRAME.PSM & 0xF) == PSMCT24 && context->TEST.DATE)
	{
		//DevCon.Warning("DATE on a 24bit format, Frame PSM %x", context->FRAME.PSM);
		return false;
	}

	if (context->TEST.ZTE && context->TEST.ZTST == ZTST_NEVER)
	{
		fm = 0xffffffff;
		zm = 0xffffffff;
	}

	if (PRIM->TME)
	{
		if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
		{
			hw.m_mem.m_clut.Read32(context->TEX0, env.TEXA);
		}
	}

	if (context->TEST.ATE)
	{
		if (!hw.TryAlphaTest(fm, zm))
		{
			gd.sel.atst = context->TEST.ATST;
			gd.sel.afail = context->TEST.GetAFAIL(context->FRAME.PSM);

			gd.aref = GSVector4i((int)context->TEST.AREF);

			switch (gd.sel.atst)
			{
			case ATST_LESS:
				gd.sel.atst = ATST_LEQUAL;
				gd.aref -= GSVector4i::x00000001();
				break;
			case ATST_GREATER:
				gd.sel.atst = ATST_GEQUAL;
				gd.aref += GSVector4i::x00000001();
				break;
			}
		}
	}

	const bool fwrite = (fm & fm_mask) != fm_mask;
	const bool ftest = gd.sel.atst != ATST_ALWAYS || (context->TEST.DATE && context->FRAME.PSM != PSMCT24);

	const bool zwrite = zm != 0xffffffff;
	const bool ztest = context->TEST.ZTE && context->TEST.ZTST > ZTST_ALWAYS;
	if (!fwrite && !zwrite)
		return false;

	gd.sel.fwrite = fwrite;
	gd.sel.ftest = ftest;

	// The texture snapshot and the wrap limits are built after the selector is complete (PrepareTexture), so a
	// draw a copy takes never pays for the snapshot.
	bool prepare_texture = false;

	if (fwrite || ftest)
	{
		gd.sel.fpsm = GSLocalMemory::m_psm[context->FRAME.PSM].fmt;

		if ((primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && vt.m_eq.rgba != 0xffff)
		{
			gd.sel.iip = PRIM->IIP;
		}

		if (PRIM->TME)
		{
			gd.sel.tfx = context->TEX0.TFX;
			gd.sel.tcc = context->TEX0.TCC;
			gd.sel.fst = PRIM->FST;
			gd.sel.ltf = vt.IsLinear();

			if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
			{
				gd.sel.tlu = 1;

				gd.clut = const_cast<u32*>(static_cast<const u32*>(hw.m_mem.m_clut));
			}

			gd.sel.wms = context->CLAMP.WMS;
			gd.sel.wmt = context->CLAMP.WMT;

			// Modulate by a vertex colour of 0x80 returns the texel unchanged, so it is decal. Without
			// TCC both take alpha from the vertex, so only the colour channels need to be 0x80.
			const u32 modulated = gd.sel.tcc ? 0xffff : 0x0fff;
			if (gd.sel.tfx == TFX_MODULATE && vt.m_eq.rgba == 0xffff && ((vt.m_min.c == GSVector4i(128)).mask() & modulated) == modulated)
			{
				gd.sel.tfx = TFX_DECAL;
			}

			prepare_texture = true;
		}

		if (PRIM->FGE)
		{
			gd.sel.fge = 1;

			gd.frb = env.FOGCOL.U32[0] & 0x00ff00ff;
			gd.fga = (env.FOGCOL.U32[0] >> 8) & 0x00ff00ff;
		}

		if (context->FRAME.PSM != PSMCT24)
		{
			gd.sel.date = context->TEST.DATE;
			gd.sel.datm = context->TEST.DATM;
		}

		if (!hw.IsOpaque())
		{
			gd.sel.abe = PRIM->ABE;
			gd.sel.ababcd = context->ALPHA.U32[0];

			if (env.PABE.PABE)
			{
				gd.sel.pabe = 1;
			}

			if (PRIM->AA1 && (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS))
			{
				gd.sel.aa1 = 1;
			}

			gd.afix = GSVector4i((int)context->ALPHA.FIX << 7).xxzzlh();
		}

		const u32 masked_fm = fm & fm_mask;
		if (gd.sel.date
			|| gd.sel.aba == 1 || gd.sel.abb == 1 || gd.sel.abc == 1 || gd.sel.abd == 1
			|| (gd.sel.atst != ATST_ALWAYS && gd.sel.afail == AFAIL_RGB_ONLY)
			|| (gd.sel.fpsm == 0 && masked_fm != 0 && masked_fm != fm_mask)
			|| (gd.sel.fpsm == 1 && masked_fm != 0 && masked_fm != fm_mask)
			|| (gd.sel.fpsm == 2 && masked_fm != 0 && masked_fm != fm_mask))
		{
			gd.sel.rfb = 1;
		}

		gd.sel.colclamp = env.COLCLAMP.CLAMP;
		gd.sel.fba = context->FBA.FBA;

		if (env.DTHE.DTHE)
		{
			gd.sel.dthe = 1;
			gd.dimx = s_dimx_storage;
			if (s_last_dimx != env.DIMX)
			{
				s_last_dimx = env.DIMX;
				GSState::ExpandDIMX(s_dimx_storage, env.DIMX);
			}
		}
	}

	gd.sel.zwrite = zwrite;
	gd.sel.ztest = ztest;

	if (zwrite || ztest)
	{
		const u32 z_max = 0xffffffff >> (GSLocalMemory::m_psm[context->ZBUF.PSM].fmt * 8);

		gd.sel.zpsm = GSLocalMemory::m_psm[context->ZBUF.PSM].fmt;
		gd.sel.ztst = ztest ? context->TEST.ZTST : (int)ZTST_ALWAYS;
		gd.sel.zequal = !!vt.m_eq.z;
		gd.sel.zoverflow = (u32)GSVector4i(vt.m_max.p).z == 0x80000000U;
		gd.sel.zclamp = (u32)GSVector4i(vt.m_max.p).z > z_max;
	}

#if _M_SSE >= 0x501

	gd.fm = fm;
	gd.zm = zm;

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= 0xff000000;
	}
	else if (gd.sel.fpsm == 2)
	{
		u32 rb = gd.fm & 0x00f800f8;
		u32 ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | 0xffff0000;
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= 0xff000000;
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= 0xffff0000;
	}

#else

	gd.fm = GSVector4i(fm);
	gd.zm = GSVector4i(zm);

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= GSVector4i::xff000000();
	}
	else if (gd.sel.fpsm == 2)
	{
		GSVector4i rb = gd.fm & 0x00f800f8;
		GSVector4i ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | GSVector4i::xffff0000();
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= GSVector4i::xff000000();
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= GSVector4i::xffff0000();
	}

#endif

	if (gd.sel.prim == GS_SPRITE_CLASS && !gd.sel.ftest && !gd.sel.ztest && data.bbox.eq(data.bbox.rintersect(data.scissor))) // TODO: check scissor horizontally only
	{
		gd.sel.notest = 1;

		const u32 ofx = context->XYOFFSET.OFX;

		for (int i = 0, j = hw.m_vertex->tail; i < j; i++)
		{
#if _M_SSE >= 0x501
			if ((((hw.m_vertex->buff[i].XYZ.X - ofx) + 15) >> 4) & 7) // aligned to 8
#else
			if ((((hw.m_vertex->buff[i].XYZ.X - ofx) + 15) >> 4) & 3) // aligned to 4
#endif
			{
				gd.sel.notest = 0;

				break;
			}
		}
	}

	// A paletted sprite draw into a 16-bit frame is copied straight from local memory, before the snapshot of the
	// texture that the rasterizer (and the palette block copy below) would take. A draw into any other frame cannot
	// be one, which is settled before the more thorough check so that it costs those draws one compare.
	if (prepare_texture && sw.palette_block_copy && gd.sel.fpsm == 2)
	{
		const bool mip = hw.IsMipMapActive();
		if (mip)
			SetupMipLod(hw, gd);

		if (IsMaskedTexelCopy(data, PRIM->FST, mip) && DrawMaskedTexels(hw, sw, data, mip))
		{
			g_perfmon.Put(GSPerfMon::SwPaletteBlockCopies, 1);
			MarkDrawWritten(hw.m_mem, context, bbox);
			return true;
		}
	}

	// A 32-bit texture sprite draw into a 32-bit frame is copied the same way, texel for texel, with no palette. It
	// is also settled before the snapshot, so a draw it takes never converts the blocks of its texture.
	if (prepare_texture && sw.palette_block_copy && gd.sel.fpsm == 0 && context->TEX0.PSM == PSMCT32 && !hw.IsMipMapActive())
	{
		if (IsPaletteBlockCopy(data, PRIM->FST, true) && DrawTexels32(hw, sw, data))
		{
			g_perfmon.Put(GSPerfMon::SwPaletteBlockCopies, 1);
			MarkDrawWritten(hw.m_mem, context, bbox);
			return true;
		}
	}

	if (prepare_texture)
		PrepareTexture(hw, sw, data);

	if (sw.palette_block_copy && IsPaletteBlockCopy(data, PRIM->FST) && DrawPaletteBlocks(sw, data))
	{
		g_perfmon.Put(GSPerfMon::SwPaletteBlockCopies, 1);
		MarkDrawWritten(hw.m_mem, context, bbox);
		return true;
	}

	if (!sw.rasterizer)
		sw.rasterizer = std::make_unique<GSSingleRasterizer>();

	static_cast<GSSingleRasterizer*>(sw.rasterizer.get())->Draw(data);
	MarkDrawWritten(hw.m_mem, context, bbox);

	return true;
}

// ST coordinates reach the scanline as a 16.16 value computed from S, T and Q, so in general they
// are not held to sixteenths of a texel as UV is. A draw whose converted coordinates all happen to
// land on that grid, inside the range a UV can encode, is indistinguishable from the UV draw from
// here on: the selector is the same (a sprite's fst is set whenever Q is constant), and the
// rasterizer and the copy read the same converted vertices.
bool GSSwPrimRenderFunctions::OnUVGrid(const GSRasterizerData& data)
{
	constexpr float sixteenth = 4096.0f;
	constexpr float limit = 16384.0f * sixteenth; // UV is a 14-bit count of sixteenths

	for (int i = 0; i < data.vertex_count; i++)
	{
		const GSVector4 t = data.vertex[i].t;
		for (int c = 0; c < 2; c++)
		{
			const float x = t.F32[c];
			if (!(x >= 0.0f && x < limit) || (x / sixteenth) != std::floor(x / sixteenth))
				return false;
		}
	}

	return true;
}

// The palette block copy: a sprite draw that writes each texel's palette entry one-to-one into a
// 32-bit frame. Games that expand paletted textures on the GS draw thousands of these a frame, and
// the rasterizer spends most of its time on each in setup that this shape does not need.
//
// It runs instead of the rasterizer, after everything Run() sets up, and must write exactly the
// bytes the scanline would. So it is decided on the scanline's own selector rather than on the GS
// registers: a draw is taken only if the selector the rasterizer would compile is this one shape,
// up to fields that cannot change a pixel of it. It reads texels from the same snapshot the
// scanline reads (gd.tex[0], taken by Update() before any pixel is written, so a draw that
// overwrites its own texture reads what it read before), through the same palette (gd.clut),
// wraps with the same limits (gd.t) and addresses the frame through the same tables
// (gd.fzbr/fzbc).
//
// With `texel32` the same decision is made for the 32-bit texel copy (DrawTexels32()): the texture is a
// 32-bit one with no palette, and the draw is judged before the snapshot is taken, so the selector's
// texture half is filled in below the way PrepareTexture() would, as IsMaskedTexelCopy() does. Every
// other field is held to the same shape: the texel goes to the frame unchanged, so everything the palette
// entry path refuses is refused here for the same reason.
bool GSSwPrimRenderFunctions::IsPaletteBlockCopy(const GSRasterizerData& data, bool uv, bool texel32)
{
	const GSScanlineGlobalData& gd = data.global;
	GSScanlineSelector sel = gd.sel;

	// Plain repeat or clamp on each axis. The region modes can address past the texture's rows.
	if (sel.wms > CLAMP_CLAMP || sel.wmt > CLAMP_CLAMP)
		return false;

	// Fields that cannot change what this shape writes: the wrap is read from gd.t, the texture
	// pitch from sel.tw; an 8-bit texel colour passes the colour clamp or the wrap mask unchanged;
	// datm is read only with date; notest picks how the scanline handles span edges, not which
	// pixels it writes.
	sel.wms = 0;
	sel.wmt = 0;
	sel.tw = 0;
	sel.colclamp = 0;
	sel.datm = 0;
	sel.notest = 0;

	// Without TCC the alpha written is the vertex's own (DrawPaletteBlocks() writes it); the colour
	// channels are still the palette entry.
	sel.tcc = 1;

	// PrepareTexture() sets fst for a sprite whatever its coordinates came as (the ST case is held to the
	// sixteenth grid by `uv`) and the texture pitch, which reads nothing here.
	if (texel32)
	{
		sel.fst = 1;

		// A bilinear lookup is taken only where its weights are all zero (a texel a pixel) or all half (two texels a
		// pixel, the box filter); DrawTexels32() holds each sprite to one of the two.
		sel.ltf = 0;
	}

	// Everything else must be exactly this: a textured sprite with a nearest, non-mipmapped UV
	// lookup through the palette (or, for texel32, with no palette), decal (the output is the palette entry, with the vertex alpha in
	// place of the entry's when TCC is off), a 32-bit frame write with no test, no depth, no blend, no fog, no dither and no alpha
	// correction. Any other bit set, known or added later, sends the draw to the rasterizer.
	GSScanlineSelector want;
	want.key = 0;
	want.fpsm = 0;
	want.zpsm = 3;
	want.atst = ATST_ALWAYS;
	want.tfx = TFX_DECAL;
	want.tcc = 1;
	want.fst = 1;
	want.tlu = texel32 ? 0 : 1;
	want.ababcd = 0xff;
	want.fwrite = 1;
	want.prim = GS_SPRITE_CLASS;

	if (sel.key != want.key)
		return false;

	// The frame mask is not in the selector; a masked bit would keep its old value.
#if _M_SSE >= 0x501
	if (gd.fm != 0)
		return false;
#else
	if (!gd.fm.eq(GSVector4i::zero()))
		return false;
#endif

	// Scan masking skips rows.
	if (data.scanmsk_value & 2)
		return false;

	if (!(data.index && data.index_count >= 2 && (data.index_count & 1) == 0))
		return false;

	// The palette road reads the snapshot and the palette; the 32-bit road reads neither.
	if (!texel32 && !(gd.tex[0] && gd.clut))
		return false;

	return uv || OnUVGrid(data);
}

// The scissor's own width limit, which bounds a sprite's row.
static constexpr int max_width = 2048;

// Where a pixel sits within its four-pixel group of a 32-bit or 16-bit frame (GSDrawScanline's WritePixel).
static constexpr int group_offset[4] = {0, 4, 16, 20};

// The coordinate as the scanline forms it, wrapped by gd.t: repeat is (c & min) | max, clamp is
// min(max(c, min), max). The scanline also truncates a negative coordinate toward zero and
// saturates one past 2047.9375 texels; neither can happen here. One texel per pixel puts the
// far corner's U at the near corner's plus the width, and U is a 14-bit field, so every texel
// read is in [0, 1024). A mipmapped draw at a constant level shifts the coordinate down by the level first.
static u32 WrapTexel(s32 c, s16 tmin, s16 tmax, bool repeat, int lod = 0)
{
	const s16 t = static_cast<s16>((c >> lod) >> 16);
	const s16 w = repeat ? static_cast<s16>((t & tmin) | tmax) : std::min(std::max(t, tmin), tmax);
	return static_cast<u16>(w);
}

// Reads the draw's sprites into sw.palette_blocks, refusing (writing nothing) any draw with a sprite that the
// copies cannot reproduce:
//
// - The texture coordinate must step exactly one texel per pixel on both axes (two, with `minify`, on either
//   axis: the 32-bit texel copy's box filter). Positions and UVs arrive as sixteenths, so every product and
//   sum below is exact in float and the coordinate at pixel k is the seed plus k whole steps, whatever the
//   scanline's vector width.
// - The sprite's own extent must be a power of two on both axes. Otherwise the rasterizer walks
//   the coordinate a hair low from the second pixel on (GSSpriteRampBias), which this does
//   not model.
// - Under notest, a sprite the scissor cuts off-grid on the left or right is refused. Run() picks
//   notest by checking a bounding box that is already clipped to the scissor, so a clipped sprite
//   can carry it, and a notest scanline assumes its span starts and ends on the vector grid. What
//   it writes for an off-grid span is not what this would write.
template <bool minify>
bool GSSwPrimRenderFunctions::CollectSprites(GSSwPrimRenderState& sw, const GSRasterizerData& data, int lod, bool divide_q)
{
	// One texel of the level a draw samples is 2^lod texels of the coordinate the vertices carry.
	const float one_texel = 65536.0f * static_cast<float>(1 << lod);
#if _M_SSE >= 0x501
	constexpr int notest_grid = 8;
#else
	constexpr int notest_grid = 4;
#endif
	const bool notest = data.global.sel.notest;

	const GSVertexSW* vertex = data.vertex;
	const u16* index = data.index;

	sw.palette_blocks.clear();

	for (int i = 0; i < data.index_count; i += 2)
	{
		const GSVertexSW& v0 = vertex[index[i + 0]];
		const GSVertexSW& v1 = vertex[index[i + 1]];

		// Order the corners as DrawSprite does, each axis on its own.
		const GSVector4 mask = (v0.p < v1.p).xyzw(GSVector4::zero());
		const GSVector4 p0 = v1.p.blend32(v0.p, mask);
		GSVector4 t0 = v1.t.blend32(v0.t, mask);
		const GSVector4 p1 = v0.p.blend32(v1.p, mask);
		GSVector4 t1 = v0.t.blend32(v1.t, mask);

		const GSVector4i extent(p0.xyxy(p1).ceil());
		const GSVector4i r = extent.rintersect(data.scissor);

		if (r.rempty())
			continue;

		if (divide_q)
		{
			// The vertices carry S and T unscaled by Q, and the scanline divides each pixel's S and T by the
			// Q of the sprite's second vertex (DrawSprite keeps it for the whole sprite). Divided by a power
			// of two, every value the scanline forms is exact, so the divided coordinates are the
			// coordinates it samples. Any other Q divides inexactly, and is the scanline's.
			u32 bits;
			std::memcpy(&bits, &v1.t.F32[2], sizeof(bits));
			const u32 exponent = (bits >> 23) & 0xff;
			if ((bits >> 31) != 0 || (bits & 0x7fffff) != 0 || exponent == 0 || exponent == 0xff)
				return false;

			const float inv_q = 1.0f / v1.t.F32[2];
			const GSVector4 scale(inv_q, inv_q, 1.0f, 1.0f);
			t0 = t0 * scale;
			t1 = t1 * scale;

			// The divided coordinate has to land on the sixteenth grid inside the range a UV can encode, as
			// OnUVGrid() holds an ST draw's converted coordinates to.
			constexpr float sixteenth = 4096.0f;
			constexpr float limit = 16384.0f * sixteenth;
			for (const float x : {t0.x, t0.y, t1.x, t1.y})
			{
				if (!(x >= 0.0f && x < limit) || (x / sixteenth) != std::floor(x / sixteenth))
					return false;
			}
		}

		const GSVector4 dt = (t1 - t0) / (p1 - p0);
		int su = 1;
		int sv = 1;
		if constexpr (minify)
		{
			if (dt.x == 2.0f * one_texel)
				su = 2;
			if (dt.y == 2.0f * one_texel)
				sv = 2;
		}
		if (dt.x != su * one_texel || dt.y != sv * one_texel)
			return false;

		const int w = extent.width();
		const int h = extent.height();
		if ((w & (w - 1)) != 0 || (h & (h - 1)) != 0 || r.width() > max_width)
			return false;

		if (notest && ((r.left | r.right) & (notest_grid - 1)) != 0)
			return false;

		// Without TCC the alpha is the sprite's flat colour, which the scanline takes from its second
		// vertex, truncated and brought down from the seven-bit fraction with unsigned saturation.
		u32 alpha = 0;
		if (!data.global.sel.tcc)
			alpha = static_cast<u32>(std::clamp(static_cast<int>(v1.c.w) >> 7, 0, 255)) << 24;

		const GSVector4 seed = t0 + dt * (GSVector4(r.left, r.top) - p0);
		sw.palette_blocks.push_back({r, static_cast<s32>(seed.x), static_cast<s32>(seed.y), alpha, su, sv});
	}

	return true;
}

// Mirrors GSRasterizer::DrawSprite and the scanline's nearest lookup for the one shape
// IsPaletteBlockCopy() admits (see CollectSprites() for what a sprite must be), and refuses, writing
// nothing, any draw where it could not.
bool GSSwPrimRenderFunctions::DrawPaletteBlocks(GSSwPrimRenderState& sw, const GSRasterizerData& data)
{
	if (!CollectSprites(sw, data))
		return false;

	const GSScanlineGlobalData& gd = data.global;
	const u32 keep = gd.sel.tcc ? 0xFFFFFFFFu : 0x00FFFFFFu;

	const s16 umin = static_cast<s16>(gd.t.min.U16[0]);
	const s16 umax = static_cast<s16>(gd.t.max.U16[0]);
	const bool urepeat = gd.t.mask.U16[0] != 0;
	const s16 vmin = static_cast<s16>(gd.t.min.U16[4]);
	const s16 vmax = static_cast<s16>(gd.t.max.U16[4]);
	const bool vrepeat = gd.t.mask.U16[4] != 0;

	const u8* tex = static_cast<const u8*>(gd.tex[0]);
	const u32* clut = gd.clut;
	const int pitch_shift = gd.sel.tw + 3;
	u8* vm = static_cast<u8*>(gd.vm);

	u16 columns[max_width];
	s32 offsets[max_width]; // the pixel's byte offset from its row's start, before the wrap; may be negative

	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
	{
		const int width = b.rect.width();
		int col_min = INT_MAX;
		int col_max = INT_MIN;

		for (int k = 0; k < width; k++)
		{
			const int x = b.rect.left + k;
			const int col = gd.fzbc[x >> 2].x;
			col_min = std::min(col_min, col);
			col_max = std::max(col_max, col);
			columns[k] = static_cast<u16>(WrapTexel(b.u + k * 65536, umin, umax, urepeat));
			offsets[k] = col * 2 + group_offset[x & 3];
		}

		for (int y = b.rect.top; y < b.rect.bottom; y++)
		{
			const u8* row = tex + (WrapTexel(b.v + (y - b.rect.top) * 65536, vmin, vmax, vrepeat) << pitch_shift);
			const int base = gd.fzbr[y].x;

			if (base + col_min >= 0 && static_cast<u32>(base + col_max) < HALF_VM_SIZE)
			{
				// No pixel of this row wraps, so its address is the row's plus the column's.
				u8* line = vm + static_cast<ptrdiff_t>(base) * 2;
				for (int k = 0; k < width; k++)
					*reinterpret_cast<u32*>(line + static_cast<ptrdiff_t>(offsets[k])) = (clut[row[columns[k]]] & keep) | b.alpha;
			}
			else
			{
				for (int k = 0; k < width; k++)
				{
					const int x = b.rect.left + k;
					const int fa = (base + gd.fzbc[x >> 2].x) % HALF_VM_SIZE;
					*reinterpret_cast<u32*>(vm + fa * 2 + group_offset[x & 3]) = (clut[row[columns[k]]] & keep) | b.alpha;
				}
			}
		}
	}

	return true;
}

// The 32-bit texel copy: IsPaletteBlockCopy(..., texel32) admits a sprite draw from a 32-bit texture into a 32-bit
// frame, and this writes the pixels the scanline would, without the rasterizer. Stuntman draws 189 of these a frame
// (the 2:1 box filter that builds the half-size copies of its road textures), and the rasterizer spends its time on
// setup and on a snapshot of the texture for each.
//
// Mirrors DrawPaletteBlocks (see CollectSprites for the sprites it accepts), except that the texels come from local
// memory itself, as DrawMaskedTexels reads them, and not from the snapshot (the draw is judged before it is taken).
//
// Three lookups are taken, each per axis:
//
// - Nearest, one texel a pixel: the pixel is the texel at the coordinate's integer part.
// - Bilinear, one texel a pixel, with every weight zero. The scanline reads the texel at the coordinate less half a
//   texel, and weighs the next one in by the coordinate's four fraction bits. Those are zero when the coordinate's
//   own fraction is in [1/2, 1/2 + 1/16), where the lookup is the nearest one's: lerp16_4() adds (b - a) * 0 >> 4 to a.
// - Bilinear, two texels a pixel, with every weight a half. The coordinate's fraction is in [0, 1/16), so less half a
//   texel it is in [1/2, 1/2 + 1/16): the texel before the coordinate's and the one at it, weighted 8 of 16. lerp16_4()
//   gives a + ((b - a) * 8 >> 4), which is the average of a and b rounded down, bytewise; the vertical lerp takes the
//   average of the two rows' results the same way.
//
// It refuses, writing nothing, a draw where it could not read what the snapshot would hold:
//
// - The snapshot holds the blocks GetTextureMinMax() covers, and reads zero or an earlier draw's bytes from the
//   rest of its buffer. A texel outside the covered blocks is therefore not a texel this can read; the draw is
//   refused.
// - Every texel is read before the first pixel is written, as the snapshot is taken before the draw. A draw that
//   writes the blocks it reads sees the texture as it was.
// - A texel whose address is past the end of local memory is the rasterizer's.
// - A coordinate less than half a texel in, where the scanline truncates a negative coordinate toward zero.
//
// The pixel is the texel, with the vertex's alpha in place of the texel's when the texture colour function has no
// TCC (the same keep and alpha as DrawPaletteBlocks).
bool GSSwPrimRenderFunctions::DrawTexels32(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data)
{
	GSScanlineGlobalData& gd = data.global;
	GSVertexTrace& vt = hw.m_vt;
	const GSDrawingContext* context = hw.m_context;
	const bool ltf = gd.sel.ltf;

	if (!(ltf ? CollectSprites<true>(sw, data) : CollectSprites<false>(sw, data)))
		return false;

	if (sw.palette_blocks.empty())
		return true;

	size_t texels = 0;
	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
		texels += static_cast<size_t>(b.rect.width()) * b.rect.height();

	// Large draws are the scanline's, and bound the scratch.
	constexpr size_t max_texels = size_t(1) << 20;
	if (texels > max_texels)
		return false;

	// Which taps each axis of each sprite takes, settled before anything is read. `first` is the integer texel the
	// first pixel's first tap sits at.
	struct Axis
	{
		int first;
		int taps;
	};

	const auto axis = [ltf](s32 seed, int step, Axis& out) -> bool {
		const s32 whole = seed >> 16;
		const u32 fraction = static_cast<u32>(seed) & 0xffff;

		if (!ltf)
		{
			out = {whole, 1};
			return step == 1 && seed >= 0;
		}

		if (step == 1)
		{
			// Every weight zero.
			out = {whole, 1};
			return fraction >= 0x8000 && fraction < 0x9000 && seed >= 0;
		}

		// Every weight a half; the first tap is the texel before the coordinate's, and must exist.
		out = {whole - 1, 2};
		return fraction < 0x1000 && whole >= 1;
	};

	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
	{
		Axis ax, ay;
		if (!axis(b.u, b.su, ax) || !axis(b.v, b.sv, ay))
			return false;
	}

	// The blocks the snapshot would have taken, found as PrepareTexture() finds them.
	hw.CalculatePrimitiveCoversWithoutGaps(false);
	const GIFRegTEX0 TEX0 = context->GetSizeFixedTEX0(vt.m_min.t.xyxy(vt.m_max.t), vt.IsLinear(), false);
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector4i cover = hw.GetTextureMinMax(TEX0, context->CLAMP, gd.sel.ltf, true).coverage.ralign<Align_Outside>(psm.bs);

	// The wrap limits, as PrepareTexture() leaves them for the scanline.
	SetupTextureWrap(gd, context, TEX0);

	const s16 umin = static_cast<s16>(gd.t.min.U16[0]);
	const s16 umax = static_cast<s16>(gd.t.max.U16[0]);
	const bool urepeat = gd.t.mask.U16[0] != 0;
	const s16 vmin = static_cast<s16>(gd.t.min.U16[4]);
	const s16 vmax = static_cast<s16>(gd.t.max.U16[4]);
	const bool vrepeat = gd.t.mask.U16[4] != 0;

	const GSOffset off = hw.m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	const u32* vm32 = reinterpret_cast<const u32*>(hw.m_mem.m_vm8);
	constexpr u32 limit = VM_SIZE / 4; // how far a texel's address may reach, in 32-bit pixels
	static_assert((limit & (limit - 1)) == 0, "the address mask below needs a power of two");

	// The bytewise average of two pixels, rounded down: a + ((b - a) >> 1) in each of the four channels.
	const auto average = [](u32 a, u32 b) -> u32 {
		return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1);
	};

	sw.texel_words.resize(texels);
	u32* out = sw.texel_words.data();

	u16 columns[2][max_width];

	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
	{
		const int width = b.rect.width();

		Axis ax, ay;
		axis(b.u, b.su, ax);
		axis(b.v, b.sv, ay);

		// Each tap's column, wrapped on its own as the scanline wraps the second tap after adding one to the first.
		// Almost always none of it wraps: the texels run from the first to the last, and only plain repeat and clamp
		// reach here (the region modes are refused), so a repeat's max is 0 and its min a low mask, and a clamp's min
		// is 0: a coordinate in [0, limit] passes through the wrap unchanged.
		const int last = ax.first + (width - 1) * b.su + (ax.taps - 1);
		const bool plain = ax.first >= 0 && last <= (urepeat ? umin : umax);
		if (plain && (ax.first < cover.left || last >= cover.right))
			return false;

		// The pair load needs an even first column; a single tap steps by one column whatever it is.
		const bool stepped = plain && (ax.taps == 1 || (ax.first & 1) == 0);

		for (int t = 0; t < ax.taps && !stepped; t++)
		{
			for (int k = 0; k < width; k++)
			{
				const int x = ax.first + k * b.su + t;
				u32 u = static_cast<u32>(x);
				if (!plain)
				{
					u = WrapTexel(x * 65536, umin, umax, urepeat);
					if (static_cast<int>(u) < cover.left || static_cast<int>(u) >= cover.right)
						return false;
				}
				columns[t][k] = static_cast<u16>(u);
			}
		}

		// One row of the sprite at a time, with the number of taps on each axis fixed at compile time so the loop is the
		// loads and the averages and little else. An address past the end of local memory is masked for its load and
		// remembered, and the draw refused when the row is done: nothing has been written yet.
		//
		// STEP: when nothing wraps, the columns are the sprite's first plus the step times k, and need no table. For
		// two taps that also puts the first tap on an even column when the first is even, and an even column and the
		// next one are adjacent words in a 32-bit block (the column table pairs them, and the page, block and xor
		// terms of the address do not touch its low bit), so the two taps are one 64-bit load.
		const auto gather_rows = [&](auto tu, auto tv, auto stepped) -> bool {
			constexpr int TU = decltype(tu)::value;
			constexpr int TV = decltype(tv)::value;
			constexpr bool STEP = decltype(stepped)::value;

			for (int y = b.rect.top; y < b.rect.bottom; y++)
			{
				GSOffset::PAHelper pa[TV];

				for (int t = 0; t < TV; t++)
				{
					const int row = ay.first + (y - b.rect.top) * b.sv + t;
					const u32 v = WrapTexel(row * 65536, vmin, vmax, vrepeat);
					if (static_cast<int>(v) < cover.top || static_cast<int>(v) >= cover.bottom)
						return false;

					pa[t] = off.paMulti(0, static_cast<int>(v));
				}

				u32 past = 0;

				// One row of taps of one pixel: the pixel's texel, or the average of its two.
				const auto row_pixel = [&](const GSOffset::PAHelper& row, int k) -> u32 {
					if constexpr (STEP && TU == 2)
					{
						const u32 addr = row.value(static_cast<u32>(ax.first + 2 * k));
						past |= addr;
						u64 pair;
						std::memcpy(&pair, &vm32[addr & (limit - 2)], sizeof(pair));
						return average(static_cast<u32>(pair), static_cast<u32>(pair >> 32));
					}
					else
					{
						const u32 x0 = STEP ? static_cast<u32>(ax.first + k) : columns[0][k];
						u32 addr = row.value(x0);
						past |= addr;
						u32 pixel = vm32[addr & (limit - 1)];

						if constexpr (TU == 2)
						{
							addr = row.value(columns[1][k]);
							past |= addr;
							pixel = average(pixel, vm32[addr & (limit - 1)]);
						}

						return pixel;
					}
				};

				for (int k = 0; k < width; k++)
				{
					u32 pixel = row_pixel(pa[0], k);

					if constexpr (TV == 2)
						pixel = average(pixel, row_pixel(pa[1], k));

					*out++ = pixel;
				}

				if (past >= limit)
					return false;
			}

			return true;
		};

		using One = std::integral_constant<int, 1>;
		using Two = std::integral_constant<int, 2>;
		using Yes = std::true_type;
		using No = std::false_type;

		bool read;
		if (ax.taps == 2)
		{
			if (ay.taps == 2)
				read = stepped ? gather_rows(Two(), Two(), Yes()) : gather_rows(Two(), Two(), No());
			else
				read = stepped ? gather_rows(Two(), One(), Yes()) : gather_rows(Two(), One(), No());
		}
		else
		{
			if (ay.taps == 2)
				read = stepped ? gather_rows(One(), Two(), Yes()) : gather_rows(One(), Two(), No());
			else
				read = stepped ? gather_rows(One(), One(), Yes()) : gather_rows(One(), One(), No());
		}

		if (!read)
			return false;
	}

	// Nothing was written until every texel was read.
	const u32 keep = gd.sel.tcc ? 0xFFFFFFFFu : 0x00FFFFFFu;
	u8* vm = static_cast<u8*>(gd.vm);
	const u32* texel = sw.texel_words.data();

	s32 offsets[max_width]; // the pixel's byte offset from its row's start, before the wrap; may be negative

	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
	{
		const int width = b.rect.width();
		int col_min = INT_MAX;
		int col_max = INT_MIN;

		for (int k = 0; k < width; k++)
		{
			const int x = b.rect.left + k;
			const int col = gd.fzbc[x >> 2].x;
			col_min = std::min(col_min, col);
			col_max = std::max(col_max, col);
			offsets[k] = col * 2 + group_offset[x & 3];
		}

		for (int y = b.rect.top; y < b.rect.bottom; y++, texel += width)
		{
			const int base = gd.fzbr[y].x;

			if (base + col_min >= 0 && static_cast<u32>(base + col_max) < HALF_VM_SIZE)
			{
				// No pixel of this row wraps, so its address is the row's plus the column's.
				u8* line = vm + static_cast<ptrdiff_t>(base) * 2;
				for (int k = 0; k < width; k++)
					*reinterpret_cast<u32*>(line + static_cast<ptrdiff_t>(offsets[k])) = (texel[k] & keep) | b.alpha;
			}
			else
			{
				for (int k = 0; k < width; k++)
				{
					const int x = b.rect.left + k;
					const int fa = (base + gd.fzbc[x >> 2].x) % HALF_VM_SIZE;
					*reinterpret_cast<u32*>(vm + fa * 2 + group_offset[x & 3]) = (texel[k] & keep) | b.alpha;
				}
			}
		}
	}

	return true;
}

// The masked texel copy: a sprite draw that writes each texel's palette entry one-to-one into a 16-bit
// frame, under any write mask. Spider-Man 3 rebuilds 4-bit textures this way thousands of times a frame
// (an index nibble per pass, masked into one corner of the 16-bit pixel), and the rasterizer spends its time
// in the texture snapshot, which converts whole blocks for the few texels a draw reads.
//
// Like the palette block copy it is decided on the scanline's own selector, and it runs before the
// snapshot is taken, so the selector is complete but the texture half of it (PrepareTexture) is not:
// the bits that half would set are filled in below the way it would. A selector bit added later, known
// or not, is still a refusal.
bool GSSwPrimRenderFunctions::IsMaskedTexelCopy(const GSRasterizerData& data, bool uv, bool mip)
{
	const GSScanlineGlobalData& gd = data.global;
	GSScanlineSelector sel = gd.sel;

	// Plain repeat or clamp on each axis. The region modes can address past the texture's rows.
	if (sel.wms > CLAMP_CLAMP || sel.wmt > CLAMP_CLAMP)
		return false;

	// Fields that cannot change what this shape writes. The wrap is read from the draw's own limits; an
	// 8-bit texel colour passes the colour clamp unchanged; datm is read only with date; notest picks how the
	// scanline handles span edges (the sprites are held to its grid in CollectSprites); the frame is always
	// read and merged under the mask, which is all that rfb adds here (a blend, a destination alpha test and
	// an alpha test that keeps the old colour also set it, and each is a field of its own below). FBA sets
	// the frame's alpha bit and is applied by the writer.
	sel.wms = 0;
	sel.wmt = 0;
	sel.colclamp = 0;
	sel.datm = 0;
	sel.notest = 0;
	sel.rfb = 0;
	sel.fba = 0;

	// Without TCC the alpha written is the vertex's own (applied by the writer).
	sel.tcc = 1;

	// PrepareTexture sets the texture pitch, which reads nothing here, and, off the mipmap road, fst for a
	// sprite whatever its coordinates came as (the ST case is held to the sixteenth grid by `uv`). A mipmapped
	// draw keeps the primitive's own fst: UV coordinates are integers, and ST ones are divided by Q per pixel,
	// which CollectSprites() holds to the exact case. Its level of detail has to be a constant level taken
	// nearest (SetupMipLod() has set both), which is then one texture, read at the coordinate shifted down by
	// the level.
	if (mip)
	{
		if (sel.mmin != 1 || !sel.lcm)
			return false;

		sel.mmin = 0;
		sel.lcm = 0;
		sel.fst = 0;
	}
	else
	{
		sel.fst = 1;
	}

	// A textured sprite with a nearest, non-mipmapped UV lookup through the palette, decal, a 16-bit frame
	// write with no test, no depth, no blend, no fog and no dither. A palette entry is an 8-bit texel colour,
	// so the colour clamp and the wrap mask pass it unchanged.
	GSScanlineSelector want;
	want.key = 0;
	want.fpsm = 2;
	want.zpsm = 3;
	want.atst = ATST_ALWAYS;
	want.tfx = TFX_DECAL;
	want.tcc = 1;
	want.fst = mip ? 0 : 1;
	want.tlu = 1;
	want.ababcd = 0xff;
	want.fwrite = 1;
	want.prim = GS_SPRITE_CLASS;

	if (sel.key != want.key)
		return false;

	// Scan masking skips rows.
	if (data.scanmsk_value & 2)
		return false;

	if (!(data.index && data.index_count >= 2 && (data.index_count & 1) == 0 && gd.clut))
		return false;

	return uv || mip || OnUVGrid(data);
}

// Reads the palette index of every texel the draw's sprites read straight from local memory, then writes the
// sprites' pixels, in order, into the 16-bit frame. Mirrors the scanline as DrawPaletteBlocks does (see
// CollectSprites for the sprites it accepts), and refuses, writing nothing, a draw where it could not:
//
// - The rasterizer's snapshot holds the blocks GetTextureMinMax() covers, and reads zero or an earlier draw's
//   bytes from the rest of its buffer. A texel outside the covered blocks is therefore not a texel this can
//   read; the draw is refused.
// - Every texel is read before the first pixel is written, as the snapshot is taken before the draw. A draw
//   that writes the blocks it reads (an in-place expansion) sees the texture as it was.
// - The pixel is the palette entry packed to five bits a colour and one of alpha, with the frame's bit
//   forced on under FBA, merged into the old pixel under the write mask: new = (pixel & ~mask) | (old & mask).
bool GSSwPrimRenderFunctions::DrawMaskedTexels(GSRenderer& hw, GSSwPrimRenderState& sw, GSRasterizerData& data, bool mip)
{
	GSScanlineGlobalData& gd = data.global;
	GSVertexTrace& vt = hw.m_vt;
	const GSDrawingContext* context = hw.m_context;

	// A mipmapped draw reads one level, `lod`, with its coordinate shifted down by it; ST coordinates in
	// that case arrive undivided.
	const int lod = mip ? static_cast<int>(gd.lod.i.U32[0]) : 0;
	if (!CollectSprites(sw, data, lod, mip && !hw.PRIM->FST))
		return false;

	if (sw.palette_blocks.empty())
		return true;

	size_t texels = 0;
	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
		texels += static_cast<size_t>(b.rect.width()) * b.rect.height();

	// Large draws are the scanline's, and bound the scratch.
	constexpr size_t max_texels = size_t(1) << 20;
	if (texels > max_texels)
		return false;

	// The blocks the snapshot would have taken of the level this draw reads, found as PrepareTexture() finds
	// them: level 0 as is, a mip level with the coordinates and the region halved once for each level down.
	hw.CalculatePrimitiveCoversWithoutGaps(false);
	const GIFRegTEX0 TEX0 = context->GetSizeFixedTEX0(vt.m_min.t.xyxy(vt.m_max.t), vt.IsLinear(), mip);
	GIFRegTEX0 LEVEL0 = TEX0;
	GSVector4i cover;
	if (lod == 0)
	{
		cover = hw.GetTextureMinMax(TEX0, context->CLAMP, vt.IsLinear(), true).coverage;
	}
	else
	{
		GIFRegCLAMP MIP_CLAMP = context->CLAMP;
		const GSVector4 tmin = vt.m_min.t;
		const GSVector4 tmax = vt.m_max.t;

		for (int i = 1; i <= lod; i++)
		{
			LEVEL0 = hw.GetTex0Layer(i);

			MIP_CLAMP.MINU >>= 1;
			MIP_CLAMP.MINV >>= 1;
			MIP_CLAMP.MAXU >>= 1;
			MIP_CLAMP.MAXV >>= 1;

			vt.m_min.t *= 0.5f;
			vt.m_max.t *= 0.5f;
		}

		cover = hw.GetTextureMinMax(LEVEL0, MIP_CLAMP, gd.sel.ltf, true).coverage;

		vt.m_min.t = tmin;
		vt.m_max.t = tmax;
	}

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[LEVEL0.PSM];
	cover = cover.ralign<Align_Outside>(psm.bs);

	// The wrap limits are the draw's (level 0's size); the scanline shifts them down by the level too.
	SetupTextureWrap(gd, context, TEX0);

	const auto shifted = [lod](u16 limit) { return static_cast<s16>(limit >> lod); };
	const s16 umin = shifted(gd.t.min.U16[0]);
	const s16 umax = shifted(gd.t.max.U16[0]);
	const bool urepeat = gd.t.mask.U16[0] != 0;
	const s16 vmin = shifted(gd.t.min.U16[4]);
	const s16 vmax = shifted(gd.t.max.U16[4]);
	const bool vrepeat = gd.t.mask.U16[4] != 0;

	const GSOffset off = hw.m_mem.GetOffset(LEVEL0.TBP0, LEVEL0.TBW, LEVEL0.PSM);
	const u8* vm8 = hw.m_mem.m_vm8;
	const u32* vm32 = reinterpret_cast<const u32*>(vm8);

	// How the palette index sits at a texel's address.
	enum Source { T8, T4, T8H, T4HL, T4HH };

	// The frame mask in the pixel's own bits, as the scanline holds it (gd.fm repeats it above bit 16).
#if _M_SSE >= 0x501
	const u32 mask = gd.fm & 0xffff;
#else
	const u32 mask = gd.fm.U32[0] & 0xffff;
#endif
	const u32 alpha_bit = gd.sel.fba ? 0x8000u : 0u;
	const u32 keep = gd.sel.tcc ? 0xFFFFFFFFu : 0x00FFFFFFu;
	const u32* clut = gd.clut;
	u8* vm = static_cast<u8*>(gd.vm);

	// Writes one sprite's pixels from the palette indices read for it.
	const auto write_block = [&](const GSSwPrimRenderState::PaletteBlock& b, const u8* indices) {
		s32 offsets[max_width]; // the pixel's byte offset from its row's start, before the wrap; may be negative

		const int width = b.rect.width();
		int col_min = INT_MAX;
		int col_max = INT_MIN;

		for (int k = 0; k < width; k++)
		{
			const int x = b.rect.left + k;
			const int col = gd.fzbc[x >> 2].x;
			col_min = std::min(col_min, col);
			col_max = std::max(col_max, col);
			offsets[k] = col * 2 + group_offset[x & 3];
		}

		for (int y = b.rect.top; y < b.rect.bottom; y++, indices += width)
		{
			const int base = gd.fzbr[y].x;

			// The pixel's 32-bit colour, packed to the frame's five-five-five-one.
			const auto pixel = [&](int k) -> u32 {
				const u32 c = (clut[indices[k]] & keep) | b.alpha;
				return ((c >> 3) & 0x001f) | ((c >> 6) & 0x03e0) | ((c >> 9) & 0x7c00) | ((c >> 16) & 0x8000) | alpha_bit;
			};

			if (base + col_min >= 0 && static_cast<u32>(base + col_max) < HALF_VM_SIZE)
			{
				// No pixel of this row wraps, so its address is the row's plus the column's.
				u8* line = vm + static_cast<ptrdiff_t>(base) * 2;
				for (int k = 0; k < width; k++)
				{
					u16* dst = reinterpret_cast<u16*>(line + static_cast<ptrdiff_t>(offsets[k]));
					*dst = static_cast<u16>((pixel(k) & ~mask) | (*dst & mask));
				}
			}
			else
			{
				for (int k = 0; k < width; k++)
				{
					const int x = b.rect.left + k;
					const int fa = (base + gd.fzbc[x >> 2].x) % HALF_VM_SIZE;
					u16* dst = reinterpret_cast<u16*>(vm + fa * 2 + group_offset[x & 3]);
					*dst = static_cast<u16>((pixel(k) & ~mask) | (*dst & mask));
				}
			}
		}
	};

	sw.texel_scratch.resize(texels);
	u8* const scratch = sw.texel_scratch.data();

	// The coordinate's step from one pixel to the next: a texel of the level read.
	const s32 step = 65536 << lod;

	// Reads every texel of every sprite into the scratch, in the order they are written, for one source format.
	const auto gather = [&](auto format) -> bool {
		constexpr int F = decltype(format)::value;
		// How far a texel's address may reach: bytes, nibbles, or 32-bit pixels.
		constexpr u32 limit = F == T8 ? VM_SIZE : F == T4 ? VM_SIZE * 2 : VM_SIZE / 4;

		u8* indices = scratch;
		u16 columns[max_width];

		for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
		{
			const int width = b.rect.width();

			// Almost always the coordinate of pixel k is the first one plus k, with no wrap to apply: it steps a
			// whole texel of the level a pixel, so the first and last decide. Only plain repeat and clamp reach
			// here (the region modes are refused), so a repeat's max is 0 and its min a low mask, and a clamp's
			// min is 0: a coordinate in [0, limit] passes through the wrap unchanged.
			const int u_first = (b.u >> lod) >> 16;
			const int u_last = ((b.u + (width - 1) * step) >> lod) >> 16;
			if (u_first >= 0 && u_last <= (urepeat ? umin : umax))
			{
				if (u_first < cover.left || u_last >= cover.right)
					return false;
				for (int k = 0; k < width; k++)
					columns[k] = static_cast<u16>(u_first + k);
			}
			else
			{
				for (int k = 0; k < width; k++)
				{
					const u32 u = WrapTexel(b.u + k * step, umin, umax, urepeat, lod);
					if (static_cast<int>(u) < cover.left || static_cast<int>(u) >= cover.right)
						return false;
					columns[k] = static_cast<u16>(u);
				}
			}

			for (int y = b.rect.top; y < b.rect.bottom; y++)
			{
				const u32 v = WrapTexel(b.v + (y - b.rect.top) * step, vmin, vmax, vrepeat, lod);
				if (static_cast<int>(v) < cover.top || static_cast<int>(v) >= cover.bottom)
					return false;

				const GSOffset::PAHelper pa = off.paMulti(0, static_cast<int>(v));

				for (int k = 0; k < width; k++)
				{
					const u32 addr = pa.value(columns[k]);
					if (addr >= limit)
						return false;

					u32 index;
					if constexpr (F == T8)
						index = vm8[addr];
					else if constexpr (F == T4)
						index = (vm8[addr >> 1] >> ((addr & 1) << 2)) & 0x0f;
					else if constexpr (F == T8H)
						index = vm32[addr] >> 24;
					else if constexpr (F == T4HL)
						index = (vm32[addr] >> 24) & 0x0f;
					else
						index = vm32[addr] >> 28;
					*indices++ = static_cast<u8>(index);
				}
			}
		}

		return true;
	};

	bool read;
	switch (LEVEL0.PSM)
	{
		case PSMT8: read = gather(std::integral_constant<int, T8>()); break;
		case PSMT4: read = gather(std::integral_constant<int, T4>()); break;
		case PSMT8H: read = gather(std::integral_constant<int, T8H>()); break;
		case PSMT4HL: read = gather(std::integral_constant<int, T4HL>()); break;
		case PSMT4HH: read = gather(std::integral_constant<int, T4HH>()); break;
		default: return false;
	}

	if (!read)
		return false;

	// Nothing was written until every texel was read.
	const u8* indices = scratch;
	for (const GSSwPrimRenderState::PaletteBlock& b : sw.palette_blocks)
	{
		write_block(b, indices);
		indices += static_cast<size_t>(b.rect.width()) * b.rect.height();
	}

	return true;
}
