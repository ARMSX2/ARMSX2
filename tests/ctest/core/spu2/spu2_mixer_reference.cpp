// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The frozen copy of the mixer (spu2_mixer_reference.inc), in namespace isa_reference. See spu2_mixer_tests.cpp.

#define MULTI_ISA_UNSHARED_COMPILATION isa_reference
#define MULTI_ISA_IS_FIRST 0
#define spu2Output Spu2TestOutputReference

#include "spu2_mixer_reference.inc"

void ReferenceSpu2Mix()
{
	isa_reference::spu2Mix();
}

s32 ReferenceReverbDownsample(V_Core& core, bool right)
{
	return isa_reference::RefReverbDownsample(core, right);
}

StereoOut32 ReferenceReverbUpsample(V_Core& core)
{
	return isa_reference::RefReverbUpsample(core);
}
