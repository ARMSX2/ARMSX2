// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The mixer as it is in the tree: pcsx2/SPU2/Mixer.cpp compiled a second time, in namespace isa_current, with its output
// going to the test's capture function instead of the audio stream. See spu2_mixer_tests.cpp.

#define MULTI_ISA_UNSHARED_COMPILATION isa_current
#define MULTI_ISA_IS_FIRST 0
#define spu2Output Spu2TestOutputCurrent

#include "SPU2/Mixer.cpp"

void CurrentSpu2Mix()
{
	isa_current::spu2Mix();
}
