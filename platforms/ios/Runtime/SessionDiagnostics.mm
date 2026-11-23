// SessionDiagnostics.mm — the marker lines the session log carries besides the engine's own output.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <unistd.h>

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <time.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif

#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "IOSRuntime.h"

namespace
{
    std::atomic<bool> s_background{false};

    uint64_t s_startNs = 0;
    uint64_t s_prevNs = 0;
    uint64_t s_prevFrames = 0;
    dispatch_source_t s_timer = nil;
    dispatch_source_t s_memory = nil;

    double ElapsedSeconds()
    {
        return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - s_startNs) / 1e9;
    }

    unsigned FootprintMiB()
    {
        task_vm_info_data_t info = {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return 0;
        return static_cast<unsigned>(info.phys_footprint >> 20);
    }

    long long HeadroomMiB()
    {
#if defined(__APPLE__) && TARGET_OS_IPHONE
        return static_cast<long long>(os_proc_available_memory() >> 20);
#else
        return 0;
#endif
    }

    const char* ThermalName()
    {
        switch ([NSProcessInfo processInfo].thermalState) {
            case NSProcessInfoThermalStateFair: return "fair";
            case NSProcessInfoThermalStateSerious: return "serious";
            case NSProcessInfoThermalStateCritical: return "critical";
            default: return "nominal";
        }
    }

    void Tick()
    {
        const uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const uint64_t frames = PerformanceMetrics::GetFrameNumber();
        // The counter restarts at 0 on every new game.
        const uint64_t delta = frames >= s_prevFrames ? frames - s_prevFrames : frames;
        const double dt = static_cast<double>(ns - s_prevNs) / 1e9;
        fprintf(stderr, "@@DIAG_PERF@@ t=%.1f fps=%.1f state=%d bg=%d headroom_mib=%lld mem_mib=%u thermal=%s\n",
            static_cast<double>(ns - s_startNs) / 1e9, dt > 0 ? static_cast<double>(delta) / dt : 0.0,
            static_cast<int>(VMManager::GetState()), s_background.load() ? 1 : 0,
            HeadroomMiB(), FootprintMiB(), ThermalName());
        s_prevFrames = frames;
        s_prevNs = ns;
    }

    void ObserveAppPhase(NSNotificationName name, bool background)
    {
        [[NSNotificationCenter defaultCenter] addObserverForName:name object:nil
            queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
                s_background = background;
                fprintf(stderr, "@@DIAG_APP@@ t=%.1f phase=%s\n", ElapsedSeconds(), background ? "background" : "foreground");
            }];
    }

    std::atomic<bool> s_fatalWritten{false};
    NSUncaughtExceptionHandler* s_prevException = nullptr;
    std::terminate_handler s_prevTerminate = nullptr;
    struct sigaction s_prevAbort = {};

    void WriteFatalMarker(const char* line, size_t length)
    {
        if (!s_fatalWritten.exchange(true))
            (void)!write(STDERR_FILENO, line, length);
    }

    void OnException(NSException* exception)
    {
        static const char line[] = "@@DIAG_FATAL@@ kind=exception\n";
        WriteFatalMarker(line, sizeof(line) - 1);
        if (s_prevException)
            s_prevException(exception);
    }

    void OnTerminate()
    {
        static const char line[] = "@@DIAG_FATAL@@ kind=terminate\n";
        WriteFatalMarker(line, sizeof(line) - 1);
        if (s_prevTerminate)
            s_prevTerminate();
        abort();
    }

    void OnAbort(int)
    {
        static const char line[] = "@@DIAG_FATAL@@ kind=sigabrt\n";
        WriteFatalMarker(line, sizeof(line) - 1);
        sigaction(SIGABRT, &s_prevAbort, nullptr);
        raise(SIGABRT);
    }

    void InstallFatalHooks()
    {
        s_prevException = NSGetUncaughtExceptionHandler();
        NSSetUncaughtExceptionHandler(&OnException);
        s_prevTerminate = std::set_terminate(&OnTerminate);
        struct sigaction action = {};
        action.sa_handler = &OnAbort;
        sigemptyset(&action.sa_mask);
        sigaction(SIGABRT, &action, &s_prevAbort);
    }

}

void ARMSX2SessionDiagnosticsStart()
{
    if (s_timer)
        return;
    s_startNs = s_prevNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
    dispatch_queue_t queue = dispatch_queue_create("com.armsx2.session-diagnostics", attr);
    s_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(s_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
        100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(s_timer, ^{ Tick(); });
    dispatch_resume(s_timer);

    s_memory = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, queue);
    dispatch_source_set_event_handler(s_memory, ^{
        const bool critical = dispatch_source_get_data(s_memory) & DISPATCH_MEMORYPRESSURE_CRITICAL;
        fprintf(stderr, "@@DIAG_MEM@@ t=%.1f level=%s\n", ElapsedSeconds(), critical ? "critical" : "warn");
    });
    dispatch_resume(s_memory);

    ObserveAppPhase(UIApplicationDidEnterBackgroundNotification, true);
    ObserveAppPhase(UIApplicationWillEnterForegroundNotification, false);
    InstallFatalHooks();
}

namespace
{
    // One line per write, so the quoted title is cut on a character boundary.
    std::string CleanTitle(const std::string& title)
    {
        std::string out;
        for (const char c : title)
            out += c == '"' ? '\'' : (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
        if (out.size() > 120) {
            size_t n = 120;
            while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80)
                --n;
            out.resize(n);
        }
        return out;
    }

    std::string CleanSerial(const std::string& serial)
    {
        std::string out;
        for (const char c : serial)
            out += (isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80) || c == '.' || c == '_' || c == '-' ? c : '_';
        return out.empty() ? "-" : out;
    }

    std::string DiscFormat(const std::string& elf_override, const std::string& disc_path)
    {
        if (disc_path.empty())
            return elf_override.empty() ? "none" : "elf";
        const size_t dot = disc_path.rfind('.');
        if (dot == std::string::npos || disc_path.find('/', dot) != std::string::npos)
            return "none";
        std::string ext = disc_path.substr(dot + 1);
        for (char& c : ext)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext.empty())
            return "none";
        // Printed unquoted into a marker, so the value must come from a closed set.
        for (const char* known : {"iso", "bin", "chd", "cso", "zso", "gz", "img", "mdf", "nrg", "elf", "cue"})
        {
            if (ext == known)
                return ext;
        }
        return "other";
    }
}

void ARMSX2SessionDiagnosticsGame(const std::string& title, const std::string& elf_override, const std::string& disc_path,
    const std::string& serial, unsigned int disc_crc, unsigned int current_crc)
{
    // The host reports an empty game on shutdown.
    if (title.empty() && serial.empty() && elf_override.empty() && !disc_crc && !current_crc)
        return;
    fprintf(stderr, "@@DIAG_GAME@@ t=%.1f serial=%s crc=%08X cur_crc=%08X fmt=%s title=\"%s\"\n", ElapsedSeconds(),
        CleanSerial(serial).c_str(), disc_crc, current_crc, DiscFormat(elf_override, disc_path).c_str(),
        CleanTitle(title).c_str());
}

void ARMSX2SessionDiagnosticsStop()
{
    fprintf(stderr, "@@DIAG_STOP@@ t=%.1f\n", ElapsedSeconds());
}
