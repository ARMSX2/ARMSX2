// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP1 {

// EE divide unit's significand recurrence. eeDivideSignificandPortable() in
// FPU.cpp is the reference; eeDivideSignificandArm64() is the same recurrence
// written for the pipes it runs on, and EeFpuDivUnitArm64Form holds the two
// to each other. The arm64 form is defined in FPU-divunit-arm64.cpp.
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
#define EE_DIVUNIT_ARM64 1
u32 eeDivideSignificandArm64(u32 sma, u32 smb, u64 T2, u32 T);
#else
#define EE_DIVUNIT_ARM64 0
#endif

} // namespace COP1
} // namespace OpcodeImpl
} // namespace Interpreter
} // namespace R5900
