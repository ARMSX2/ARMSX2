// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/frame_gen/lsfg_translate.h.
// Logic unchanged; only the scalar aliases are ours. See FrameGenTypes.h.

#pragma once

#include "FrameGenTypes.h"

#include <span>
#include <vector>

namespace VideoCore::FrameGen {

[[nodiscard]] bool IsSpirvModule(std::span<const u8> blob);

[[nodiscard]] std::vector<u32> AdoptSpirvModule(std::span<const u8> blob);

} // namespace VideoCore::FrameGen
