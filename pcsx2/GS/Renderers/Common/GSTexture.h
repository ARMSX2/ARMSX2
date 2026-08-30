// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/EnumOps.h"
#include "GS/GSVector.h"

#include <string>
#include <string_view>

enum class TextureUsage : u8
{
	Texture      = 0,
	DepthStencil = 1,
	RenderTarget = 2,
	Feedback     = 4,
	ShaderWrite  = 8,
};

MARK_ENUM_AS_FLAGS(TextureUsage);

class GSTexture
{
public:
	struct GSMap
	{
		u8* bits;
		int pitch;
	};

	using Usage = TextureUsage;

	// Flags that shouldn't be used alone
	static constexpr Usage ShaderWrite = Usage::ShaderWrite;
	static constexpr Usage Feedback = Usage::Feedback;
	static constexpr Usage FeedbackOrShaderWrite = Usage::Feedback | Usage::ShaderWrite;

	// All valid combinations
	static constexpr Usage Texture = Usage::Texture;
	static constexpr Usage RenderTarget = Usage::RenderTarget;
	static constexpr Usage DepthStencil = Usage::DepthStencil;
	static constexpr Usage FeedbackTarget = Usage::RenderTarget | Usage::Feedback;
	static constexpr Usage FeedbackDepth = Usage::DepthStencil | Usage::Feedback;
	static constexpr Usage ShaderWriteTarget = Usage::RenderTarget | Usage::Feedback | Usage::ShaderWrite;
	static constexpr Usage ShaderWriteTexture = Usage::Texture | Usage::ShaderWrite;

	enum class Format : u8
	{
		Invalid = 0,  ///< Used for initialization
		Color,        ///< Standard (RGBA8) color texture (used to store most of PS2's textures)
		ColorHQ,      ///< High quality (RGB10A2) color texture (no proper alpha)
		ColorHDR,     ///< High dynamic range (RGBA16F) color texture
		ColorClip,    ///< Color texture with more bits for colclip (wrap) emulation, given that blending requires 9bpc (RGBA16Unorm)
		DepthStencil, ///< Depth stencil texture
		DepthColor,   ///< For treating depth texture as RT
		UNorm8,       ///< A8UNorm texture for paletted textures and the OSD font
		UInt16,       ///< UInt16 texture for reading back 16-bit depth
		UInt32,       ///< UInt32 texture for reading back 24 and 32-bit depth
		PrimID,       ///< Prim ID tracking texture for date emulation
		BC1,          ///< BC1, aka DXT1 compressed texture for replacements
		BC2,          ///< BC2, aka DXT2/3 compressed texture for replacements
		BC3,          ///< BC3, aka DXT4/5 compressed texture for replacements
		BC7,          ///< BC7, aka BPTC compressed texture for replacements
		ASTC4x4,      ///< ASTC LDR 4x4 block footprint for replacements
		ASTC5x4,      ///< ASTC LDR 5x4 block footprint for replacements
		ASTC5x5,      ///< ASTC LDR 5x5 block footprint for replacements
		ASTC6x5,      ///< ASTC LDR 6x5 block footprint for replacements
		ASTC6x6,      ///< ASTC LDR 6x6 block footprint for replacements
		ASTC8x5,      ///< ASTC LDR 8x5 block footprint for replacements
		ASTC8x6,      ///< ASTC LDR 8x6 block footprint for replacements
		ASTC8x8,      ///< ASTC LDR 8x8 block footprint for replacements
		ASTC10x5,     ///< ASTC LDR 10x5 block footprint for replacements
		ASTC10x6,     ///< ASTC LDR 10x6 block footprint for replacements
		ASTC10x8,     ///< ASTC LDR 10x8 block footprint for replacements
		ASTC10x10,    ///< ASTC LDR 10x10 block footprint for replacements
		ASTC12x10,    ///< ASTC LDR 12x10 block footprint for replacements
		ASTC12x12,    ///< ASTC LDR 12x12 block footprint for replacements
		Last = ASTC12x12,
	};

	static bool ValidateUsageAndFormat(Usage usage, Format format);

	/// Compressed block descriptor. ASTC footprints are rectangular, so one scalar block
	/// size cannot describe them: pitch, row count, and transfer geometry all need both
	/// dimensions plus the bytes-per-block separately. Uncompressed formats report a
	/// degenerate {1, 1, bytes_per_texel} block.
	struct BlockInfo
	{
		u32 width;
		u32 height;
		u32 bytes;
	};

	static BlockInfo GetBlockInfo(Format format);

	enum class State : u8
	{
		Dirty,
		Cleared,
		Invalidated
	};

	union ClearValue
	{
		u32 color;
		float depth;
	};

protected:
	GSVector2i m_size{};
	int m_mipmap_levels = 0;
	Usage m_usage = Usage::Texture;
	Format m_format = Format::Invalid;
	State m_state = State::Dirty;

	// frame number (arbitrary base) the texture was recycled on
	// different purpose than texture cache ages, do not attempt to merge
	u32 m_last_frame_used = 0;

	bool m_needs_mipmaps_generated = true;
	ClearValue m_clear_value = {};

#ifdef PCSX2_DEVBUILD
	std::string m_debug_name;
#endif

	virtual bool DoUpdate(const GSVector4i& r, const void* data, int pitch, int layer = 0) = 0;
	virtual bool DoMap(GSMap& m, const GSVector4i* r, int layer) = 0;

	/// The scheduler is the one caller that rewrites clear state while its own draws are
	/// queued — it hides a pending clear at enqueue so the deferred draws see the target
	/// as already-cleared, and restores it at emit. Everything else goes through SetState
	/// and trips the tripwire.
	friend class GSPassScheduler;
	__fi void SetStateForDeferral(State state) { m_state = state; }

public:
	GSTexture();
	virtual ~GSTexture();

	// Returns the native handle of a texture.
	virtual void* GetNativeHandle() const = 0;

	/// Uploads CPU data into the texture. Flushes any deferred draws first: a draw held
	/// back by GSPassScheduler must not be reordered past an upload into a texture it
	/// writes, nor past one into a texture it samples.
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0);

	/// Maps the texture for a CPU upload. Flushes deferred draws for the same reason
	/// Update() does — the write lands as soon as the caller fills the mapping, so a
	/// queued draw against this texture must not be reordered past it.
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0);
	virtual void Unmap() = 0;
	virtual void GenerateMipmap() = 0;

#ifdef PCSX2_DEVBUILD
	virtual void SetDebugName(std::string_view name) = 0;
	const std::string& GetDebugName() { return m_debug_name; }
#endif

	bool Save(const std::string& fn);

	__fi int GetWidth() const { return m_size.x; }
	__fi int GetHeight() const { return m_size.y; }
	__fi const GSVector2i& GetSize() const { return m_size; }
	__fi GSVector4i GetRect() const { return GSVector4i::loadh(m_size); }

	__fi int GetMipmapLevels() const { return m_mipmap_levels; }
	__fi bool IsMipmap() const { return m_mipmap_levels > 1; }

	__fi Usage GetUsage() const { return m_usage; }
	__fi Format GetFormat() const { return m_format; }
	__fi bool IsCompressedFormat() const { return IsCompressedFormat(m_format); }

	static const char* GetFormatName(Format format);
	static bool IsBlockCompressedFormat(Format format);
	static bool IsASTCFormat(Format format);
	static u32 GetCompressedBytesPerBlock(Format format);
	static u32 GetCompressedBlockSize(Format format);
	static u32 CalcUploadPitch(Format format, u32 width);
	static u32 CalcUploadRowLengthFromPitch(Format format, u32 pitch);
	static u32 CalcUploadSize(Format format, u32 height, u32 pitch);
	static bool IsFeedbackFormat(Format format);
	static bool IsShaderWriteFormat(Format format);

	u32 GetCompressedBytesPerBlock() const;
	u32 GetCompressedBlockSize() const;
	u32 CalcUploadPitch(u32 width) const;
	u32 CalcUploadRowLengthFromPitch(u32 pitch) const;
	u32 CalcUploadSize(u32 height, u32 pitch) const;

	static __fi bool IsRenderTarget(Usage usage)
	{
		return (usage & RenderTarget);
	}
	static __fi bool IsDepthStencil(Usage usage)
	{
		return (usage & DepthStencil);
	}
	static __fi bool IsRenderTargetOrDepthStencil(Usage usage)
	{
		return IsRenderTarget(usage) || IsDepthStencil(usage);
	}
	static __fi bool IsDepthColor(Usage usage, Format format)
	{
		return IsRenderTarget(usage) && (format == Format::DepthColor);
	}
	static __fi bool IsTexture(Usage usage)
	{
		return usage == Texture;
	}
	static __fi bool IsDepthLike(Usage usage, Format format)
	{
		return IsDepthStencil(usage) || IsDepthColor(usage, format);
	}
	static __fi bool IsFeedback(Usage usage)
	{
		return (usage & Feedback);
	}
	static __fi bool IsShaderWrite(Usage usage)
	{
		return (usage & ShaderWrite);
	}
	static __fi bool IsFeedbackOrShaderWrite(Usage usage)
	{
		return IsFeedback(usage) || IsShaderWrite(usage);
	}


	__fi bool IsRenderTargetOrDepthStencil() const
	{
		return IsRenderTargetOrDepthStencil(m_usage);
	}
	__fi bool IsRenderTarget() const
	{
		return IsRenderTarget(m_usage);
	}
	__fi bool IsDepthStencil() const
	{
		return IsDepthStencil(m_usage);
	}
	__fi bool IsDepthColor() const
	{
		return IsDepthColor(m_usage, m_format);
	}
	__fi bool IsTexture() const
	{
		return IsTexture(m_usage);
	}
	__fi bool IsDepthLike() const
	{
		return IsDepthLike(m_usage, m_format);
	}
	__fi bool IsFeedback() const
	{
		return IsFeedback(m_usage);
	}
	__fi bool IsShaderWrite() const
	{
		return IsShaderWrite(m_usage);
	}
	__fi bool IsFeedbackOrShaderWrite() const
	{
		return IsFeedbackOrShaderWrite(m_usage);
	}

	virtual bool IsShaderWriteMode() const
	{
		// Backends that track state explicitly can specialize this to use the actual state.
		return IsShaderWrite();
	}

	__fi State GetState() const { return m_state; }
	__fi void SetState(State state)
	{
		AssertNoQueuedObserver("SetState on a texture with deferred draws queued against it");
		m_state = state;
	}

	__fi u32 GetLastFrameUsed() const { return m_last_frame_used; }
	void SetLastFrameUsed(u32 frame) { m_last_frame_used = frame; }

	__fi u32 GetClearColor() const { return m_clear_value.color; }
	__fi float GetClearDepth() const { return m_clear_value.depth; }
	__fi GSVector4 GetUNormClearColor() const { return GSVector4::unorm8(m_clear_value.color); }
	__fi GSVector4 GetClearForFormat() const
	{
		return IsDepthLike() ? GSVector4(m_clear_value.depth, 0.0f, 0.0f, 0.0f) : GetUNormClearColor();
	}

	__fi void SetClearColor(u32 color)
	{
		AssertNoQueuedObserver("SetClearColor on a texture with deferred draws queued against it");
		m_state = State::Cleared;
		m_clear_value.color = color;
	}
	__fi void SetClearDepth(float depth)
	{
		AssertNoQueuedObserver("SetClearDepth on a texture with deferred draws queued against it");
		m_state = State::Cleared;
		m_clear_value.depth = depth;
	}

	void GenerateMipmapsIfNeeded();
	void ClearMipmapGenerationFlag() { m_needs_mipmaps_generated = false; }

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)
	/// Tripwire for the render-pass scheduler, and the counterpart to the flush wrappers
	/// on GSDevice. Those guard what a deferred draw can be *reordered past*; this guards
	/// what a deferred draw can be *made to lie about*.
	///
	/// A queued draw has not run yet, but m_state says whether the target still owes a
	/// clear. Rewriting it behind the scheduler's back is invisible in a frame hash right
	/// up until the clear lands on the wrong side of the draw — which is how this class
	/// has bitten twice: m_state going stale during deferral, and Recycle() parking a
	/// texture a queued draw still named. Deliberately in Common rather than a backend:
	/// this state is backend-agnostic, and the VK-layout-transition placement was tried
	/// first and caught nothing, because none of these paths touch the GPU at all.
	///
	/// Out of line because GSTexture.h cannot include GSDevice.h.
	void AssertNoQueuedObserver(const char* what) const;
#else
	__fi void AssertNoQueuedObserver(const char*) const {}
#endif

	// Typical size of a RGBA texture
	u32 GetMemUsage() const { return m_size.x * m_size.y * (m_format == Format::UNorm8 ? 1 : 4); }

	// Helper routines for formats/types
	static bool IsCompressedFormat(Format format)
	{
		return (format >= Format::BC1 && format <= Format::BC7) || IsASTCFormat(format);
	}
};

class GSDownloadTexture
{
protected:
	virtual void DoCopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) = 0;

public:
	GSDownloadTexture(u32 width, u32 height, GSTexture::Format format);
	virtual ~GSDownloadTexture();

	/// Basically, this has dimensions only because of DX11.
	__fi u32 GetWidth() const { return m_width; }
	__fi u32 GetHeight() const { return m_height; }
	__fi GSTexture::Format GetFormat() const { return m_format; }
	__fi bool NeedsFlush() const { return m_needs_flush; }
	__fi bool IsMapped() const { return (m_map_pointer != nullptr); }
	__fi const u8* GetMapPointer() const { return m_map_pointer; }
	__fi u32 GetMapPitch() const { return m_current_pitch; }

	/// True when the mapped memory is NOT host-cached — the Mali coherent-readback
	/// workaround prefers coherent (uncached) download memory, and some pools have
	/// no cached type at all. Scalar reads of such a map are each a full memory
	/// round trip (measured 84% of the GS thread inside the Tile readback's
	/// swizzle loops on Mali-G615); a consumer that reads the map any way other
	/// than one streaming pass should bulk-copy the rectangle into cached scratch
	/// first, because a streaming copy is the one access pattern uncached memory
	/// serves well. Backends that cannot know report false (cached), which keeps
	/// consumers on the direct path.
	__fi bool IsMapUncached() const { return m_map_uncached; }

	/// Calculates the pitch of a transfer.
	u32 GetTransferPitch(u32 width, u32 pitch_align) const;

	/// Calculates the size of the data you should transfer.
	void GetTransferSize(const GSVector4i& rc, u32* copy_offset, u32* copy_size, u32* copy_rows) const;

	/// Queues a copy from the specified texture to this buffer.
	/// Does not complete immediately, you should flush before accessing the buffer.
	/// use_transfer_pitch should be true if there's only a single texture being copied to this buffer before
	/// it will be used. This allows the image to be packed tighter together, and buffer reuse.
	/// Flushes any deferred draws first, so a readback always sees every draw that had been
	/// submitted when it was issued.
	void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch = true);

	/// Copies from a texture whose GPU-side contents are already COMPLETE -- every command
	/// that touched it has been submitted -- without recording into, submitting, or waiting
	/// on the command buffer being recorded. The copy runs out of band, on its own
	/// submission, and waits only for itself, so on return the buffer is ready to Map()
	/// with no Flush() needed. Returns false when the backend has no such path or the
	/// texture is NOT complete (something touching it is still in the recording buffer),
	/// in which case nothing was done and the caller takes CopyFromTexture() + Flush().
	///
	/// This is the price difference the three-state page model is built on: a readback of
	/// pages the GPU finished with long ago costs a copy, not a drain of every draw
	/// recorded since. Depth sources are copied RAW (their D32F texels) -- the caller
	/// converts, matching the convert shader's depth_to_uint().
	virtual bool CopyFromCompletedTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
	{
		return false;
	}

	/// Maps the texture into the CPU address space, enabling it to read the contents.
	/// The Map call may not perform synchronization. If the contents of the staging texture
	/// has been updated by a CopyFromTexture() call, you must call Flush() first.
	/// If persistent mapping is supported in the backend, this may be a no-op.
	virtual bool Map(const GSVector4i& read_rc) = 0;

	/// Unmaps the CPU-readable copy of the texture. May be a no-op on backends which
	/// support persistent-mapped buffers.
	virtual void Unmap() = 0;

	/// Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
	/// This may cause a command buffer submit depending on if one has occurred between the last
	/// call to CopyFromTexture() and the Flush() call.
	virtual void Flush() = 0;

	/// Checks whether a queued GPU download has completed, WITHOUT waiting on it.
	/// Returns true when Map() can be called without blocking. Backends which cannot test
	/// completion non-blockingly keep it pending until a Flush() has happened.
	virtual bool Poll() { return !m_needs_flush; }

#ifdef PCSX2_DEVBUILD
	/// Sets object name that will be displayed in graphics debuggers.
	virtual void SetDebugName(std::string_view name) = 0;
#endif

	/// Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
	/// (length in bytes of each row). CopyFromTexture() must be called first. The contents of any
	/// texels outside of the rectangle used for CopyFromTexture is undefined.
	bool ReadTexels(const GSVector4i& rc, void* out_ptr, u32 out_stride);

	/// Returns what the size of the specified texture would be, in bytes.
	static u32 GetBufferSize(u32 width, u32 height, GSTexture::Format format, u32 pitch_align = 1);

protected:
	u32 m_width;
	u32 m_height;
	GSTexture::Format m_format;

	const u8* m_map_pointer = nullptr;
	u32 m_current_pitch = 0;

	bool m_needs_flush = false;
	bool m_map_uncached = false; ///< see IsMapUncached(); set by backends that know their allocation
};
