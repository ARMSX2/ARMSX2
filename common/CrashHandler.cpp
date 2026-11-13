// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Pcsx2Defs.h"
#include "CrashHandler.h"
#include "DynamicLibrary.h"
#include "FileSystem.h"
#include "StringUtil.h"
#include <cinttypes>
#include <cstdio>
#include <ctime>

#if defined(_WIN32)
#include "RedtapeWindows.h"

#include "StackWalker.h"
#include <DbgHelp.h>

class CrashHandlerStackWalker : public StackWalker
{
public:
	explicit CrashHandlerStackWalker(HANDLE out_file);
	~CrashHandlerStackWalker();

protected:
	void OnOutput(LPCSTR szText) override;

private:
	HANDLE m_out_file;
};

CrashHandlerStackWalker::CrashHandlerStackWalker(HANDLE out_file)
	: StackWalker(RetrieveVerbose, nullptr, GetCurrentProcessId(), GetCurrentProcess())
	, m_out_file(out_file)
{
}

CrashHandlerStackWalker::~CrashHandlerStackWalker() = default;

void CrashHandlerStackWalker::OnOutput(LPCSTR szText)
{
	if (m_out_file)
	{
		DWORD written;
		WriteFile(m_out_file, szText, static_cast<DWORD>(std::strlen(szText)), &written, nullptr);
	}

	OutputDebugStringA(szText);
}

static bool WriteMinidump(HMODULE hDbgHelp, HANDLE hFile, HANDLE hProcess, DWORD process_id, DWORD thread_id,
	PEXCEPTION_POINTERS exception, MINIDUMP_TYPE type)
{
	using PFNMINIDUMPWRITEDUMP =
		BOOL(WINAPI*)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
			PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
			PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

	PFNMINIDUMPWRITEDUMP minidump_write_dump =
		hDbgHelp ? reinterpret_cast<PFNMINIDUMPWRITEDUMP>(GetProcAddress(hDbgHelp, "MiniDumpWriteDump")) : nullptr;
	if (!minidump_write_dump)
		return false;

	MINIDUMP_EXCEPTION_INFORMATION mei = {};
	if (exception)
	{
		mei.ThreadId = thread_id;
		mei.ExceptionPointers = exception;
		mei.ClientPointers = FALSE;
		return minidump_write_dump(hProcess, process_id, hFile, type, &mei, nullptr, nullptr);
	}

	__try
	{
		RaiseException(EXCEPTION_INVALID_HANDLE, 0, 0, nullptr);
	}
	__except (WriteMinidump(hDbgHelp, hFile, GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId(),
				  GetExceptionInformation(), type),
		EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return true;
}

static std::wstring s_write_directory;
static DynamicLibrary s_dbghelp_module;
static bool s_in_crash_handler = false;

static void GenerateCrashFilename(wchar_t* buf, size_t len, const wchar_t* prefix, const wchar_t* extension)
{
	SYSTEMTIME st = {};
	GetLocalTime(&st);

	_snwprintf_s(buf, len, _TRUNCATE, L"%s%scrash-%04u-%02u-%02u-%02u-%02u-%02u-%03u.%s",
		prefix ? prefix : L"", prefix ? L"\\" : L"",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, extension);
}

static void WriteMinidumpAndCallstack(PEXCEPTION_POINTERS exi)
{
	s_in_crash_handler = true;

	wchar_t filename[1024] = {};
	GenerateCrashFilename(filename, std::size(filename), s_write_directory.empty() ? nullptr : s_write_directory.c_str(), L"txt");

	// might fail
	HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (exi && hFile != INVALID_HANDLE_VALUE)
	{
		char line[1024];
		DWORD written;
		std::snprintf(line, std::size(line), "Exception 0x%08X at 0x%p\n", static_cast<unsigned>(exi->ExceptionRecord->ExceptionCode),
			exi->ExceptionRecord->ExceptionAddress);
		WriteFile(hFile, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
	}

	GenerateCrashFilename(filename, std::size(filename), s_write_directory.empty() ? nullptr : s_write_directory.c_str(), L"dmp");

	const MINIDUMP_TYPE minidump_type =
		static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithProcessThreadData |
								   MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
	const HANDLE hMinidumpFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (hMinidumpFile == INVALID_HANDLE_VALUE ||
		!WriteMinidump(static_cast<HMODULE>(s_dbghelp_module.GetHandle()), hMinidumpFile, GetCurrentProcess(), GetCurrentProcessId(),
			GetCurrentThreadId(), exi, minidump_type))
	{
		static const char error_message[] = "Failed to write minidump file.\n";
		if (hFile != INVALID_HANDLE_VALUE)
		{
			DWORD written;
			WriteFile(hFile, error_message, sizeof(error_message) - 1, &written, nullptr);
		}
	}
	if (hMinidumpFile != INVALID_HANDLE_VALUE)
		CloseHandle(hMinidumpFile);

	CrashHandlerStackWalker sw(hFile);
	sw.ShowCallstack(GetCurrentThread(), exi ? exi->ContextRecord : nullptr);

	if (hFile != INVALID_HANDLE_VALUE)
		CloseHandle(hFile);
}

static LONG NTAPI ExceptionHandler(PEXCEPTION_POINTERS exi)
{
	// if the debugger is attached, or we're recursively crashing, let it take care of it.
	if (!s_in_crash_handler)
		WriteMinidumpAndCallstack(exi);

	// returning EXCEPTION_CONTINUE_SEARCH makes sense, except for the fact that it seems to leave zombie processes
	// around. instead, force ourselves to terminate.
	TerminateProcess(GetCurrentProcess(), 0xFEFEFEFEu);
	return EXCEPTION_CONTINUE_SEARCH;
}

bool CrashHandler::Install()
{
	// load dbghelp at install/startup, that way we're not LoadLibrary()'ing after a crash
	// .. because that probably wouldn't go down well.
	HMODULE mod = StackWalker::LoadDbgHelpLibrary();
	if (mod)
		s_dbghelp_module.Adopt(mod);

	SetUnhandledExceptionFilter(ExceptionHandler);
	return true;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
	s_write_directory = FileSystem::GetWin32Path(dump_directory);
}

void CrashHandler::WriteDumpForCaller()
{
	WriteMinidumpAndCallstack(nullptr);
}

#else

#include <signal.h>

#if !defined(__APPLE__)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

// How a crash ends on Linux and Android: our handlers log what they can, then
// put back the action that was installed before the first of them and return.
// The faulting instruction runs again and faults again, and this time the
// signal goes where it would have gone without us: the default action on a
// Linux desktop (a core dump, and the shell says "Segmentation fault"), or on
// Android debuggerd's handler, whose tombstone then records the real signal,
// fault address and registers. Calling abort() instead replaced all of that
// with a SIGABRT from inside the handler.
//
// On Android the "action installed before us" is the one ART's sigchain hands
// back: sigchain keeps its own handler in the kernel and treats ours as the
// app's action, so our sigaction() calls only swap the app's action, and the
// one PageFaultHandler::Install got back is debuggerd's. Putting that back from
// inside the handler is a sigaction() through sigchain again. When the fault
// repeats, sigchain asks ART first (ART declined the first time, and the pc is
// in our code, not ART's) and then calls debuggerd's handler with the
// faulting context.
namespace CrashHandler
{
	struct PreviousAction
	{
		struct sigaction action;
		bool known;
	};
	static PreviousAction s_previous[3] = {};

	// The thread that last put an old action back. If that thread comes back
	// here, the action we put back led here again (a handler of ours installed
	// over another of ours), and it gets the default action instead.
	static std::atomic<long> s_redelivered_by{0};

	static int Slot(int signal)
	{
		return signal == SIGSEGV ? 0 : signal == SIGBUS ? 1 : signal == SIGILL ? 2 : -1;
	}

	static long CurrentThreadId()
	{
#if defined(__linux__)
		return static_cast<long>(syscall(SYS_gettid));
#else
		return static_cast<long>(getpid());
#endif
	}

	static void WriteToStderr(const char* text)
	{
		size_t left = std::strlen(text);
		while (left > 0)
		{
			const ssize_t n = write(STDERR_FILENO, text, left);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			text += n;
			left -= static_cast<size_t>(n);
		}
	}

	static void Redeliver(int signal, siginfo_t* siginfo)
	{
		struct sigaction action = {};
		sigemptyset(&action.sa_mask);
		action.sa_handler = SIG_DFL;

		const long tid = CurrentThreadId();
		const int slot = Slot(signal);
		if (slot >= 0 && s_previous[slot].known && s_redelivered_by.exchange(tid) != tid)
			action = s_previous[slot].action;
		sigaction(signal, &action, nullptr);

		// A fault happens again when the instruction is retried; a signal
		// that was sent (kill, tgkill, sigqueue) does not, so send it again.
		// Our handlers are SA_NODEFER, so it arrives before this returns.
		if (siginfo && siginfo->si_code <= 0)
		{
#if defined(__linux__)
			if (syscall(SYS_rt_tgsigqueueinfo, getpid(), tid, signal, siginfo) != 0)
				raise(signal);
#else
			raise(signal);
#endif
		}
	}
} // namespace CrashHandler

void CrashHandler::RecordReplacedHandler(int signal, const struct sigaction& replaced)
{
	// Replacing our own crash handler: it already knows what came before it.
	if ((replaced.sa_flags & SA_SIGINFO) && replaced.sa_sigaction == CrashSignalHandler)
		return;

	const int slot = Slot(signal);
	if (slot < 0 || s_previous[slot].known)
		return;
	s_previous[slot].action = replaced;
	s_previous[slot].known = true;
}

#endif // !defined(__APPLE__)

#if !defined(__APPLE__) && defined(HAS_LIBBACKTRACE)

#include "FileSystem.h"

#include <backtrace.h>
#include <sys/mman.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace CrashHandler
{
	struct BacktraceBuffer
	{
		char* buffer;
		size_t used;
		size_t size;
	};

	static const char* GetSignalName(int signal_no);
	static void AllocateBuffer(BacktraceBuffer* buf);
	static void FreeBuffer(BacktraceBuffer* buf);
	static void AppendToBuffer(BacktraceBuffer* buf, const char* format, ...);
	static int BacktraceFullCallback(void* data, uintptr_t pc, const char* filename, int lineno, const char* function);
	static void LogCallstack(int signal, const void* exception_pc);

	static std::recursive_mutex s_crash_mutex;
	static bool s_in_signal_handler = false;

	static backtrace_state* s_backtrace_state = nullptr;
} // namespace CrashHandler

const char* CrashHandler::GetSignalName(int signal_no)
{
	switch (signal_no)
	{
		// Don't need to list all of them, there's only a couple we register.
		// clang-format off
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS: return "SIGBUS";
		case SIGILL: return "SIGILL";
		default: return "UNKNOWN";
			// clang-format on
	}
}

void CrashHandler::AllocateBuffer(BacktraceBuffer* buf)
{
	buf->used = 0;
	buf->size = __pagesize;
	buf->buffer = static_cast<char*>(mmap(nullptr, buf->size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
	if (buf->buffer == static_cast<char*>(MAP_FAILED))
	{
		buf->buffer = nullptr;
		buf->size = 0;
	}
}

void CrashHandler::FreeBuffer(BacktraceBuffer* buf)
{
	if (buf->buffer)
		munmap(buf->buffer, buf->size);
}

void CrashHandler::AppendToBuffer(BacktraceBuffer* buf, const char* format, ...)
{
	std::va_list ap;
	va_start(ap, format);

	// Hope this doesn't allocate memory... it *can*, but hopefully unlikely since
	// it won't be the first call, and we're providing the buffer.
	if (buf->size > 0 && buf->used < (buf->size - 1))
	{
		const int written = std::vsnprintf(buf->buffer + buf->used, buf->size - buf->used, format, ap);
		if (written > 0)
			buf->used += static_cast<size_t>(written);
	}

	va_end(ap);
}

int CrashHandler::BacktraceFullCallback(void* data, uintptr_t pc, const char* filename, int lineno,
	const char* function)
{
	BacktraceBuffer* buf = static_cast<BacktraceBuffer*>(data);
	AppendToBuffer(buf, "  %016p", pc);
	if (function)
		AppendToBuffer(buf, " %s", function);
	if (filename)
		AppendToBuffer(buf, " [%s:%d]", filename, lineno);

	AppendToBuffer(buf, "\n");
	return 0;
}

void CrashHandler::LogCallstack(int signal, const void* exception_pc)
{
	BacktraceBuffer buf;
	AllocateBuffer(&buf);
	if (signal != 0 || exception_pc)
		AppendToBuffer(&buf, "*************** Unhandled %s at %p ***************\n", GetSignalName(signal), exception_pc);
	else
		AppendToBuffer(&buf, "*******************************************************************\n");

	const int rc = backtrace_full(s_backtrace_state, 0, BacktraceFullCallback, nullptr, &buf);
	if (rc != 0)
		AppendToBuffer(&buf, "  backtrace_full() failed: %d\n");

	AppendToBuffer(&buf, "*******************************************************************\n");

	if (buf.used > 0)
		write(STDERR_FILENO, buf.buffer, buf.used);

	FreeBuffer(&buf);
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
	std::unique_lock lock(s_crash_mutex);

	// If we crash somewhere in libbacktrace, don't bother trying again. Without
	// Install() there is no backtrace state to log a callstack with.
	if (!s_in_signal_handler && s_backtrace_state)
	{
		s_in_signal_handler = true;

#if defined(__FreeBSD__) && defined(__x86_64__)
		void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
#elif defined(__x86_64__)
		void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
		void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
#else
		void* const exception_pc = nullptr;
#endif

		LogCallstack(signal, exception_pc);

		s_in_signal_handler = false;
	}

	lock.unlock();

	// The bug-report tooling looks for this line. The process does not abort
	// any more: the signal itself kills it (see Redeliver).
	WriteToStderr("Aborting application.\n");
	Redeliver(signal, siginfo);
}

bool CrashHandler::Install()
{
	const std::string progpath = FileSystem::GetProgramPath();
	s_backtrace_state = backtrace_create_state(progpath.empty() ? nullptr : progpath.c_str(), 0, nullptr, nullptr);
	if (!s_backtrace_state)
		return false;

	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sa.sa_sigaction = CrashSignalHandler;
	if (sigaction(SIGBUS, &sa, &s_previous[Slot(SIGBUS)].action) != 0)
		return false;
	s_previous[Slot(SIGBUS)].known = true;
	if (sigaction(SIGSEGV, &sa, &s_previous[Slot(SIGSEGV)].action) != 0)
		return false;
	s_previous[Slot(SIGSEGV)].known = true;
	if (sigaction(SIGILL, &sa, &s_previous[Slot(SIGILL)].action) != 0)
		return false;
	s_previous[Slot(SIGILL)].known = true;

	return true;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
}

void CrashHandler::WriteDumpForCaller()
{
	LogCallstack(0, nullptr);
}

#else

bool CrashHandler::Install()
{
	return false;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
}

void CrashHandler::WriteDumpForCaller()
{
}

#if defined(__APPLE__)

void CrashHandler::RecordReplacedHandler(int signal, const struct sigaction& replaced)
{
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
	// We can't continue from here. Just bail out and dump core.
	std::fputs("Aborting application.\n", stderr);
	std::fflush(stderr);
	std::abort();
}

#else

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
	// The bug-report tooling looks for this line. The process does not abort
	// any more: the signal itself kills it (see Redeliver).
	WriteToStderr("Aborting application.\n");
	Redeliver(signal, siginfo);
}

#endif

#endif

#endif
