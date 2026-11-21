// DeviceStats.mm — the device stats cache the OSD reads
// SPDX-License-Identifier: GPL-3.0+

#import <UIKit/UIKit.h>

#include <mach/mach.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "pcsx2/Host.h"

#include "IOSRuntime.h"

double ARMSX2IOSGetAppRAMGB()
{
    task_vm_info_data_t vm_info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) != KERN_SUCCESS)
        return 0.0;

    return static_cast<double>(vm_info.phys_footprint) / (1024.0 * 1024.0 * 1024.0);
}

const char* ARMSX2IOSHeatStateName(NSProcessInfoThermalState state)
{
    switch (state) {
    case NSProcessInfoThermalStateNominal:
        return "OK";
    case NSProcessInfoThermalStateFair:
        return "Warm";
    case NSProcessInfoThermalStateSerious:
        return "Hot";
    case NSProcessInfoThermalStateCritical:
        return "Critical";
    default:
        return "Unknown";
    }
}

struct ARMSX2IOSDeviceStatsCache
{
    bool show = true;
    int severity = 0;
    std::string line;
    std::chrono::steady_clock::time_point last_update;
};

static std::mutex s_device_stats_mutex;
static ARMSX2IOSDeviceStatsCache s_device_stats_cache;

static const ARMSX2IOSDeviceStatsCache& ARMSX2IOSRefreshDeviceStatsCacheLocked()
{
    const auto now = std::chrono::steady_clock::now();
    if (!s_device_stats_cache.line.empty() &&
        (now - s_device_stats_cache.last_update) < std::chrono::seconds(1))
    {
        return s_device_stats_cache;
    }

    // Mirror the rule the Swift side loads with, so a fresh install with the key still absent agrees
    // with the preset instead of showing stats at Off.
    s_device_stats_cache.show = s_settings_interface ?
        Host::GetBaseBoolSettingValue("ARMSX2iOS/UI", "OsdShowDeviceStats",
            Host::GetBaseIntSettingValue("ARMSX2iOS/UI", "OsdPreset", 0) != 0) : false;

    @autoreleasepool {
        UIDevice* device = [UIDevice currentDevice];
        const float battery = [device batteryLevel];
        const int battery_percent = (battery >= 0.0f) ? static_cast<int>(std::round(battery * 100.0f)) : -1;
        const NSProcessInfoThermalState thermal_state = [[NSProcessInfo processInfo] thermalState];
        const double app_ram_gb = ARMSX2IOSGetAppRAMGB();
        const bool low_power = [[NSProcessInfo processInfo] isLowPowerModeEnabled];

        if (thermal_state >= NSProcessInfoThermalStateSerious || (battery >= 0.0f && battery <= 0.15f))
            s_device_stats_cache.severity = 2;
        else if (thermal_state == NSProcessInfoThermalStateFair || (battery >= 0.0f && battery <= 0.30f))
            s_device_stats_cache.severity = 1;
        else
            s_device_stats_cache.severity = 0;

        char buffer[192];
        if (battery_percent >= 0) {
            std::snprintf(buffer, sizeof(buffer), "Battery: %d%% | Heat: %s | RAM: %.1f GB%s",
                battery_percent, ARMSX2IOSHeatStateName(thermal_state), app_ram_gb, low_power ? " | Low Power" : "");
        } else {
            std::snprintf(buffer, sizeof(buffer), "Battery: -- | Heat: %s | RAM: %.1f GB%s",
                ARMSX2IOSHeatStateName(thermal_state), app_ram_gb, low_power ? " | Low Power" : "");
        }

        s_device_stats_cache.line = buffer;
    }

    s_device_stats_cache.last_update = now;
    return s_device_stats_cache;
}

extern "C" bool ARMSX2_iOSShouldShowDeviceStatsOverlay()
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    return ARMSX2IOSRefreshDeviceStatsCacheLocked().show;
}

extern "C" int ARMSX2_iOSGetDeviceStatsOverlaySeverity()
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    return ARMSX2IOSRefreshDeviceStatsCacheLocked().severity;
}

extern "C" const char* ARMSX2_iOSGetDeviceStatsOverlayLine()
{
    // Handing back a pointer into the cached string leaves the caller reading it after the lock has
    // gone, and another thread refreshing the cache can reallocate it underneath them. Copy into
    // storage the calling thread owns instead.
    static thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lock(s_device_stats_mutex);
        copy = ARMSX2IOSRefreshDeviceStatsCacheLocked().line;
    }
    return copy.c_str();
}

// Structured device stats for the SwiftUI VoiceOver HUD mirror, from the same cache as
// ARMSX2_iOSGetDeviceStatsOverlayLine. TODO: FPS - the overlay FPS lives in the
// PerformanceOverlay path with no thread-safe getter surfaced here yet; omitted for now.
extern "C" void ARMSX2_iOSCopyDeviceStats(int* outBatteryPercent, int* outThermalState,
                                          double* outRamGB, bool* outLowPower)
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    const auto& stats = ARMSX2IOSRefreshDeviceStatsCacheLocked();
    if (outBatteryPercent) {
        UIDevice* device = [UIDevice currentDevice];
        const float battery = [device batteryLevel];
        *outBatteryPercent = (battery >= 0.0f) ? static_cast<int>(std::round(battery * 100.0f)) : -1;
    }
    if (outThermalState) *outThermalState = stats.severity;
    if (outRamGB) *outRamGB = ARMSX2IOSGetAppRAMGB();
    if (outLowPower) *outLowPower = [[NSProcessInfo processInfo] isLowPowerModeEnabled];
}
