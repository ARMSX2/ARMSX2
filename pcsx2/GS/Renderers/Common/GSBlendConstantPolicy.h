// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

// Carrying a fixed blend factor (ALPHA.C == 2, AFIX) to the blend unit through the second
// fragment output instead of through the API's blend constant.
//
// A fixed factor normally goes through the blend constant (CONST_COLOR / INV_CONST_COLOR,
// vkCmdSetBlendConstants). Alternatively the shader writes AFIX/128 to the second fragment output,
// which the blend unit already reads for the As equations (SRC1_COLOR / INV_SRC1_COLOR). For
// AFIX <= 128 both feed the same value to the same fixed-function multiply.
//
// Mesa Turnip (Adreno 6xx/7xx) sometimes applies ONE_MINUS_CONSTANT_COLOR as if the constant were
// zero, leaving the destination at full strength, while handling ONE_MINUS_SRC1_COLOR correctly in
// the same pass. Katamari Damacy's ball saturates towards white. The trigger depends on run
// history, not on the draw, and re-emitting the constant per draw does not help. The Qualcomm
// proprietary driver is correct on the same hardware.
//
// On devices the driver-bug database marks BrokenBlendConstant, the constant is sent through the
// second output instead. This rewrites the blend state and sets one pixel-shader selector bit; it
// changes HOW the factor reaches the blender, never WHETHER a draw is hardware-blended.
//
// Pure functions so the no-change case (every device without the bit takes identical decisions)
// can be tested off-device.
namespace GSBlendConstantPolicy
{
	/// The dual-source twin of a constant-colour blend factor. Every other factor is unchanged.
	/// There is no constant-ALPHA factor in the enum, so the alpha slots go through the same map.
	static constexpr u8 RemapFactor(u8 factor)
	{
		switch (factor)
		{
			case GSDevice::CONST_COLOR:
				return GSDevice::SRC1_COLOR;
			case GSDevice::INV_CONST_COLOR:
				return GSDevice::INV_SRC1_COLOR;
			default:
				return factor;
		}
	}

	/// Does any of the four factors read the blend constant?
	static constexpr bool ReadsBlendConstant(const GSHWDrawConfig::BlendState& bs)
	{
		return GSDevice::IsConstantBlendFactor(bs.src_factor) || GSDevice::IsConstantBlendFactor(bs.dst_factor) ||
		       GSDevice::IsConstantBlendFactor(bs.src_factor_alpha) ||
		       GSDevice::IsConstantBlendFactor(bs.dst_factor_alpha);
	}

	/// Does any of the four factors already read the second fragment output?
	static constexpr bool ReadsSecondOutput(const GSHWDrawConfig::BlendState& bs)
	{
		return GSDevice::IsDualSourceBlendFactor(bs.src_factor) || GSDevice::IsDualSourceBlendFactor(bs.dst_factor) ||
		       GSDevice::IsDualSourceBlendFactor(bs.src_factor_alpha) ||
		       GSDevice::IsDualSourceBlendFactor(bs.dst_factor_alpha);
	}

	/// The blend state with constant-colour factors moved to their dual-source twins and the blend
	/// constant dropped, since nothing reads it.
	static constexpr GSHWDrawConfig::BlendState RemapToSecondOutput(const GSHWDrawConfig::BlendState& bs)
	{
		return GSHWDrawConfig::BlendState(bs.enable, RemapFactor(bs.src_factor), RemapFactor(bs.dst_factor), bs.op,
			RemapFactor(bs.src_factor_alpha), RemapFactor(bs.dst_factor_alpha), false, 0);
	}

	/// Pixel-shader selector state the decision reads, beyond the blend state. The guards ask whether
	/// the second output would still be exactly vec4(AFIX/128) when the blend unit reads it.
	struct DrawInputs
	{
		/// The driver-bug database says this device ignores the blend constant.
		bool broken_blend_constant = false;
		/// The device has a second fragment output to blend from at all.
		bool dual_source_blend = true;
		/// GSHWDrawConfig::PSSelector::blend_c -- 2 is the fixed factor, AFIX.
		u8 blend_c = 0;
		/// GSHWDrawConfig::PSSelector::pabe. PABE reads the second output's alpha as the source alpha
		/// to decide per pixel whether to blend.
		bool pabe = false;
		/// GSHWDrawConfig::PSSelector::blend_factor_in_alpha: the no-dual-source substitution, which
		/// already uses the output for something else.
		bool blend_factor_in_alpha = false;
		/// The blend multi-pass second draw reads the second output, and both passes share one pixel
		/// shader selector.
		bool multi_pass_reads_second_output = false;
	};

	/// May this draw's fixed factor travel through the second fragment output?
	///
	/// Guards: the device has the defect and a second output; the factor is AFIX and reaches the
	/// blender as a constant; AFIX <= 1.0 (above that the constant path has its own clamp behaviour,
	/// left untouched); and nothing else owns the second output (ps_blend rewriting it, which always
	/// comes with a SRC1 factor in the state, or PABE).
	static constexpr bool CanRouteFixedFactorToSecondOutput(
		const GSHWDrawConfig::BlendState& bs, const DrawInputs& in)
	{
		return in.broken_blend_constant && in.dual_source_blend && bs.enable && bs.constant_enable &&
		       in.blend_c == 2 && bs.constant <= 128 && ReadsBlendConstant(bs) && !ReadsSecondOutput(bs) &&
		       !in.multi_pass_reads_second_output && !in.pabe && !in.blend_factor_in_alpha;
	}
} // namespace GSBlendConstantPolicy
