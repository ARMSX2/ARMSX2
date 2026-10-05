// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// RAISR (Rapid and Accurate Image Super Resolution) 2x upscaler for RGBA8 images.
///
/// The algorithm and the trained filters are Intel's Library for Video Super Resolution
/// (BSD-3-Clause, https://github.com/OpenVisualCloud/Video-Super-Resolution-Library); the code is
/// a plain C++ port with a NEON path, structured after cppraisr (MIT). No GS, GPU or settings
/// dependencies, so it can be unit-tested and built standalone.
///
/// Per pixel: a bilinear 2x upscale of luma, a 11x11 structure tensor of its gradients picks one
/// of 24 angles x 3 strengths x 3 coherences hash buckets, and that bucket's filter (one per output
/// pixel phase) is dotted with the 11x11 luma neighbourhood. A census blend against the plain
/// bilinear result limits the filter's overshoot. Chroma and alpha are upscaled bilinearly.
///
/// Every entry point is safe to call from several threads at once, including with one FilterSet.
namespace GSTextureUpscaler
{
	inline constexpr u32 kAngleBins = 24;
	inline constexpr u32 kStrengthBins = 3;
	inline constexpr u32 kCoherenceBins = 3;
	inline constexpr u32 kHashCount = kAngleBins * kStrengthBins * kCoherenceBins; // 216
	inline constexpr u32 kPixelPhases = 4; // 2x2 output pixel positions per source pixel
	inline constexpr u32 kPatchSize = 11; // filter window, odd
	inline constexpr u32 kFilterTaps = kPatchSize * kPatchSize; // 121
	/// Filter rows are stored zero-padded to 12 taps so a row is three float32x4.
	inline constexpr u32 kFilterRowStride = 12;
	inline constexpr u32 kFilterStride = kPatchSize * kFilterRowStride; // floats per padded filter

	/// One trained Intel filter set: 216 hash buckets x 4 pixel phases of 11x11 taps, plus the
	/// two strength and two coherence bin edges that map a pixel onto a bucket.
	class FilterSet
	{
	public:
		/// dir holds filterbin_2_8, Qfactor_strbin_2_8 and Qfactor_cohbin_2_8. dir is UTF-8.
		/// Returns null and sets *error (if non-null) when a file is missing or malformed.
		static std::shared_ptr<const FilterSet> Load(const std::string& dir, std::string* error);

		/// Same, from memory. The header must be "fp32", then little-endian u32 hash count (216),
		/// pixel phases (4) and taps (121); the file must be exactly header + payload. The bin
		/// edge strings hold exactly two whitespace-separated decimal numbers each, ascending.
		static std::shared_ptr<const FilterSet> LoadFromMemory(std::span<const u8> filterbin,
			std::string_view strbin, std::string_view cohbin, std::string* error);

		/// The padded filter for a hash bucket (0..215) and pixel phase (0..3): 11 rows of
		/// kFilterRowStride floats, the last float of each row zero. Row-major, row 0 is the top.
		const float* Filter(u32 hash, u32 phase) const { return m_taps.data() + (hash * kPixelPhases + phase) * kFilterStride; }

		/// Strength and coherence bucket edges: the bucket index is how many edges the value
		/// is greater than.
		const float* StrengthEdges() const { return m_strength_edges; }
		const float* CoherenceEdges() const { return m_coherence_edges; }

	private:
		FilterSet() = default;

		std::vector<float> m_taps;
		float m_strength_edges[2] = {};
		float m_coherence_edges[2] = {};
	};

	/// Which implementation to run. Auto picks NEON when it is compiled in. Neon on a build
	/// without NEON runs the scalar path. Both give bit-identical float planes and bytes.
	enum class Impl
	{
		Auto,
		Scalar,
		Neon,
	};

	/// True when this build has the NEON path.
	bool NeonAvailable();

	/// The intermediate luma planes of an upscale, all (2w)x(2h), row-major, 0..255 scale.
	struct LumaPlanes
	{
		u32 width = 0;
		u32 height = 0;
		std::vector<float> cheap; ///< Bilinear 2x luma (Intel's "LR upscaled", L).
		std::vector<s32> hash; ///< Hash bucket per pixel: angle * 9 + strength * 3 + coherence.
		std::vector<float> raisr; ///< Filter output, after the range fallback to L.
		std::vector<float> blended; ///< Final luma Y' after the census blend.
	};

	/// Upscale an RGBA8 image 2x. dst is (2w)x(2h). Pitches are in bytes. src and dst must not
	/// overlap. Any size works, including sizes smaller than the filter window: the image is
	/// treated as edge-replicated beyond its border.
	void UpscaleRGBA8x2(const FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u8* dst, u32 dst_pitch);

	/// UpscaleRGBA8x2 that also returns the luma planes (planes may be null) and picks the
	/// implementation. For tests and tools.
	void UpscaleRGBA8x2Planes(const FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u8* dst, u32 dst_pitch, Impl impl, LumaPlanes* planes);

	/// Plain bilinear 2x on the same centre-aligned sample grid, rounded to nearest. Used for
	/// levels too small for RAISR. Its alpha equals UpscaleRGBA8x2's alpha exactly.
	void BilinearRGBA8x2(const u8* src, u32 w, u32 h, u32 src_pitch, u8* dst, u32 dst_pitch);

	/// The hash bucket for a structure tensor [[a b][b d]] (a is the vertical-gradient energy, d
	/// the horizontal), as the scalar path computes it. a, b and d are in the engine's units
	/// (gradients of 0..255 luma, window-weighted, divided by 4 * 255^2).
	s32 HashTensor(const FilterSet& filters, float a, float b, float d);

	/// The 11 taps of the Gaussian that weights the structure tensor window: Intel's
	/// createGaussianKernel(11, 2.0).
	std::array<float, kPatchSize> StructureKernel();
} // namespace GSTextureUpscaler
