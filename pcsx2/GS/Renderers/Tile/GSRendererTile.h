// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"

MULTI_ISA_UNSHARED_START

// The tiler-native hardware renderer. Vulkan-only and opt-in; the selection decision is
// GSTileSelectionPolicy.h and the constructing branch is OpenGSRenderer.
//
// What ships here today is the SW-fallback floor and nothing else: every draw takes the
// inherited GSRendererSW path — the "spill inputs to truth" half is trivial while no native
// draw exists, because CPU local memory is always truth — and presentation reads local
// memory back out. That makes the whole dump corpus scoreable end-to-end against the SW
// oracle before the first native triangle exists, which is the point of landing the floor
// first. The floor is not scaffolding: it stays as the permanent lowest rung of the
// realization lattice. The native path lands as a Draw() override that routes provable
// draws to the pass graph and falls through to this floor for everything it cannot express.
class GSRendererTile final : public GSRendererSW
{
public:
	GSRendererTile(int threads);
};

MULTI_ISA_UNSHARED_END
