// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSRendererTile.h"

MULTI_ISA_UNSHARED_IMPL;

GSRenderer* CURRENT_ISA::makeGSRendererTile(int threads)
{
	return new GSRendererTile(threads);
}

GSRendererTile::GSRendererTile(int threads)
	: GSRendererSW(threads)
{
}
