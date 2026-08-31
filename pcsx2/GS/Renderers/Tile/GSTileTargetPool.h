// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"
#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/Tile/GSVramModel.h" // kFullBlockMask: the pool moves what the model tracks

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
// on BOTH axes the model tracks so bytes whose newest data lives elsewhere are never
// clobbered: `write_mask` selects the bytes of a cell (the plane window) and
// `block_mask` selects which of a page's 32 physical blocks travel (the sidecar's
// truth, for a page a CPU transfer has partially overwritten). Everything is
// synchronous with the draw stream: M2 is sequential passes, correctness first.
class GSTileTargetPool
{
public:
	GSTileTargetPool();
	~GSTileTargetPool();

	/// Ask every future COLOUR surface for the extra usage a pass needs to read it while it is the
	/// attachment (an input attachment on Vulkan). Set once, before the first Allocate, by a renderer
	/// whose device serves the in-pass destination read -- a usage bit cannot be added to an image
	/// after it exists, so this cannot be discovered later. Leaves depth alone: this road is colour
	/// only.
	void SetColorSelfReadable(bool enable) { m_color_self_readable = enable; }

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

	/// Read pages back into CPU local memory. write_mask selects the bytes of each 32-bit cell to
	/// write, in the model's own plane units whatever the layout's width: a 16-bit COLOUR layout
	/// narrows it internally (bytes 0-2 are the fifteen colour bits, byte 3 the alpha bit), and
	/// 16-bit DEPTH writes whole cells and ignores it. block_mask selects which of each page's 32
	/// physical blocks are pulled, so a page whose truth a CPU transfer has already shrunk keeps
	/// its CPU-newest blocks.
	///
	/// out_drains, when given, is incremented once per GPU stall this call actually
	/// pays. That is the unit the readback bill is denominated in — a stall is a full
	/// GPU drain (~0.28 ms on M2, ~0.24 ms on SD865) whether it carries one page or a
	/// hundred — and it is NOT derivable from the call count, because a call whose
	/// pages collect to no runs returns without touching the device at all.
	bool ReadbackPages(GSLocalMemory& mem, u32 handle, const GSTileSurfaceLayout& layout, const GSPageBitmap& pages,
		u32 write_mask, u32 block_mask = GSVramModel::kFullBlockMask, u32* out_drains = nullptr);

	/// Whether ReadbackPages' store road can address `layout` at all -- see the definition.
	static bool ReadbackAddressable(const GSTileSurfaceLayout& layout);

	/// A caller's 32-bit-cell plane mask restated in the cell width a 16-bit COLOUR surface
	/// actually stores: bits 0-14 are the colour, bit 15 the alpha. Callers speak in 32-bit
	/// cell bytes because that is what the memory model's planes are, so the narrowing lives
	/// here rather than at every call site.
	///
	/// ⚠️ It DISTRIBUTES OVER OR -- Narrow(a | b) == Narrow(a) | Narrow(b) -- and a caller that
	/// merges several plane masks into one readback relies on that: one call with the union
	/// must write exactly the bytes the separate calls would have. Pinned in
	/// gs_tilegpu_readback_store_tests.cpp, because it is not obvious from the expression and
	/// it would stop being true the moment the narrowing gained a case that is not a
	/// per-byte-group test.
	static u16 NarrowWriteMaskTo16(u32 write_mask)
	{
		return static_cast<u16>(
			((write_mask & 0x00FFFFFFu) ? 0x7FFFu : 0u) | ((write_mask & 0xFF000000u) ? 0x8000u : 0u));
	}

	/// Pixel rects covering the set pages, refined to `block_mask`'s blocks within
	/// each page (kFullBlockMask takes the whole-page run path). Pure: derived from
	/// GSOffset's own shifts, so the unit suite runs it with no device and no
	/// GSLocalMemory instance.
	static u32 CollectRuns(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages, u32 block_mask,
		std::vector<GSVector4i>& runs);

	/// Inverse of the format's block swizzle: index[row * cols + col] is the physical
	/// in-page block number sitting at that block cell.
	struct BlockGrid
	{
		GSVector2i bs; ///< block size in pixels
		int cols;
		int rows;
		u8 index[GSVramModel::kBlocksPerPage];
	};
	static BlockGrid BlockGridFor(const GSTileSurfaceLayout& layout);

	/// The convert shader's depth_to_uint(), on the CPU: uint(d * 2^32), truncating and
	/// saturating. The two roads of ReadbackPages must agree byte for byte, so this is
	/// the one definition both are held to (unit-pinned).
	static u32 DepthToUint(float d)
	{
		const double v = static_cast<double>(d) * 4294967296.0;
		return v <= 0.0 ? 0u : (v >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<u32>(v));
	}

	/// Readbacks refused because ReadbackAddressable said no: their pages kept the CPU
	/// shadow's bytes instead of the surface's. Nonzero means a title is running a road
	/// this renderer does not have yet, not that anything went wrong at this call.
	u32 UnaddressableReadbacks() const { return m_unaddressable_readbacks; }
	/// Readbacks served by the out-of-band copy (no drain of the frame's buffer).
	u32 OutOfBandCopies() const { return m_oob_copies; }
	/// Readbacks that took the drain road. Counted here as well as through the caller's
	/// optional `out_drains`, because a caller that does not pass one (TileGpu does not)
	/// otherwise leaves half the wait bill unattributed.
	u32 Drains() const { return m_drains; }
	/// Wall time spent inside drain-road submit-and-waits. Beside the drain COUNT
	/// because a drain's price varies with the GPU backlog it lands behind.
	u64 DrainWallNs() const { return m_drain_wall_ns; }
	/// What the readbacks actually BROUGHT DOWN, as opposed to what they were asked for.
	/// Counted at the one point both roads share -- after the runs are collected and the
	/// download rectangle is known -- so a call that reached the device for nothing (no
	/// runs, an unaddressable layout, an empty mask) adds nothing to either.
	///
	/// Texels rather than bytes because the download is four bytes a texel on both roads
	/// (RGBA8 colour, UInt32 depth) whatever the guest format is, and the number that
	/// answers "how wide was this pull" is the rectangle, not the guest cells it stores
	/// into. Pages beside it because the rectangle is the union bounding box of the runs,
	/// so a scattered page set fetches far more texels than its pages hold.
	u64 ReadbackTexels() const { return m_readback_texels; }
	u64 ReadbackPageCount() const { return m_readback_pages; }

private:
	u32 m_unaddressable_readbacks = 0;
	bool m_warned_unaddressable = false;

	struct Slot
	{
		GSTexture* tex = nullptr;
		GSTileSurfaceKind kind = GSTileSurfaceKind::Color;
		bool alive = false;
	};

	Slot& GetSlot(u32 handle);
	const Slot& GetSlot(u32 handle) const;

	bool PrepareDownload(u32 width, u32 height, GSTexture::Format format, std::unique_ptr<GSDownloadTexture>* tex);
	u8* GetScratch(u32 size);
	/// The readback consumers below read the mapped download buffer with scalar,
	/// swizzle-ordered loops. Over UNCACHED download memory (the Mali
	/// coherent-readback workaround) every such read is a full memory round trip —
	/// measured at 84% of the GS thread on Mali-G615, 11.8× Classic on SotC — so
	/// when the map reports uncached this bulk-copies the rectangle into cached
	/// staging (one streaming pass, the pattern uncached memory serves well) and
	/// the consumers run out of the staging. Cached maps pass through untouched.
	/// Separate from GetScratch: the depth road uses that per run while this holds
	/// the whole rectangle.
	const u8* StageUncachedMap(const GSDownloadTexture* dl, const u8* bits, u32 pitch, int rows);

	/// One place both creation sites (Allocate, and EnsureHeight's grow) ask, so a grown surface
	/// cannot come back without the usage the original had.
	GSTexture* CreateColorSurface(int width_px, int height_px) const;

	std::vector<Slot> m_slots; // handle - 1 indexed
	std::vector<u32> m_free_handles;
	bool m_color_self_readable = false;
	std::vector<GSVector4i> m_runs; // scratch for CollectRuns
	std::unique_ptr<GSDownloadTexture> m_color_download;
	std::unique_ptr<GSDownloadTexture> m_uint32_download;
	std::unique_ptr<GSDownloadTexture> m_uint16_download;
	std::unique_ptr<u8[]> m_scratch;
	u32 m_scratch_size = 0;
	std::unique_ptr<u8[]> m_map_stage;
	u32 m_map_stage_size = 0;
	u32 m_oob_copies = 0;
	u32 m_drains = 0;
	u64 m_drain_wall_ns = 0;
	u64 m_readback_texels = 0;
	u64 m_readback_pages = 0;
};
