// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

// Carrying a fixed blend factor (ALPHA.C == 2, AFIX) to the blend unit through the second
// fragment output instead of through the API's blend constant.
//
// There are two hardware expressions of the same number. A fixed factor is a per-draw constant, so
// the natural one is the blend constant: the blend state asks for CONST_COLOR / INV_CONST_COLOR and
// the backend hands the value to vkCmdSetBlendConstants. The other is the second fragment output,
// which the blend unit already reads for the As equations (SRC1_COLOR / INV_SRC1_COLOR); the shader
// writes AFIX/128 there and the factor arrives the same way As does. Both feed the same fixed-
// function multiply, and for AFIX <= 128 both feed it the same value.
//
// Mesa Turnip applies VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR as if the constant were zero on some
// draws -- the destination survives at full strength instead of at 1/128 -- while honouring
// ONE_MINUS_SRC1_COLOR correctly in the same render pass on the same frame. Katamari Damacy's ball
// is the visible case: every layer accumulates and the ball saturates towards white. The trigger is
// run history, not anything the draw carries, so no emission change reaches it; re-emitting the
// constant before every draw that reads it (17 -> 58 vkCmdSetBlendConstants a frame) moves zero
// pixels. Reproduced on Adreno 650 and 610 (Mesa 26.1.2) and Adreno 740 (26.3.0-devel), with the
// Qualcomm blob correct on the same silicon. Evidence and the RenderDoc solve of the applied factor:
// umbrella devs/bmdhacks/campaigns/gs-exactness-round/classic-fixes/katamari-turnip-overbright/.
//
// So on a device the driver-bug database marks BrokenBlendConstant, stop asking for the constant and
// send the same number through the second output. That is a pure rewrite of the blend state plus one
// pixel-shader selector bit; it changes HOW the factor reaches the blender and never WHETHER a draw
// is blended in hardware.
//
// Kept here, as pure functions, because the no-change half is the half that matters: every device
// without the bug bit must take byte-identical decisions, and that cannot be observed on the one
// device that takes the changed road.
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

	/// The same blend state with every constant-colour factor moved to its dual-source twin, and the
	/// blend constant dropped -- nothing reads it any more, so the backend has no reason to set it.
	static constexpr GSHWDrawConfig::BlendState RemapToSecondOutput(const GSHWDrawConfig::BlendState& bs)
	{
		return GSHWDrawConfig::BlendState(bs.enable, RemapFactor(bs.src_factor), RemapFactor(bs.dst_factor), bs.op,
			RemapFactor(bs.src_factor_alpha), RemapFactor(bs.dst_factor_alpha), false, 0);
	}

	/// Everything about the draw the decision reads, beyond the blend state itself. All of it is
	/// pixel-shader selector state, because the question the guards answer is "would the second
	/// output still be exactly vec4(AFIX/128) when the blend unit reads it?".
	struct DrawInputs
	{
		/// The driver-bug database says this device ignores the blend constant.
		bool broken_blend_constant = false;
		/// The device has a second fragment output to blend from at all.
		bool dual_source_blend = true;
		/// GSHWDrawConfig::PSSelector::blend_c -- 2 is the fixed factor, AFIX.
		u8 blend_c = 0;
		/// GSHWDrawConfig::PSSelector::pabe. PABE reads the second output's alpha as the SOURCE
		/// alpha to decide per pixel whether to blend at all; AFIX is not that number.
		bool pabe = false;
		/// GSHWDrawConfig::PSSelector::blend_factor_in_alpha. The no-dual-source substitution,
		/// which is already using the factor for something else on a device that has no SRC1.
		bool blend_factor_in_alpha = false;
		/// The blend multi-pass second draw reads the second output, so its value is spoken for and
		/// the two passes share one pixel shader selector.
		bool multi_pass_reads_second_output = false;
	};

	/// May this draw's fixed factor travel through the second fragment output?
	///
	/// The guards, in order: the device must have the defect and a second output to use; the factor
	/// must actually be the fixed one and actually be reaching the blender as a constant; AFIX must
	/// be at or below 1.0, because above it the constant path has its own clamp behaviour that this
	/// change deliberately does not touch; and nothing else may already own the second output --
	/// once ps_blend rewrites it (every blend_hw type that does comes with a SRC1 factor in the same
	/// state) or PABE claims it, it is no longer AFIX/128 when the blend unit reads it.
	static constexpr bool CanRouteFixedFactorToSecondOutput(
		const GSHWDrawConfig::BlendState& bs, const DrawInputs& in)
	{
		return in.broken_blend_constant && in.dual_source_blend && bs.enable && bs.constant_enable &&
		       in.blend_c == 2 && bs.constant <= 128 && ReadsBlendConstant(bs) && !ReadsSecondOutput(bs) &&
		       !in.multi_pass_reads_second_output && !in.pabe && !in.blend_factor_in_alpha;
	}
} // namespace GSBlendConstantPolicy
