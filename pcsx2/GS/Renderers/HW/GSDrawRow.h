// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSSpriteEdgeSnap.h"

#include <bit>
#include <cstddef>
#include <memory>
#include <vector>

/// Rows and the state table.
///
/// DrawPrims' state derivation is a function of the GS registers, the device and settings, and a set of facts about
/// the draw, its target and its source. A state entry is what one derivation produced: the configuration block
/// ResetStates clears, the two constant buffers, and what it did to the renderer outside the configuration. Entries
/// are found in two steps:
///
///   1. the register state (RegKey): register fields masked to what the derivation reads, compared as bytes. It
///      usually equals the last draw's or the one before, and is otherwise found by hash.
///   2. one of that register state's variants (VarKey): the facts about the draw, the target and the source, and the
///      run context (target scale and size). The variant that matched last is tried first.
///
/// A row is one game draw continuing a run of draws into one target. GSDrawRow and GSDrawFacts are the records the
/// row writer and the vertex writer exchange; see the comments on each.
///
/// Header-only apart from the table's slow paths, so the tests can pin it without a device.
namespace GSDrawRows
{
	/// gsrunner scaffolding. Off: every draw derives. On: a draw whose state is in the table takes the entry. Check:
	/// every such draw derives as well and the two are compared byte for byte.
	enum class Mode : u8
	{
		Off,
		On,
		Check,
	};

	/// Register fields the derivation reads, masked, as one block compared whole.
	struct alignas(16) RegKey
	{
		static constexpr u32 WORDS = 10;
		u64 w[WORDS];

		__fi bool operator==(const RegKey& r) const
		{
			u64 d = 0;
			for (u32 i = 0; i < WORDS; i++)
				d |= w[i] ^ r.w[i];
			return d == 0;
		}
		__fi bool operator!=(const RegKey& r) const { return !(*this == r); }

		/// Two words folded and multiplied per step.
		__fi u32 Hash() const
		{
			u64 h = 0;
			for (u32 i = 0; i < WORDS; i += 2)
				h = (h ^ w[i] ^ std::rotr(w[i + 1], 32)) * 0x9E3779B97F4A7C15ull;
			return static_cast<u32>(h >> 32);
		}
	};

	/// The facts that vary inside one register state, compared by value.
	///   0  draw facts: primitive class, vertex colour and depth classes, overlap, coverage, half-pixel answers
	///   1  alpha values: the draw's range, the target's range before and after, the range the blend sees
	///   2  target scale and its offset hack
	///   3  target size, scaled and unscaled
	///   4  mipmap levels and the source's scale
	///   5  the source's kind and format
	///   6  the source's region
	///   7  the source's unscaled size
	///
	/// `values` holds the alpha bounds' values (AlphaValues). A variant compares them only where its derivation read a
	/// value (the variant's care mask); everywhere else the bounds' classes in word 1 decide.
	struct alignas(16) VarKey
	{
		static constexpr u32 WORDS = 8;
		u64 w[WORDS];
		u64 values;

		/// Equal facts, and equal alpha values where `care` says the derivation read them.
		__fi bool Matches(const VarKey& r, u64 care) const
		{
			u64 d = (values ^ r.values) & care;
			for (u32 i = 0; i < WORDS; i++)
				d |= w[i] ^ r.w[i];
			return d == 0;
		}
		__fi bool operator==(const VarKey& r) const { return Matches(r, ~0ull); }
		__fi bool operator!=(const VarKey& r) const { return !(*this == r); }
	};

	/// The alpha-range bounds the derivation reads, which the key holds by class (AlphaBound).
	enum AlphaBoundId : u32
	{
		DRAW_MIN, ///< the draw's alpha range after the alpha test's correction
		DRAW_MAX,
		BLEND_MIN, ///< the range the blend sees (CalculateAlphaRange)
		BLEND_MAX,
		RT_NEW_MAX, ///< the target's maximum after this draw
		ALPHA_BOUNDS
	};

	/// A bound's class: its place among 0 and 128, the only constants the derivation compares a bound with.
	inline constexpr u64 AlphaClass(int v) { return static_cast<u64>((v > 0) + (v >= 128) + (v > 128)); }

	/// The bounds' values in VarKey::values, nine bits each in AlphaBoundId order.
	inline constexpr u64 AlphaValues(int draw_min, int draw_max, int blend_min, int blend_max, int rt_new_max)
	{
		const auto v9 = [](int v) { return static_cast<u64>(static_cast<u32>(v) & 0x1FF); };
		return v9(draw_min) | (v9(draw_max) << 9) | (v9(blend_min) << 18) | (v9(blend_max) << 27) | (v9(rt_new_max) << 36);
	}

	/// The mask of VarKey::values for the bounds in `asked` (one bit per AlphaBoundId).
	inline constexpr u64 AlphaValueCare(u32 asked)
	{
		u64 m = 0;
		for (u32 i = 0; i < ALPHA_BOUNDS; i++)
		{
			if (asked & (1u << i))
				m |= 0x1FFull << (9 * i);
		}
		return m;
	}

	/// A bound of an alpha range as the derivation reads it. The key holds each bound's class and whether each range
	/// is one value, so a comparison with 0 or 128, and the range's OneValue, need nothing more. Any other read, a
	/// comparison with another constant or the value itself, marks the bound in `asked`; the entry the derivation makes
	/// then matches only draws with the same value of that bound. So every read the key cannot answer by class makes the
	/// key hold the value instead: nothing is left to an inventory of the reads.
	class AlphaBound
	{
	public:
		AlphaBound(int v, AlphaBoundId id, u32* asked) : m_v(v), m_bit(1u << id), m_asked(asked) {}

		bool operator==(int k) const { return Ask(k), m_v == k; }
		bool operator!=(int k) const { return Ask(k), m_v != k; }
		bool operator<(int k) const { return Ask(k), m_v < k; }
		bool operator<=(int k) const { return Ask(k), m_v <= k; }
		bool operator>(int k) const { return Ask(k), m_v > k; }
		bool operator>=(int k) const { return Ask(k), m_v >= k; }

		/// The value, which the entry then keys.
		int Value() const
		{
			*m_asked |= m_bit;
			return m_v;
		}
		/// The value for a log message only: recorded nowhere, so it must not decide anything.
		int ForLog() const { return m_v; }

	private:
		friend struct AlphaRange;
		void Ask(int k) const
		{
			if (k != 0 && k != 128)
				*m_asked |= m_bit;
		}

		int m_v;
		u32 m_bit;
		u32* m_asked;
	};

	/// The two bounds of one range; whether they are equal is in the key.
	struct AlphaRange
	{
		AlphaBound min, max;
		bool OneValue() const { return min.m_v == max.m_v; }
	};

	/// Bytes of GSHWDrawConfig that ResetStates clears: the configuration block an entry holds.
	inline constexpr u32 CONF_BYTES = static_cast<u32>(offsetof(GSHWDrawConfig, cb_vs));
	/// Bytes of the two constant buffers.
	inline constexpr u32 CB_BYTES =
		static_cast<u32>(offsetof(GSHWDrawConfig, cb_ps) + sizeof(GSHWDrawConfig::PSConstantBuffer) - offsetof(GSHWDrawConfig, cb_vs));
	inline constexpr u32 CB_WORDS = CB_BYTES / 4;
	static_assert(CONF_BYTES % 16 == 0 && CB_BYTES % 16 == 0, "entries copy whole vectors");

	/// Copies whole vectors, unrolled. Both ends are aligned.
	template <u32 Bytes>
	static __fi void CopyBlock(void* dst, const void* src)
	{
		static_assert(Bytes % 16 == 0);
		u8* d = static_cast<u8*>(dst);
		const u8* s = static_cast<const u8*>(src);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
		for (u32 i = 0; i < Bytes; i += 16)
			GSVector4i::store<true>(d + i, GSVector4i::load<true>(s + i));
	}

	/// What one derivation produced. Read when a draw takes the entry and by check mode.
	struct alignas(16) StateBody
	{
		enum : u16
		{
			NOT_ROWABLE = 1u << 0, ///< the derivation acted on a texture, the vertices or carried state: always derive
			CANCEL = 1u << 1, ///< the draw is dropped before the target's alpha is written
			ABORT = 1u << 2, ///< the draw is dropped after it (the alpha test's ABORT_DRAW)
			BLEND_IDENTITY = 1u << 3,
			RT_ALPHA_SCALE = 1u << 4, ///< the target's alpha is stored scaled after the draw
			SPRITE_EDGE_CLAMP_DRAW = 1u << 5, ///< counts toward GSPerfMon::SpriteEdgeClampDraws
			COLCLIP_FRAME = 1u << 6, ///< colclip_mode is set, and colclip_frame is this draw's FRAME
			OPTIMIZED_BLEND = 1u << 7, ///< the derivation wrote the blend equation the ROV setup reads
		};

		alignas(16) u8 conf[CONF_BYTES];
		/// The constant buffers as the derivation left them, with zero in every word it did not write.
		alignas(16) u8 cb[CB_BYTES];
		/// Which constant words the derivation wrote. A hit writes only those: the others keep what the previous draw
		/// left, as on the derived path, so a draw hands the device the same bytes either way.
		u64 cb_written[2];
		u64 optimized_blend;
		u32 test; ///< TEST as the derivation left it
		u16 flags;
		u8 zmsk; ///< ZBUF.ZMSK as the derivation left it
		u8 colclip_mode; ///< with COLCLIP_FRAME
	};

	static_assert(CB_WORDS <= 128, "cb_written holds one bit per constant word");

	/// The written words of `src` over `dst`: word i is copied when bit i of `mask` is set.
	inline void CopyWrittenWords(void* dst, const void* src, const u64 mask[2])
	{
		u32* d = static_cast<u32*>(dst);
		const u32* s = static_cast<const u32*>(src);
		for (u32 half = 0; half < 2; half++)
		{
			u64 m = mask[half];
			while (m)
			{
				const u32 i = half * 64 + static_cast<u32>(std::countr_zero(m));
				d[i] = s[i];
				m &= m - 1;
			}
		}
	}

	/// A variant of a register state: the facts it was derived for, and its place in the variant list. 80 bytes: the
	/// key and the list link are what a walk reads.
	struct alignas(16) StateHead
	{
		VarKey var;
		u64 care; ///< which bits of var.values the derivation read (AlphaValueCare)
		u16 next; ///< next variant of the same register state, NONE at the end
		u16 reg; ///< the register state it belongs to
		u32 hits; ///< diagnostic
	};
	static_assert(sizeof(StateHead) == 96);

	/// A register state: its key and its variants.
	struct alignas(16) RegState
	{
		RegKey key;
		u32 hash;
		u16 first; ///< first variant, NONE if none
		u16 last_hit; ///< the variant that matched last time, NONE if none
		u64 pad;
	};
	static_assert(sizeof(RegState) == 96);

	/// Every state entry of the renderer, in chunks of CHUNK so that an index stays valid while the table grows.
	/// Flushed whole when full, on a settings change, and on any event the caller names.
	class StateTable
	{
	public:
		static constexpr u16 NONE = 0xFFFF;
		static constexpr u32 CHUNK = 1024;
		static constexpr u32 MAX_CHUNKS = 8;
		static constexpr u32 CAPACITY = CHUNK * MAX_CHUNKS;

		StateTable() = default;
		StateTable(const StateTable&) = delete;
		StateTable& operator=(const StateTable&) = delete;

		/// The register state with this key, or NONE. The last state, then the one before it, then the hash index.
		__fi u16 FindReg(const RegKey& k)
		{
			if (m_last_reg != NONE && Reg(m_last_reg).key == k) [[likely]]
				return m_last_reg;
			if (m_prev_reg != NONE && Reg(m_prev_reg).key == k)
			{
				std::swap(m_last_reg, m_prev_reg);
				return m_last_reg;
			}
			return FindRegSlow(k);
		}

		/// The variant of register state `reg` with these facts, or NONE. The variant that matched last, then the list,
		/// which keeps the variants in the order they last matched: a variant found is moved to its front, so draws
		/// that alternate between a few variants find each within the first few.
		__fi u16 FindVariant(u16 reg, const VarKey& v)
		{
			RegState& rs = Reg(reg);
			if (rs.last_hit != NONE && Head(rs.last_hit).var.Matches(v, Head(rs.last_hit).care)) [[likely]]
				return rs.last_hit;
			u16 prev = NONE;
			for (u16 i = rs.first; i != NONE; prev = i, i = Head(i).next)
			{
				if (Head(i).var.Matches(v, Head(i).care))
				{
					if (prev != NONE)
					{
						Head(prev).next = Head(i).next;
						Head(i).next = rs.first;
						rs.first = i;
					}
					rs.last_hit = i;
					return i;
				}
			}
			return NONE;
		}

		/// A register state for `k`, which must not be in the table. NONE when the table is full: the caller
		/// flushes and asks again.
		u16 InsertReg(const RegKey& k);
		/// A variant of `reg` for `v`, matching draws whose alpha values agree under `care`. It must not be in the table.
		/// Its body is uninitialised. NONE when full.
		u16 InsertVariant(u16 reg, const VarKey& v, u64 care = ~0ull);

		void Flush();

		__fi RegState& Reg(u16 i) { return m_regs[i / CHUNK][i % CHUNK]; }
		__fi StateHead& Head(u16 i) { return m_heads[i / CHUNK][i % CHUNK]; }
		__fi StateBody& Body(u16 i) { return m_bodies[i / CHUNK][i % CHUNK]; }
		__fi GSRowDeviceCache& Device(u16 i) { return m_devices[i / CHUNK][i % CHUNK]; }

		u32 RegCount() const { return m_reg_count; }
		u32 EntryCount() const { return m_entry_count; }
		u32 Chunks() const { return static_cast<u32>(m_heads.size()); }
		u64 Flushes() const { return m_flushes; }

	private:
		u16 FindRegSlow(const RegKey& k);
		void PlaceReg(u16 i);

		u16 m_last_reg = NONE;
		u16 m_prev_reg = NONE;
		u32 m_reg_count = 0;
		u32 m_entry_count = 0;
		u64 m_flushes = 0;
		std::vector<std::unique_ptr<RegState[]>> m_regs;
		std::vector<std::unique_ptr<StateHead[]>> m_heads;
		std::vector<std::unique_ptr<StateBody[]>> m_bodies;
		std::vector<std::unique_ptr<GSRowDeviceCache[]>> m_devices;
		std::vector<u16> m_index; ///< open addressing on the register key's hash, NONE when empty
		u32 m_mask = 0;
	};

	/// The table's counters, for gsrunner.
	struct Stats
	{
		u64 draws = 0; ///< DrawPrims calls that reached the derivation
		u64 eligible = 0; ///< draws whose registers and device allow an entry
		u64 hits = 0;
		u64 misses = 0;
		u64 not_rowable = 0; ///< took an entry marked NOT_ROWABLE, or a fact that always derives
		u64 check_mismatches = 0;
		u64 ineligible[5] = {}; ///< no target, a shuffle, DATE, AA1, other
		u64 prep_mismatches = 0; ///< the frame-mask stage disagreed with what the alpha range was given ahead of it
		u64 pushed = 0; ///< draws keyed with a half-pixel mode-5 vertex push
		u64 hpo_keyed = 0; ///< draws keyed with a half-pixel texture-offset answer
		u64 rows = 0; ///< draws that skipped the target search (or would have, in check mode)
		u64 row_checked = 0; ///< check mode: rows whose full path was compared
		u64 row_mismatches = 0; ///< check mode: rows where the full path changed what the row skips
		u64 entries_value_care = 0; ///< entries whose derivation read an alpha bound's value
		u64 entries_raw_alpha = 0; ///< of those, entries whose derivation read the draw's range bypassing AlphaBound
		u64 device_rows = 0; ///< draws handed to GSDevice::DrawRow
		u64 device_rows_plain = 0; ///< of those, drawn by the backend's plain path
		u64 device_rows_refused[9] = {}; ///< why not: no pass, the draw's roads, targets, target state, textures, feedback bits, kept copy, colour clip, readback window
		u32 reg_states = 0; ///< register states in the table after the last miss
		u32 entries = 0; ///< entries in the table after the last miss
		u64 flushes = 0;
	};

	extern Mode s_mode;
	extern bool s_stats;
	extern Stats stats;

	/// Half-pixel-offset mode 5 ("Native with texture offset") above 1x pushes a textured FST sprite's vertex offset
	/// by (16 - frac1) / 8 * scale times its unpushed value on each axis where the far edge sits in the second half of
	/// a native pixel (GSSpriteEdgeSnap::NativeSpritePushApplies). The unpushed offset is -1 / (width * scale) and the
	/// vertex scale 1 / (8 * width), so the push is the same as adding 16 - frac1 sixteenths to every coordinate on
	/// that axis. Returns that number, 0 when the push does not apply. `p0`, `p1` are the sprite's two coordinates on
	/// the axis with the offset (OFX or OFY) still in, as the vertex holds them.
	inline u32 NativeSpritePush(int p0, int p1, int offset)
	{
		const int frac0 = (p0 - offset) & 0xf;
		const int frac1 = (p1 - offset) & 0xf;
		return GSSpriteEdgeSnap::NativeSpritePushApplies(frac0, frac1, p0 <= p1) ? static_cast<u32>(16 - frac1) : 0u;
	}

	/// The 32-byte record of one game draw that continues a run. CPU memory only.
	struct alignas(16) GSDrawRow
	{
		enum : u8
		{
			TWIN = 1u << 0,
			FULL_COVER = 1u << 1,
			SPRITE = 1u << 2,
			OPENER = 1u << 3,
			DERIVED = 1u << 4, ///< written by a derive-only draw
		};
		static constexpr u8 TEX_NONE = 0xFF;
		static constexpr u8 TEX_SELF = 0xFE;

		u16 state; ///< index of the state entry
		u8 tex; ///< index in the run's texture list, or TEX_NONE, TEX_SELF
		u8 flags;
		u8 a_min; ///< lowest alpha the draw's fragments can write
		u8 a_max; ///< highest
		u8 lod; ///< low 3 bits lowest mip level, next 3 highest; 0 when the entry has no mipmapping
		u8 zero;
		u16 box[4]; ///< x0, y0, x1, y1 in native target pixels, after the scissor
		u32 vfirst; ///< first vertex in the vertex stream
		u32 ifirst; ///< first index in the run's index staging
		u32 nidx; ///< index count
		u32 seq; ///< the draw's sequence number, which its vertices carry in their fog word
	};
	static_assert(sizeof(GSDrawRow) == 32);
	static_assert(offsetof(GSDrawRow, box) == 8 && offsetof(GSDrawRow, vfirst) == 16 && offsetof(GSDrawRow, seq) == 28);

	/// What a draw's vertices are, filled where they are written. Lives only until the row is written.
	struct alignas(16) GSDrawFacts
	{
		enum : u16
		{
			RGB_ZERO = 1u << 0,
			ALPHA_ZERO = 1u << 1,
			COLOUR_128 = 1u << 2,
			QUADS = 1u << 3,
			NO_GAPS = 1u << 4,
			Z_OVER = 1u << 5,
			X1_FRAC_HIGH = 1u << 6, ///< bit 3 of vertex 1's X sub-pixel position
			Y1_FRAC_HIGH = 1u << 7,
			U0_IS_8 = 1u << 8,
			U0_IS_16 = 1u << 9,
			MIN_X_HALF = 1u << 10, ///< the lowest x is -0.5
		};

		u32 seq; ///< sequence number in draw order, never reused within SEQ_WRAP draws
		u32 vfirst, nverts; ///< where the vertices are in the stream, after sprite expansion
		u32 nidx;
		u16 box[4]; ///< bounds in native target pixels, before the half-pixel push
		u16 uv[4], uv_rounded[4]; ///< texture-coordinate range before and after texel rounding
		u8 a_min, a_max; ///< vertex alpha range
		u8 prim_class, eq; ///< primitive class; which components are equal over the draw
		u16 bits;
		u8 bake_x, bake_y; ///< sixteenths added to every X and Y by the half-pixel push (0 to 16)
		float q0; ///< Q of vertex 0, meaningful when Q is equal over the draw
		u64 memo; ///< the burst memo's signature; opaque here
	};
	static_assert(sizeof(GSDrawFacts) == 64);

	/// Sequence numbers wrap here: the fog word holds the low byte of fog and 24 bits of sequence.
	inline constexpr u32 SEQ_WRAP = 1u << 24;
	/// Most rows one run holds: the shader table is indexed by seq - first seq.
	inline constexpr u32 MAX_RUN_ROWS = 8184;

	/// Whether `seq` still fits the run that started at `first`, across the 24-bit wrap.
	inline bool SeqInRun(u32 first, u32 seq)
	{
		return ((seq - first) & (SEQ_WRAP - 1)) < MAX_RUN_ROWS;
	}
} // namespace GSDrawRows
