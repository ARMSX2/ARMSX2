// SceneDelegate+VMThread.mm — the persistent VM thread, the JIT keepalive and the JIT gate
// SPDX-License-Identifier: GPL-3.0+

#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Darwin/DarwinMisc.h"

#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/Config.h"          // EmuConfig, GSConfig
#include "pcsx2/Counters.h"
#include "pcsx2/Host.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Memory.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/R5900.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/vtlb.h"

#include "IconsFontAwesome.h"
#include "IOSRuntime.h"
#import "ARMSX2BridgeKeys.h"
#import "SceneDelegateInternal.h"

// JIT keepalive: every 12 seconds while the VM is idle, DarwinMisc::ValidateJITAlive()
// re-checks the grant. iOS can revoke CS_DEBUGGED ~30-60s after the app goes inactive,
// and catching that early lets the UI react before the next boot attempt.
static dispatch_source_t s_jitKeepaliveTimer = nil;
static std::mutex s_jitKeepaliveMutex;
static std::atomic<bool> s_jitExpired{false};

// s_vmInitComplete is false from just before CPUThreadInitialize until it returns, and the
// init watchdog polls it for 15 seconds. s_vmThreadShouldExit wakes the boot loop so it can
// exit when revalidation replaces the thread with a new one.
static std::atomic<bool> s_vmInitComplete{false};
static std::atomic<bool> s_vmThreadShouldExit{false};
static std::atomic<bool> s_idleVMPrewarmResolved{false};

// True while a thread may run or emit JIT code: the VM runs, or the worker is inside
// CPUThreadInitialize. ValidateJITAlive skips its arena canary then, because the prewarm on
// every app switch would flip the dispatcher page under a live CPU thread: an Instruction Abort.
bool ARMSX2JITWorkerBusy()
{
    if (s_vmThreadActive.load(std::memory_order_acquire))
        return true;
    // Before the worker exists there is no arena (the canary skips g_code_rw_base==0),
    // so this only matters while CPUThreadInitialize is in flight.
    return !s_vmInitComplete.load(std::memory_order_acquire);
}

static void ARMSX2ResolveIdleVMPrewarm()
{
    bool expected = false;
    if (!s_idleVMPrewarmResolved.compare_exchange_strong(expected, true))
        return;

    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter]
            postNotificationName:ARMSX2iOSIdleVMPrewarmResolved object:nil];
    });
}

extern "C" bool ARMSX2_IsIdleVMPrewarmResolved()
{
#if TARGET_OS_SIMULATOR
    return true;
#else
    return s_idleVMPrewarmResolved.load(std::memory_order_acquire);
#endif
}

// The backend is fixed for a CPU thread's lifetime, so switching between JIT and
// interpreter needs a full CPUThreadShutdown/CPUThreadInitialize cycle.
static bool s_vmThreadUsesJIT = false; // guarded by s_vmMutex

static void ARMSX2StopJITKeepalive()
{
    std::lock_guard<std::mutex> lock(s_jitKeepaliveMutex);
    if (s_jitKeepaliveTimer)
    {
        dispatch_source_cancel(s_jitKeepaliveTimer);
        s_jitKeepaliveTimer = nil;
        NSLog(@"@@JIT_KEEPALIVE@@ timer_stopped");
    }
}

static void ARMSX2StartJITKeepalive()
{
    // Interpreter-only sessions have no executable mapping and no JIT grant to keep.
    if (DarwinMisc::iPSX2_FORCE_EE_INTERP || !SysMemory::HasCodeMemory())
        return;
    std::lock_guard<std::mutex> lock(s_jitKeepaliveMutex);
    if (s_jitKeepaliveTimer) return;
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0);
    s_jitKeepaliveTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(s_jitKeepaliveTimer,
                              dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC),
                              12 * NSEC_PER_SEC, 0);
    dispatch_source_set_event_handler(s_jitKeepaliveTimer, ^{
        // The recompiler keeps writing code while the VM runs, so the grant cannot lapse then.
        if (s_vmThreadActive.load(std::memory_order_relaxed))
            return;
        if (!DarwinMisc::ValidateJITAlive())
        {
            s_jitExpired.store(true);
            ARMSX2StopJITKeepalive();
        }
    });
    dispatch_resume(s_jitKeepaliveTimer);
    NSLog(@"@@JIT_KEEPALIVE@@ timer_started interval=12s");
}

@implementation PCSX2SceneDelegate (VMThread)

#pragma mark - JIT gate & VM launch
// JIT availability check for real devices. The allocation path can use MAP_JIT,
// dual-map, or legacy mprotect depending on what iOS/LiveContainer permits.
- (void)checkJITAndStartVM {
#if !TARGET_OS_SIMULATOR
    // Do not change the process-wide backend request underneath an active VM.
    if (s_vmThreadActive.load(std::memory_order_acquire)) {
        [self startVMThread];
        return;
    }

    ARMSX2ApplyJITScriptProtocol("jit-gate");

    // iOS can revoke CS_DEBUGGED after ~30-60s idle, so check again even if JIT was up at launch.
    const bool jitAlive = DarwinMisc::IsJITAvailable() && DarwinMisc::ValidateJITAlive();

    if (jitAlive) {
        std::fprintf(stderr, "@@BOOT_JIT_GATE@@ available=1 mode=jit_alloc\n");
        std::fflush(stderr);
        Console.WriteLn("@@JIT_GATE@@ JIT channel available; starting VM");
        DarwinMisc::iPSX2_FORCE_EE_INTERP = 0;
        [self startVMThread];
        return;
    }

    // JIT is dead. Fall back to interpreter instead of blocking boot.
    std::fprintf(stderr, "@@BOOT_JIT_GATE@@ available=0 fallback=interpreter\n");
    std::fflush(stderr);

    DarwinMisc::iPSX2_FORCE_EE_INTERP = 1;

    Host::AddIconOSDMessage("JITExpired", ICON_FA_TRIANGLE_EXCLAMATION,
        "JIT session expired — booting in interpreter mode (much slower). "
        "Relaunch the app to re-enable JIT.",
        15.0f);

    [self startVMThread];
#else
    [self startVMThread];
#endif
}

#pragma mark - Persistent VM thread
- (void)startVMThread {
    [self startVMThreadRequestingBoot:YES];
}

- (void)prepareVMThreadForIdle {
#if !TARGET_OS_SIMULATOR
    ARMSX2ApplyJITScriptProtocol("idle-vm-prewarm");
    const bool jitAlive = DarwinMisc::IsJITAvailable() && DarwinMisc::ValidateJITAlive();
    if (!jitAlive) {
        std::fprintf(stderr, "@@BOOT_IDLE_PREWARM@@ jit=0 action=skip\n");
        std::fflush(stderr);
        ARMSX2ResolveIdleVMPrewarm();
        return;
    }

    DarwinMisc::iPSX2_FORCE_EE_INTERP = 0;
    std::fprintf(stderr, "@@BOOT_IDLE_PREWARM@@ jit=1 action=prepare\n");
    std::fflush(stderr);
    [self startVMThreadRequestingBoot:NO];
#endif
}

- (void)startVMThreadRequestingBoot:(BOOL)requestBoot {
    ARMSX2ApplyJITScriptProtocol("start-vm-thread");
    bool requestedJIT = !DarwinMisc::iPSX2_FORCE_EE_INTERP;
    {
        std::unique_lock<std::mutex> lk(s_vmMutex);
        if (s_vmThreadActive.load()) {
            std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=1 action=ignored\n");
            std::fflush(stderr);
            return;
        }

        if (requestBoot) {
            // Signal the persistent thread to boot. Idle preparation deliberately
            // leaves this false so the initialized worker blocks in its wait loop.
            s_requestVMBoot.store(true);
            s_requestVMStop.store(false);
        }

        if (s_vmThreadCreated) {
            if (!requestBoot) {
                std::fprintf(stderr, "@@BOOT_IDLE_PREWARM@@ action=already_prepared\n");
                std::fflush(stderr);
                if (s_vmInitComplete.load(std::memory_order_acquire))
                    ARMSX2ResolveIdleVMPrewarm();
                return;
            }

            // The persistent thread reuses the JIT memory from its first boot, so check the
            // grant before signaling it: after a revoke the recompiler would write into dead memory.
            if (requestedJIT && !DarwinMisc::ValidateJITAlive())
            {
                std::fprintf(stderr, "@@BOOT_JIT_GATE@@ revalidate=0 fallback=interpreter\n");
                std::fflush(stderr);
                DarwinMisc::iPSX2_FORCE_EE_INTERP = 1;
                requestedJIT = false;

                Host::AddIconOSDMessage("JITExpired", ICON_FA_TRIANGLE_EXCLAMATION,
                    "JIT session expired — booting in interpreter mode (much slower). "
                    "Relaunch the app to re-enable JIT.",
                    15.0f);

            }

            if (s_vmThreadUsesJIT == requestedJIT)
            {
                s_requestVMBoot.store(true);
                s_requestVMStop.store(false);
                std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=0 created=1 action=signal\n");
                std::fflush(stderr);
                s_vmCV.notify_one();
                return;
            }

            // An interpreter thread has no executable mapping, while a JIT
            // thread owns live code caches. Wait for full teardown before
            // constructing the other backend; a timed sleep can overlap both.
            std::fprintf(stderr, "@@BOOT_START_THREAD@@ backend_switch=%s_to_%s action=wait_for_exit\n",
                s_vmThreadUsesJIT ? "jit" : "interpreter",
                requestedJIT ? "jit" : "interpreter");
            std::fflush(stderr);
            s_requestVMBoot.store(false);
            s_vmThreadShouldExit.store(true);
            s_vmCV.notify_one();
            s_vmCV.wait(lk, [] { return !s_vmThreadCreated; });
        }

        s_requestVMBoot.store(requestBoot);
        s_requestVMStop.store(false);
        s_vmThreadShouldExit.store(false);
        s_vmThreadCreated = true;
        s_vmThreadUsesJIT = requestedJIT;
    }

    std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=0 created=0 action=create request_boot=%d\n",
        requestBoot ? 1 : 0);
    std::fflush(stderr);

    std::thread vmThread([]() {
        s_cpuThreadId = std::this_thread::get_id();
        ARMSX2ConfigureImGuiFonts("vm-thread");
        if (!DarwinMisc::iPSX2_FORCE_EE_INTERP)
        {
            // CPUThreadInitialize allocates fastmem before its full settings
            // reload. Restore this allocation-time input when recreating a JIT
            // backend after an interpreter-only session.
            EmuConfig.Cpu.Recompiler.EnableFastmem =
                Host::GetBaseBoolSettingValue(
                    "EmuCore/CPU/Recompiler", "EnableFastmem",
                    EmuConfig.Cpu.Recompiler.EnableFastmem);
        }
        // Init watchdog: if CPUThreadInitialize hangs (a Universal TXM prepare that never
        // traps), show an error and return to the menu instead of a black screen.
        s_vmInitComplete.store(false, std::memory_order_release);
        std::thread watchdog([]() {
            for (int i = 0; i < 150; i++) // 15 seconds at 100ms intervals
            {
                if (s_vmInitComplete.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!s_vmInitComplete.load(std::memory_order_acquire))
            {
                std::fprintf(stderr, "@@BOOT_FAIL@@ reason=vm_init_timeout stage=cpu_thread_initialize\n");
                std::fflush(stderr);
                dispatch_async(dispatch_get_main_queue(), ^{
                    Host::ReportErrorAsync("JIT Init Timeout",
                        "JIT memory setup took too long. This is a known issue with the Universal TXM "
                        "protocol on iOS 26. Try Settings → Emulator → JIT Script → Legacy, or relaunch "
                        "via StikDebug.");
                    [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSReturnToMenu object:nil];
                });
                ARMSX2ResolveIdleVMPrewarm();
                std::lock_guard<std::mutex> lk(s_vmMutex);
                s_vmThreadCreated = false;
            }
        });
        watchdog.detach();
        const bool cpuInitOk = VMManager::Internal::CPUThreadInitialize();
        s_vmInitComplete.store(true, std::memory_order_release);
        ARMSX2ResolveIdleVMPrewarm();
        // Benign race: a watchdog firing between init finishing and here has already reset
        // s_vmThreadCreated and shown "JIT Init Timeout". The cost is one extra dialog, and
        // the thread still blocks on its wait predicate.
        if (!cpuInitOk) {
            std::fprintf(stderr, "@@BOOT_THREAD_INIT@@ ok=0\n");
            std::fflush(stderr);
            Console.Error("VM Thread: CPUThreadInitialize failed.");
            std::lock_guard<std::mutex> lk(s_vmMutex);
            s_vmThreadCreated = false;
            s_vmThreadUsesJIT = false;
            s_vmCV.notify_all();
            return;
        }
        std::fprintf(stderr, "@@BOOT_THREAD_INIT@@ ok=1\n");
        std::fflush(stderr);
        {
            std::lock_guard<std::mutex> lk(s_vmMutex);
            s_vmThreadUsesJIT = !DarwinMisc::iPSX2_FORCE_EE_INTERP;
        }
        ARMSX2StartJITKeepalive(); // JIT acquired — start idle-period validation

        bool auto_boot_first = (getenv("ARMSX2_AUTO_BOOT") && atoi(getenv("ARMSX2_AUTO_BOOT")) == 1)
                            || (getenv("ARMSX2_BOOT_ELF") != nullptr);
        while (true) {
            // Wait for boot signal (or auto-boot on first iteration)
            {
                std::unique_lock<std::mutex> lk(s_vmMutex);
                if (auto_boot_first) {
                    Console.WriteLn("[AutoBoot] @@AUTO_BOOT@@ skipping UI wait, auto-boot enabled");
                    auto_boot_first = false;
                } else {
                    std::fprintf(stderr, "@@BOOT_THREAD_WAIT@@ waiting=1\n");
                    std::fflush(stderr);
                    s_vmCV.wait(lk, [] { return s_requestVMBoot.load() || s_vmThreadShouldExit.load(); });
                    if (s_vmThreadShouldExit.load(std::memory_order_relaxed))
                    {
                        s_vmThreadShouldExit.store(false, std::memory_order_relaxed);
                        std::fprintf(stderr, "@@BOOT_THREAD_EXIT@@ reason=should_exit\n");
                        std::fflush(stderr);
                        // A thread torn down for a new one pairs its init with a shutdown, or the
                        // new thread would reserve the ~161MB of SysMemory a second time.
                        s_vmInitComplete.store(false, std::memory_order_release);
                        ARMSX2StopJITKeepalive();
                        DarwinMisc::WaitForJITValidation();
                        VMManager::Internal::CPUThreadShutdown();
                        s_vmThreadCreated = false;
                        s_vmThreadUsesJIT = false;
                        s_vmCV.notify_all();
                        break; // exit the while(true) loop — thread ends
                    }
                }
                s_requestVMBoot.store(false);
            }

            std::fprintf(stderr, "@@BOOT_THREAD_SIGNAL@@ received=1\n");
            std::fflush(stderr);
            // Block new canaries, cancel periodic validation, then wait for a
            // callback which already passed the busy check. JIT execution may
            // begin only after that callback has restored the probed code byte.
            ARMSX2DiscardCPUThreadTasks();
            s_vmThreadActive.store(true, std::memory_order_release);
            ARMSX2StopJITKeepalive();
            DarwinMisc::WaitForJITValidation();

            VMBootParameters boot_params;
            const bool loadLastSaveState = s_loadLastSaveStateOnBoot.exchange(false);
            std::string bootGameName;
            boot_params.fast_boot = false;
            {
                std::string isoDir = EmuFolders::DataRoot + "/iso";
                std::string defaultISO = "";
                std::string isoFilename = Host::GetBaseStringSettingValue("GameISO", "BootISO", defaultISO.c_str());
                Host::SetBaseStringSettingValue("GameISO", "BootISO", isoFilename.c_str());
                Host::CommitBaseSettingChanges();
                std::string isoPath = (!isoFilename.empty() && isoFilename.front() == '/') ? isoFilename : (isoDir + "/" + isoFilename);
                // Fallback: check Documents/ root if not found in iso/.
                if (!isoFilename.empty() && isoFilename.front() != '/' && !FileSystem::FileExists(isoPath.c_str())) {
                    std::string rootPath = EmuFolders::DataRoot + "/" + isoFilename;
                    if (FileSystem::FileExists(rootPath.c_str())) {
                        isoPath = rootPath;
                        Console.WriteLn("ISO found in Documents/ root: %s", isoPath.c_str());
                    }
                }
                // Resolve fast boot from the per-game override if present, otherwise the
                // configured global value. Global settings are not mutated here.
                const bool fastBoot = ARMSX2ResolveFastBootForISO(isoPath);
                const bool isoExists = !isoFilename.empty() && FileSystem::FileExists(isoPath.c_str());
                if (isoExists) {
                    bootGameName = isoFilename;
                    std::string suffix = isoFilename.size() >= 4 ? isoFilename.substr(isoFilename.size() - 4) : "";
                    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
                    bool isElf = (suffix == ".elf");
                    if (isElf) {
                        boot_params.elf_override = isoPath;
                        boot_params.source_type = CDVD_SourceType::NoDisc;
                        boot_params.fast_boot = true;
                        std::string discPath = VMManager::GetDiscOverrideFromGameSettings(isoPath);
                        if (!discPath.empty()) {
                            if (FileSystem::FileExists(discPath.c_str())) {
                                boot_params.filename = discPath;
                                boot_params.source_type = CDVD_SourceType::Iso;
                            } else {
                                Host::ReportErrorAsync("Linked disc not found",
                                    "This ELF has a linked disc, but the disc file is missing. Booting without it. Re-link it in the game's Disc Path menu.");
                            }
                        }
                        std::fprintf(stderr, "@@ISO_BOOT@@ path=%s fast_boot=1 mode=ELF INI=\"%s\"\n",
                            isoPath.c_str(), isoFilename.c_str());
                        std::fflush(stderr);
                    } else {
                        boot_params.filename = isoPath;
                        boot_params.source_type = CDVD_SourceType::Iso;
                        boot_params.fast_boot = fastBoot;
                        std::fprintf(stderr, "@@ISO_BOOT@@ path=%s fast_boot=%d mode=ISO INI=\"%s\"\n",
                            isoPath.c_str(), fastBoot ? 1 : 0, isoFilename.c_str());
                        std::fflush(stderr);
                    }
                } else {
                    std::fprintf(stderr, "@@ISO_BOOT_MISSING@@ ini_iso=\"%s\" attempted=\"%s\"\n",
                        isoFilename.c_str(), isoPath.c_str());
                    std::fflush(stderr);
                }
            }

            if (getenv("ARMSX2_AUTO_BOOT_BIOS")) {
                Console.WriteLn("@@AUTO_BOOT_BIOS@@ enabled=1 action=triggered");
                boot_params.fast_boot = false;
            }
            // ps2autotests: boot ELF directly via env var
            if (const char* testElf = getenv("ARMSX2_BOOT_ELF")) {
                boot_params.elf_override = testElf;
                boot_params.source_type = CDVD_SourceType::NoDisc;
                boot_params.fast_boot = true;
                Console.WriteLn("@@BOOT_ELF@@ elf=%s", testElf);
            }

            // BIOS sanity check
            const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
            const bool biosExists = !EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str());
            std::fprintf(stderr, "@@BOOT_BIOS_CHECK@@ bios=\"%s\" path=\"%s\" exists=%d\n",
                EmuConfig.BaseFilenames.Bios.c_str(), biosPath.c_str(), biosExists ? 1 : 0);
            std::fflush(stderr);
            if (!biosExists) {
                std::fprintf(stderr, "@@BOOT_BIOS_FAIL@@ action=abort_to_menu\n");
                std::fflush(stderr);
                Console.Error("CRITICAL: BIOS verification failed inside VM thread.");
                Host::ReportErrorAsync("BIOS Error", "Validation failed.");
                s_vmThreadActive.store(false);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSVMDidShutdown object:nil];
                });
                continue; // back to wait loop
            }

            ARMSX2SanitizeFrameLimiterConfig("pre-vm-initialize");
            {
                auto lock = Host::GetSettingsLock();
                ARMSX2EnsureIOSSpeedhackDefaults(s_settings_interface, "pre-vm-initialize");
                ARMSX2RepairIOSARM64JITSettings(s_settings_interface, "pre-vm-initialize");
                ARMSX2MigrateJITScriptProtocolForIOS(s_settings_interface, "pre-vm-initialize");
                ARMSX2IOSSanitizeFolderSettings(s_settings_interface, EmuFolders::DataRoot, "pre-vm-initialize");
                VMManager::Internal::LoadStartupSettings();
            }
            ARMSX2ApplyIOSOsdPresetFromConfig("pre-vm-initialize");
            EmuConfig.Speedhacks.vuThread =
                Host::GetBaseBoolSettingValue("EmuCore/Speedhacks", "vuThread", EmuConfig.Speedhacks.vuThread);
            EmuConfig.Cpu.Recompiler.EnableFastmem =
                Host::GetBaseBoolSettingValue("EmuCore/CPU/Recompiler", "EnableFastmem", EmuConfig.Cpu.Recompiler.EnableFastmem);
            if (DarwinMisc::iPSX2_FORCE_EE_INTERP)
            {
                // Per-game settings are loaded after the JIT gate. Keep the
                // no-JIT capability as a boot-scoped runtime override without
                // rewriting the user's saved preferences.
                EmuConfig.Cpu.Recompiler.EnableEE = false;
                EmuConfig.Cpu.Recompiler.EnableIOP = false;
                EmuConfig.Cpu.Recompiler.EnableVU0 = false;
                EmuConfig.Cpu.Recompiler.EnableVU1 = false;
                EmuConfig.Cpu.Recompiler.EnableFastmem = false;
                EmuConfig.Speedhacks.vuThread = false;
            }
            // The INI read above can turn fastmem back on. If the 4 GB reservation failed at
            // boot, loads and stores would hit fastmem_base=0, so re-apply the sticky disable.
            if (vtlb_FastmemAreaUnavailable() && EmuConfig.Cpu.Recompiler.EnableFastmem)
                EmuConfig.Cpu.Recompiler.EnableFastmem = false;
            const int configuredCoreType = Host::GetBaseIntSettingValue("EmuCore/CPU", "CoreType", 2);
            const bool configuredUseArm64 = Host::GetBaseBoolSettingValue("EmuCore/CPU", "UseArm64Dynarec", configuredCoreType == 2);
            std::fprintf(stderr,
                "@@CPU_CONFIG@@ core=%d use_arm64=%d ee=%d iop=%d vu0=%d vu1=%d fastmem=%d mtvu=%d forced_interp=%d\n",
                configuredCoreType, configuredUseArm64 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableEE ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableIOP ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableVU0 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableVU1 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableFastmem ? 1 : 0,
                EmuConfig.Speedhacks.vuThread ? 1 : 0,
                DarwinMisc::iPSX2_FORCE_EE_INTERP ? 1 : 0);
            std::fflush(stderr);

            ARMSX2ApplyJITScriptProtocol("pre-vm-initialize");

            Error bootError;
            const VMBootResult bootResult = VMManager::Initialize(boot_params, &bootError);
            const std::string bootErrorText = bootError.GetDescription();
            std::fprintf(stderr, "@@BOOT_VM_INIT@@ result=%d success=%d error=\"%s\"\n",
                static_cast<int>(bootResult), bootResult == VMBootResult::StartupSuccess ? 1 : 0,
                bootErrorText.c_str());
            std::fflush(stderr);
            if (bootResult == VMBootResult::StartupSuccess) {
                // Identity and per-game settings are now valid, but the CPU has
                // not executed any game or BIOS frames. No UI polling is needed.
                if (!getenv("ARMSX2_BOOT_ELF")) {
                    @autoreleasepool {
                        ARMSX2IOSCompleteGameBoot(bootGameName, loadLastSaveState);
                    }
                }
                ARMSX2IOSLogMemoryCardConfig("post-vm-initialize");
                ARMSX2ApplyIOSOsdPresetFromConfig("post-vm-initialize");
                // The one OSD apply point that runs with the GS open, so it pushes the overlay
                // position. Only safe on the VM thread, which MTGS::RunOnGSThread asserts on.
                if (MTGS::IsOpen())
                    MTGS::ApplySettings();
                VMManager::SetState(VMState::Running);

                while (true) {
                    Host::PumpMessagesOnCPUThread();

                    if (s_requestVMStop.load()) {
                        Console.WriteLn("[VM] VM Thread: stop requested from UI.");
                        break;
                    }
                    VMState state = VMManager::GetState();
                    if (state == VMState::Stopping || state == VMState::Shutdown) {
                        Console.WriteLn("[VM] VM Thread: shutdown signal received.");
                        break;
                    } else if (state == VMState::Running) {
                        VMManager::Execute();
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }

                VMManager::Shutdown(false);
            } else {
                Console.Error("VM Thread: VMManager::Initialize failed!");
                Host::ReportErrorAsync("Startup Error", "VM Initialization Failed.");
            }

            s_vmThreadActive.store(false);
            ARMSX2DiscardCPUThreadTasks();
            ARMSX2StartJITKeepalive(); // VM stopped — JIT idle, restart monitoring
            s_requestVMStop.store(false);
            Console.WriteLn("[VM] VM Thread: shutdown complete, posting notification");
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSVMDidShutdown object:nil];
            });
        } // end while(true) boot loop
    });
    vmThread.detach();
}

@end
