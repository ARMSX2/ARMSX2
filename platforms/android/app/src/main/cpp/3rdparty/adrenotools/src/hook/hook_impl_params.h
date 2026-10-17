// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws

#pragma once

#include <atomic>
#include <string>
#include <adrenotools/priv.h>

/**
 * @brief Holds the parameters needed for all hooks
 * @note See comments for adrenotools_open_libvulkan as a reference for member variables
 */
struct HookImplParams {
    int featureFlags;
    std::string tmpLibDir;
    std::string hookLibDir;
    std::string customDriverDir;
    std::string customDriverName;
    std::string fileRedirectDir;
    adrenotools_gpu_mapping *nextGpuMapping;
    std::atomic<uint32_t> *contextPriority; //!< KGSL priority for draw contexts created now (0: the driver's own)
    std::atomic<uint32_t> *contextsRaised; //!< Draw contexts created with such a priority

    HookImplParams(int featureFlags, const char *tmpLibDir, const char *hookLibDir, const char *customDriverDir,
                  const char *customDriverName, const char *fileRedirectDir, adrenotools_gpu_mapping *nextGpuMapping,
                  std::atomic<uint32_t> *contextPriority = nullptr, std::atomic<uint32_t> *contextsRaised = nullptr)
        : featureFlags(featureFlags),
          tmpLibDir(tmpLibDir ? tmpLibDir : ""),
          hookLibDir(hookLibDir),
          customDriverDir(customDriverDir ? customDriverDir : ""),
          customDriverName(customDriverName ? customDriverName : ""),
          fileRedirectDir(fileRedirectDir ? fileRedirectDir : ""),
          nextGpuMapping(nextGpuMapping),
          contextPriority(contextPriority),
          contextsRaised(contextsRaised) {}
};
