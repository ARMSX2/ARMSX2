// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <string_view>

#ifndef _WIN32
#include <csignal>
#endif

namespace CrashHandler
{
	bool Install();
	void SetWriteDirectory(std::string_view dump_directory);
	void WriteDumpForCaller();

#ifndef _WIN32
	// Allow crash handler to be invoked from a signal. Logs what it can, writes
	// "Aborting application." to stderr, and returns having put back the
	// action installed before our handlers, so that the retried instruction
	// faults again and the signal kills the process (on Apple it still calls
	// abort()). A handler that calls this must return straight after.
	void CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx);

	// A handler that replaced another one for SIGSEGV, SIGBUS or SIGILL passes the one
	// it replaced, for CrashSignalHandler to put back. The first one recorded
	// for a signal stays; our own crash handler is not recorded.
	void RecordReplacedHandler(int signal, const struct sigaction& replaced);
#endif
} // namespace CrashHandler
