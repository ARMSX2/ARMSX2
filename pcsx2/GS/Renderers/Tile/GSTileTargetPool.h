// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <memory>
#include <vector>

class GSDownloadTexture;
class GSLocalMemory;

// GPU texture backing for the interval memory model's surfaces. Device-side and
// deliberately thin: the model decides WHAT moves, this class only moves it.
//
// A surface texture is the layout's pixel space laid out linearly from its base
// page: width is always the stride (bw * 64 — every M2 format has 64-pixel-wide
// pages), row r of pages occupies pixel rows [r*pageH, (r+1)*pageH), and page
// (base + r*bw + c) sits at pixel rect (c*64, r*pageH). That makes the guest pixel
// rect and the texture rect the same rect, so the swizzle machinery's rect-taking
// readers/writers apply unchanged.
//
// Uploads deswizzle whole pages out of CPU local memory through the house staging +
// convert-StretchRect recipe (GSTextureCache::Target::Update is the donor): color
// converts with COPY, depth with the RGBA8_TO_DEPTH family — for Z24 the masked
// 24-bit integer is exact in float32, which is precisely why Z32 is outside the M2
// native envelope. Readbacks are the exact inverse of GSTextureCache::Read, masked
// to the byte set the caller owns so bytes whose newest data lives elsewhere are
// never clobbered. Everything is synchronous with the draw stream: M2 is sequential
// passes, correctness first.
class GSTileTargetPool
{
public:
	GSTileTargetPool();
	~GSTileTargetPool();

	struct Geometry
	{
		int ppr; ///< pages per page-row (== bw for every M2-native format)
		GSVector2i pgs; ///< page size in pixels
	};
	static Geometry GeometryFor(const GSTileSurfaceLayout& layout);

	/// Pixel height needed so rows [0, max page row of `pages`] are resident.
	static int HeightForPages(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages);

	/// Returns a handle (never 0) or 0 on allocation failure.
	u32 Allocate(const GSTileSurfaceLayout& layout, int height_px);
	void Release(u32 handle);
	/// Recycles every texture (renderer reset / teardown).
	void ReleaseAll();

	GSTexture* GetTexture(u32 handle) const;
	int GetHeight(u32 handle) const;
	/// Grows the texture to at least height_px, preserving contents. False = OOM.
	bool EnsureHeight(u32 handle, int height_px);

	/// Deswizzle whole pages out of CPU local memory into the surface texture.
	bool UploadPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages);

	/// Read whole pages back into CPU local memory. write_mask selects the bytes of
	/// each 32-bit cell to write (16-bit formats write whole cells and ignore it).
	bool ReadbackPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, u32 write_mask);

private:
	struct Slot
	{
		GSTexture* tex = nullptr;
		GSTileSurfaceKind kind = GSTileSurfaceKind::Color;
		bool alive = false;
	};

	Slot& GetSlot(u32 handle);
	const Slot& GetSlot(u32 handle) const;

	/// Consecutive same-page-row runs of the set pages, as pixel rects.
	static u32 CollectRuns(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, std::vector<GSVector4i>& runs);

	bool PrepareDownload(u32 width, u32 height, GSTexture::Format format, std::unique_ptr<GSDownloadTexture>* tex);
	u8* GetScratch(u32 size);

	std::vector<Slot> m_slots; // handle - 1 indexed
	std::vector<u32> m_free_handles;
	std::vector<GSVector4i> m_runs; // scratch for CollectRuns
	std::unique_ptr<GSDownloadTexture> m_color_download;
	std::unique_ptr<GSDownloadTexture> m_uint32_download;
	std::unique_ptr<GSDownloadTexture> m_uint16_download;
	std::unique_ptr<u8[]> m_scratch;
	u32 m_scratch_size = 0;
};
