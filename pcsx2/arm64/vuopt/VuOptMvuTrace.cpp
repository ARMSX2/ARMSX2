// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuOptMvuTrace.h"

namespace vuopt
{
#ifdef PCSX2_RECOMPILER_TESTS
	MvuBlockHook g_mvuBlockHook = nullptr;
	void* g_mvuBlockHookUser = nullptr;
#endif
	VuoptBodyHook g_vuoptBodyHook = nullptr;
	void* g_vuoptBodyHookUser = nullptr;
	VuoptRegionHook g_vuoptRegionHook = nullptr;
	void* g_vuoptRegionHookUser = nullptr;
	thread_local bool g_vuoptShadowCompile = false;
} // namespace vuopt
