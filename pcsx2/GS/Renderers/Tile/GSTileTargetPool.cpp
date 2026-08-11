// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileTargetPool.h"

#include "GS/GSExtra.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "common/Assertions.h"

GSTileTargetPool::GSTileTargetPool() = default;

GSTileTargetPool::~GSTileTargetPool()
{
	// Textures must have been released through ReleaseAll() while the device was
	// still alive; by destructor time the device may already be gone.
	for (const Slot& s : m_slots)
		pxAssert(!s.alive);
}

GSTileTargetPool::Geometry GSTileTargetPool::GeometryFor(const GSTileSurfaceLayout& layout)
{
	const GSVector2i pgs = GSLocalMemory::m_psm[layout.psm].pgs;
	// Every M2-native format (CT32/CT24, Z32 family, 16-bit families) has 64-pixel-wide
	// pages, which is what makes ppr == bw. 8/4-bit layouts never reach the pool.
	pxAssert(pgs.x == 64);
	return Geometry{static_cast<int>(layout.bw), pgs};
}

int GSTileTargetPool::HeightForPages(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	const Geometry g = GeometryFor(layout);
	const u32 base = (layout.bp >> 5) & (GS_MAX_PAGES - 1);
	int max_row = -1;
	pages.forEachSetPage([&](u32 page) {
		const u32 rel = (page + GS_MAX_PAGES - base) & (GS_MAX_PAGES - 1);
		max_row = std::max(max_row, static_cast<int>(rel) / g.ppr);
	});
	return (max_row + 1) * g.pgs.y;
}

GSTileTargetPool::Slot& GSTileTargetPool::GetSlot(u32 handle)
{
	pxAssert(handle != 0 && handle <= m_slots.size() && m_slots[handle - 1].alive);
	return m_slots[handle - 1];
}

const GSTileTargetPool::Slot& GSTileTargetPool::GetSlot(u32 handle) const
{
	pxAssert(handle != 0 && handle <= m_slots.size() && m_slots[handle - 1].alive);
	return m_slots[handle - 1];
}

u32 GSTileTargetPool::Allocate(const GSTileSurfaceLayout& layout, int height_px)
{
	const Geometry g = GeometryFor(layout);
	const int width_px = g.ppr * 64;
	const int max_height = (GS_MAX_PAGES / g.ppr) * g.pgs.y;
	if (height_px <= 0 || height_px > max_height)
		return 0;

	GSTexture* tex = (layout.kind == GSTileSurfaceKind::Depth) ?
		g_gs_device->CreateDepthStencil(width_px, height_px, true) :
		g_gs_device->CreateRenderTarget(width_px, height_px, GSTexture::Format::Color, true);
	if (!tex)
		return 0;

	u32 handle;
	if (!m_free_handles.empty())
	{
		handle = m_free_handles.back();
		m_free_handles.pop_back();
	}
	else
	{
		m_slots.emplace_back();
		handle = static_cast<u32>(m_slots.size());
	}

	Slot& s = m_slots[handle - 1];
	s.tex = tex;
	s.kind = layout.kind;
	s.alive = true;
	return handle;
}

void GSTileTargetPool::Release(u32 handle)
{
	Slot& s = GetSlot(handle);
	g_gs_device->Recycle(s.tex);
	s.tex = nullptr;
	s.alive = false;
	m_free_handles.push_back(handle);
}

void GSTileTargetPool::ReleaseAll()
{
	for (u32 i = 0; i < m_slots.size(); i++)
	{
		if (m_slots[i].alive)
			Release(i + 1);
	}
	m_color_download.reset();
	m_uint32_download.reset();
	m_uint16_download.reset();
}

GSTexture* GSTileTargetPool::GetTexture(u32 handle) const
{
	return GetSlot(handle).tex;
}

int GSTileTargetPool::GetHeight(u32 handle) const
{
	return GetSlot(handle).tex->GetHeight();
}

bool GSTileTargetPool::EnsureHeight(u32 handle, int height_px)
{
	Slot& s = GetSlot(handle);
	const GSVector2i old_size = s.tex->GetSize();
	if (old_size.y >= height_px)
		return true;

	GSTexture* grown = (s.kind == GSTileSurfaceKind::Depth) ?
		g_gs_device->CreateDepthStencil(old_size.x, height_px, true) :
		g_gs_device->CreateRenderTarget(old_size.x, height_px, GSTexture::Format::Color, true);
	if (!grown)
		return false;

	g_gs_device->CopyRect(s.tex, grown, GSVector4i(0, 0, old_size.x, old_size.y), 0, 0);
	g_gs_device->Recycle(s.tex);
	s.tex = grown;
	return true;
}

u32 GSTileTargetPool::CollectRuns(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, std::vector<GSVector4i>& runs)
{
	runs.clear();
	const Geometry g = GeometryFor(layout);
	const u32 base = (layout.bp >> 5) & (GS_MAX_PAGES - 1);

	int run_row = -1;
	int run_col0 = 0;
	int run_col1 = 0; // inclusive
	const auto flush = [&]() {
		if (run_row >= 0)
			runs.emplace_back(run_col0 * 64, run_row * g.pgs.y, (run_col1 + 1) * 64, (run_row + 1) * g.pgs.y);
	};

	pages.forEachSetPage([&](u32 page) {
		const u32 rel = (page + GS_MAX_PAGES - base) & (GS_MAX_PAGES - 1);
		const int row = static_cast<int>(rel) / g.ppr;
		const int col = static_cast<int>(rel) % g.ppr;
		if (row == run_row && col == run_col1 + 1)
		{
			run_col1 = col;
			return;
		}
		flush();
		run_row = row;
		run_col0 = run_col1 = col;
	});
	flush();
	return static_cast<u32>(runs.size());
}

bool GSTileTargetPool::PrepareDownload(u32 width, u32 height, GSTexture::Format format, std::unique_ptr<GSDownloadTexture>* tex)
{
	GSDownloadTexture* ctex = tex->get();
	if (ctex && ctex->GetWidth() >= width && ctex->GetHeight() >= height)
		return true;

	const u32 new_width = std::max(ctex ? ctex->GetWidth() : 0, width);
	const u32 new_height = std::max(ctex ? ctex->GetHeight() : 0, height);
	tex->reset();
	*tex = g_gs_device->CreateDownloadTexture(new_width, new_height, format);
	return static_cast<bool>(*tex);
}

u8* GSTileTargetPool::GetScratch(u32 size)
{
	if (m_scratch_size < size)
	{
		m_scratch = std::make_unique<u8[]>(size);
		m_scratch_size = size;
	}
	return m_scratch.get();
}

bool GSTileTargetPool::UploadPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	if (pages.empty())
		return true;

	Slot& s = GetSlot(handle);
	if (CollectRuns(layout, pages, m_runs) == 0)
		return true;

	GSVector4i bb = m_runs[0];
	for (const GSVector4i& r : m_runs)
		bb = bb.runion(r);
	pxAssert(bb.z <= s.tex->GetWidth() && bb.w <= s.tex->GetHeight());

	GSTexture* staging = g_gs_device->CreateTexture(bb.width(), bb.height(), 1, GSTexture::Format::Color, true);
	if (!staging)
		return false;

	// The TEXA pin is load-bearing for Z16: its cells travel as RGB5A1-expanded RGBA8
	// (TA0=0 clears bit 15, TA1=0x80 sets it) and RGB5A1_TO_DEPTH16 reverses exactly
	// this expansion. 32-bit cells ignore TEXA entirely.
	GIFRegTEXA TEXA = {};
	TEXA.AEM = 0;
	TEXA.TA0 = 0;
	TEXA.TA1 = 0x80;

	const GSOffset off = mem.GetOffset(layout.bp, layout.bw, layout.psm);

	GSTexture::GSMap map;
	if (staging->Map(map))
	{
		for (const GSVector4i& r : m_runs)
			mem.ReadTexture(off, r, map.bits + (r.y - bb.y) * static_cast<u32>(map.pitch) + (r.x - bb.x) * sizeof(u32), map.pitch, TEXA);
		staging->Unmap();
	}
	else
	{
		for (const GSVector4i& r : m_runs)
		{
			const int pitch = VectorAlign(r.width() * static_cast<int>(sizeof(u32)));
			u8* buf = GetScratch(static_cast<u32>(pitch) * r.height());
			mem.ReadTexture(off, r, buf, pitch, TEXA);
			staging->Update(GSVector4i(r.x - bb.x, r.y - bb.y, r.z - bb.x, r.w - bb.y), buf, pitch);
		}
	}

	// The depth-writing pipeline variant hangs off the selector's depth_out bit, not
	// the enum: without it the color variant binds inside a depth-only render pass
	// and the draw is incompatible (validation VUID-vkCmdDraw-renderPass-02684).
	ShaderConvertSelector shader = ShaderConvertSelector(ShaderConvert::COPY);
	if (s.kind == GSTileSurfaceKind::Depth)
	{
		ShaderConvert conv;
		switch (layout.psm)
		{
			case PSMZ32:
				conv = ShaderConvert::RGBA8_TO_DEPTH32;
				break;
			case PSMZ24:
				conv = ShaderConvert::RGBA8_TO_DEPTH24;
				break;
			default:
				conv = ShaderConvert::RGB5A1_TO_DEPTH16;
				break;
		}
		shader = ShaderConvertSelector(conv, 0xf, /*depth_out=*/true);
	}

	const GSVector4 staging_size = GSVector4(staging->GetSize()).xyxy();
	for (const GSVector4i& r : m_runs)
	{
		const GSVector4 src = GSVector4(r - bb.xyxy()) / staging_size;
		g_gs_device->StretchRect(staging, src, s.tex, GSVector4(r), shader, Nearest);
	}

	g_gs_device->Recycle(staging);
	return true;
}

bool GSTileTargetPool::ReadbackPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, u32 write_mask)
{
	if (pages.empty() || write_mask == 0)
		return true;

	Slot& s = GetSlot(handle);
	if (CollectRuns(layout, pages, m_runs) == 0)
		return true;

	GSVector4i bb = m_runs[0];
	for (const GSVector4i& r : m_runs)
		bb = bb.runion(r);
	pxAssert(bb.z <= s.tex->GetWidth() && bb.w <= s.tex->GetHeight());

	const bool depth16 = (s.kind == GSTileSurfaceKind::Depth) && (layout.psm == PSMZ16 || layout.psm == PSMZ16S);
	const GSVector4i drc(0, 0, bb.width(), bb.height());

	std::unique_ptr<GSDownloadTexture>* dltex;
	g_gs_device->HintReadbackSource(s.tex);
	if (s.kind == GSTileSurfaceKind::Color)
	{
		// Native scale, raw cells: a straight copy, no convert pass.
		dltex = &m_color_download;
		if (!PrepareDownload(drc.z, drc.w, GSTexture::Format::Color, dltex) || !dltex->get())
			return false;
		dltex->get()->CopyFromTexture(drc, s.tex, bb, 0, true);
	}
	else
	{
		const GSTexture::Format fmt = depth16 ? GSTexture::Format::UInt16 : GSTexture::Format::UInt32;
		const ShaderConvert shader = depth16 ? ShaderConvert::DEPTH32_TO_16_BITS : ShaderConvert::DEPTH32_TO_32_BITS;
		dltex = depth16 ? &m_uint16_download : &m_uint32_download;
		if (!PrepareDownload(drc.z, drc.w, fmt, dltex) || !dltex->get())
			return false;

		GSTexture* tmp = g_gs_device->CreateRenderTarget(drc.z, drc.w, fmt, false);
		if (!tmp)
			return false;
		const GSVector4 src = GSVector4(bb) / GSVector4(s.tex->GetSize()).xyxy();
		g_gs_device->StretchRect(s.tex, src, tmp, GSVector4(drc), shader, Nearest);
		dltex->get()->CopyFromTexture(drc, tmp, drc, 0, true);
		g_gs_device->Recycle(tmp);
	}

	dltex->get()->Flush();
	if (!dltex->get()->Map(drc))
		return false;

	u8* bits = const_cast<u8*>(dltex->get()->GetMapPointer());
	const u32 pitch = dltex->get()->GetMapPitch();
	const GSOffset off = mem.GetOffset(layout.bp, layout.bw, layout.psm);

	for (const GSVector4i& r : m_runs)
	{
		if (depth16)
		{
			mem.WritePixel16(bits + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u16), pitch, off, r);
		}
		else
		{
			mem.WritePixel32(bits + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u32), pitch, off, r, write_mask);
		}
	}

	dltex->get()->Unmap();
	return true;
}
