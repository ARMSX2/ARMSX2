// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileTargetPool.h"

#include "common/Timer.h"

#include "GS/GSExtra.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <cstring>

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
	// Geometry from the offset's own shifts rather than GSLocalMemory::m_psm: the
	// latter is filled by a GSLocalMemory constructor, and the run builder below is
	// pure so the unit suite can exercise it with no device and no local memory.
	// Same discipline, same reason, as GSVramModel::FootprintForRect.
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	const GSVector2i pgs(1 << off.pageShiftX(), 1 << off.pageShiftY());
	// Every M2-native format (CT32/CT24, Z32 family, 16-bit families) has 64-pixel-wide
	// pages, which is what makes ppr == bw. 8/4-bit layouts never reach the pool.
	pxAssert(pgs.x == 64);
	return Geometry{static_cast<int>(layout.bw), pgs};
}

// The in-page pixel position of each of a page's 32 physical blocks, for one format.
// The GS block swizzle is page-periodic, so inverting one page's worth of bn() serves
// every page of the layout.
GSTileTargetPool::BlockGrid GSTileTargetPool::BlockGridFor(const GSTileSurfaceLayout& layout)
{
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	BlockGrid grid = {};
	grid.bs = GSVector2i(1 << off.blockShiftX(), 1 << off.blockShiftY());
	const GSVector2i pgs(1 << off.pageShiftX(), 1 << off.pageShiftY());
	grid.cols = pgs.x / grid.bs.x;
	grid.rows = pgs.y / grid.bs.y;
	pxAssert(grid.cols * grid.rows == static_cast<int>(GSVramModel::kBlocksPerPage));

	for (int by = 0; by < grid.rows; by++)
	{
		for (int bx = 0; bx < grid.cols; bx++)
		{
			// bn() folds bp in, and pool layouts are page-aligned, so the low five
			// bits are the block's index within its own physical page.
			grid.index[by * grid.cols + bx] = static_cast<u8>(off.bn(bx * grid.bs.x, by * grid.bs.y) & 31);
		}
	}
	return grid;
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

u32 GSTileTargetPool::CollectRuns(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, u32 block_mask,
	std::vector<GSVector4i>& runs)
{
	runs.clear();
	if (block_mask == 0)
		return 0;

	const Geometry g = GeometryFor(layout);
	const u32 base = (layout.bp >> 5) & (GS_MAX_PAGES - 1);

	// A page whose truth a CPU transfer has partially consumed contributes only its
	// surviving blocks. Rare by design (whole-page draws, whole-page invalidations),
	// so it takes the slow shape and leaves the whole-page path below untouched.
	if (block_mask != GSVramModel::kFullBlockMask)
	{
		const BlockGrid grid = BlockGridFor(layout);
		pages.forEachSetPage([&](u32 page) {
			const u32 rel = (page + GS_MAX_PAGES - base) & (GS_MAX_PAGES - 1);
			const int px0 = (static_cast<int>(rel) % g.ppr) * g.pgs.x;
			const int py0 = (static_cast<int>(rel) / g.ppr) * g.pgs.y;
			for (int by = 0; by < grid.rows; by++)
			{
				// Coalesce adjacent blocks along the row; the swizzle scatters block
				// indices, so a contiguous pixel span is not a contiguous index span.
				int span0 = -1;
				for (int bx = 0; bx <= grid.cols; bx++)
				{
					const bool set = bx < grid.cols && (block_mask & (1u << grid.index[by * grid.cols + bx])) != 0;
					if (set)
					{
						if (span0 < 0)
							span0 = bx;
						continue;
					}
					if (span0 >= 0)
					{
						runs.emplace_back(px0 + span0 * grid.bs.x, py0 + by * grid.bs.y, px0 + bx * grid.bs.x,
							py0 + (by + 1) * grid.bs.y);
						span0 = -1;
					}
				}
			}
		});
		return static_cast<u32>(runs.size());
	}

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

const u8* GSTileTargetPool::StageUncachedMap(const GSDownloadTexture* dl, const u8* bits, u32 pitch, int rows)
{
	if (!dl->IsMapUncached()) [[likely]]
		return bits;

	const u32 size = pitch * static_cast<u32>(rows);
	if (m_map_stage_size < size)
	{
		m_map_stage = std::make_unique<u8[]>(size);
		m_map_stage_size = size;
	}
	std::memcpy(m_map_stage.get(), bits, size);
	return m_map_stage.get();
}

bool GSTileTargetPool::UploadPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	if (pages.empty())
		return true;

	Slot& s = GetSlot(handle);
	// Whole pages: an upload only ever runs after every GPU-newest block on the page
	// has been pulled back (GSRendererTile::PagesNeedingUpload refuses a page whose
	// truth mask is not full, and the caller spills it first), so CPU is current for
	// the whole page and there is nothing on the texture worth preserving.
	if (CollectRuns(layout, pages, GSVramModel::kFullBlockMask, m_runs) == 0)
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

// Whether the store road below can put this layout's pixels back where they belong.
//
// Both roads end in GSLocalMemory's rect writers, and a writer is bound to a CELL WIDTH: it
// indexes vm32() or vm16() with the pixel index GSOffset produced, and that index counts pixels
// in the LAYOUT'S OWN format. Pair a 32-bit writer with a 16-bit layout and every destination
// byte offset doubles -- the store does not miss by a pixel, it misses by tens of pages, and it
// lands there through the wrapped VM alias so nothing faults and nothing complains. That was a
// real defect, measured on OutRun 2006: a PSMCT16S surface at page 380 zeroing GS pages 293 and
// 297, the game's fog-index plane.
//
// Every family the pool can back now has a writer of its own width: WritePixel32 for the 32-bit
// colour formats, WritePixel16 for Z16/Z16S, and WriteFrame16 (the 5551 pack) for the 16-bit
// colour ones. So the answer is yes for everything -- stated as the two questions rather than as
// `true`, because the property being asserted is "a writer of the layout's own cell width exists",
// and the day a format arrives without one this must go back to saying no.
bool GSTileTargetPool::ReadbackAddressable(const GSTileSurfaceLayout& layout)
{
	const u32 bpp = gsTileStorageBpp(layout.psm);
	return bpp == 32 || bpp == 16;
}

bool GSTileTargetPool::ReadbackPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout,
	const GSPageBitmap& pages, u32 write_mask, u32 block_mask, u32* out_drains)
{
	if (pages.empty() || write_mask == 0 || block_mask == 0)
		return true;

	// A layout whose pixels this road cannot address brings nothing down. Reported as served,
	// not as a failure: the thing it replaces is not a readback that did not happen, it is one
	// that happened at the wrong addresses, so every caller's control flow stays exactly where it
	// was and the pages simply keep the bytes the CPU shadow already holds. Counted, and said
	// once, because "stale" must never be silent.
	if (!ReadbackAddressable(layout))
	{
		m_unaddressable_readbacks++;
		if (!m_warned_unaddressable)
		{
			m_warned_unaddressable = true;
			Console.Warning("Tile: a readback was asked of a %u-bit colour surface (PSM %u), which the store road "
							"cannot address -- its pages keep the CPU shadow's bytes, stale. Counted as "
							"unaddressable readbacks.",
				gsTileStorageBpp(layout.psm), layout.psm);
		}
		return true;
	}

	Slot& s = GetSlot(handle);
	if (CollectRuns(layout, pages, block_mask, m_runs) == 0)
		return true;

	GSVector4i bb = m_runs[0];
	for (const GSVector4i& r : m_runs)
		bb = bb.runion(r);
	pxAssert(bb.z <= s.tex->GetWidth() && bb.w <= s.tex->GetHeight());

	const bool depth16 = (s.kind == GSTileSurfaceKind::Depth) && (layout.psm == PSMZ16 || layout.psm == PSMZ16S);
	const bool color16 = (s.kind == GSTileSurfaceKind::Color) && (gsTileStorageBpp(layout.psm) == 16);
	// The plane window, restated in the cell width the store actually writes. Callers speak in
	// 32-bit cell bytes because that is what the model's planes are; a 16-bit colour cell keeps its
	// colour in bits 0-14 and its alpha in bit 15, so "any of bytes 0-2" is the colour half and
	// "any of byte 3" is the alpha bit. Derived here rather than asked of the caller so nothing
	// outside this file has to know which surfaces are narrow.
	const u16 write_mask16 = static_cast<u16>(((write_mask & 0x00FFFFFFu) ? 0x7FFFu : 0u) |
											  ((write_mask & 0xFF000000u) ? 0x8000u : 0u));
	const GSVector4i drc(0, 0, bb.width(), bb.height());
	const GSOffset off = mem.GetOffset(layout.bp, layout.bw, layout.psm);

	g_gs_device->HintReadbackSource(s.tex);

	// -- The cheap road: the image is COMPLETE on the GPU (nothing recorded since the
	// last submit touches it), so its bytes come back through an out-of-band copy that
	// waits only on itself and never drains the frame's buffer. Depth comes back RAW —
	// its D32F texels — and is converted here exactly as the convert shader's
	// depth_to_uint() does: uint(d * 2^32), truncating, then narrowed for 16-bit. The
	// float-to-integer edge is identical because d * 2^32 is exact in float32 (a power
	// of two scale) and both sides truncate.
	{
		std::unique_ptr<GSDownloadTexture>* raw = (s.kind == GSTileSurfaceKind::Color) ? &m_color_download : &m_uint32_download;
		const GSTexture::Format fmt = (s.kind == GSTileSurfaceKind::Color) ? GSTexture::Format::Color : GSTexture::Format::UInt32;
		if (GSConfig.TileOutOfBandReadback && PrepareDownload(drc.z, drc.w, fmt, raw) && raw->get() &&
			raw->get()->CopyFromCompletedTexture(drc, s.tex, bb, 0, true))
		{
			m_oob_copies++;
			if (!raw->get()->Map(drc))
				return false;
			const u32 pitch = raw->get()->GetMapPitch();
			const u8* bits = StageUncachedMap(raw->get(), raw->get()->GetMapPointer(), pitch, drc.w);
			if (s.kind == GSTileSurfaceKind::Color)
			{
				// The image is RGBA8 whatever the guest format is, so a 16-bit surface's cells are
				// packed to 5551 on the way down -- GSLocalMemory's own WriteFrame16 arithmetic,
				// which is also what tilegpu_writeback.glsl performs on the GPU side. The two must
				// agree bit for bit or a page reconciled by one road and read through the other
				// changes colour.
				for (const GSVector4i& r : m_runs)
				{
					u8* const row = const_cast<u8*>(bits) + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u32);
					if (color16)
						mem.WriteFrame16(row, pitch, off, r, write_mask16);
					else
						mem.WritePixel32(row, pitch, off, r, write_mask);
				}
			}
			else
			{
				// Convert run by run into scratch, then store through the same writers the
				// shader road uses, so the two roads share every byte of the swizzled store.
				for (const GSVector4i& r : m_runs)
				{
					const int w = r.width(), h = r.height();
					const u32 spitch = static_cast<u32>(w) * (depth16 ? sizeof(u16) : sizeof(u32));
					u8* conv = GetScratch(spitch * h);
					for (int y = 0; y < h; y++)
					{
						const float* srow = reinterpret_cast<const float*>(bits + (r.y - bb.y + y) * pitch + (r.x - bb.x) * sizeof(u32));
						if (depth16)
						{
							u16* drow = reinterpret_cast<u16*>(conv + y * spitch);
							for (int x = 0; x < w; x++)
								drow[x] = static_cast<u16>(GSTileTargetPool::DepthToUint(srow[x]));
						}
						else
						{
							u32* drow = reinterpret_cast<u32*>(conv + y * spitch);
							for (int x = 0; x < w; x++)
								drow[x] = GSTileTargetPool::DepthToUint(srow[x]);
						}
					}
					if (depth16)
						mem.WritePixel16(conv, spitch, off, r);
					else
						mem.WritePixel32(conv, spitch, off, r, write_mask);
				}
			}
			raw->get()->Unmap();
			return true;
		}
	}

	// -- The drain road: something recorded since the last submit touches the image, so
	// the copy has to go behind it, in the frame's buffer, and wait for all of it.
	std::unique_ptr<GSDownloadTexture>* dltex;
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

	// The stall. The copy was recorded into the CURRENT command buffer moments ago, so
	// the download texture's "already complete, do nothing" early-out can never fire
	// here and this is unconditionally a submit-and-wait — one full GPU drain per call.
	if (out_drains)
		(*out_drains)++;
	// Timed as well as counted: a drain's PRICE varies with the GPU backlog it
	// waits behind, so the count alone under-informs any A/B where the arms
	// queue different amounts of GPU work between drains.
	const u64 drain_t0 = Common::Timer::GetCurrentValue();
	dltex->get()->Flush();
	if (!dltex->get()->Map(drc))
		return false;
	m_drain_wall_ns += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - drain_t0);

	const u32 pitch = dltex->get()->GetMapPitch();
	u8* bits = const_cast<u8*>(StageUncachedMap(dltex->get(), dltex->get()->GetMapPointer(), pitch, drc.w));

	for (const GSVector4i& r : m_runs)
	{
		if (depth16)
		{
			mem.WritePixel16(bits + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u16), pitch, off, r);
		}
		else if (color16)
		{
			mem.WriteFrame16(bits + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u32), pitch, off, r, write_mask16);
		}
		else
		{
			mem.WritePixel32(bits + (r.y - bb.y) * pitch + (r.x - bb.x) * sizeof(u32), pitch, off, r, write_mask);
		}
	}

	dltex->get()->Unmap();
	return true;
}
