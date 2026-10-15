// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotMvuTrace.h"

namespace vuaot
{
#ifdef PCSX2_RECOMPILER_TESTS
	MvuBlockHook g_mvuBlockHook = nullptr;
	void* g_mvuBlockHookUser = nullptr;
#endif
	VuAotBodyHook g_vuAotBodyHook = nullptr;
	void* g_vuAotBodyHookUser = nullptr;
	VuAotRegionHook g_vuAotRegionHook = nullptr;
	void* g_vuAotRegionHookUser = nullptr;
	thread_local bool g_vuAotShadowCompile = false;
} // namespace vuaot
