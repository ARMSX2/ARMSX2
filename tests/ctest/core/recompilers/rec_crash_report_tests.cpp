// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What happens when the process takes a fault nothing handles: the page fault
// handler gives up on it, and the process must die by that signal, not by an
// abort() of its own, so Android's tombstone and a Linux core dump show the
// fault itself.
//
// Each crash runs in a forked child, which writes its log to a file of its own
// and is waited for here. Linux only, as is the handler.

#if defined(__linux__)

#include "common/Console.h"
#include "common/CrashHandler.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Crash
{
	int status = 0;
	std::string log;    // the child's emulog
	std::string err; // and what it wrote to stderr
};

std::string ReadFd(int fd)
{
	std::string text;
	char buf[4096];
	for (;;)
	{
		const ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0)
			text.append(buf, static_cast<size_t>(n));
		else if (n == 0 || errno != EINTR)
			break;
	}
	return text;
}

std::string ReadFile(const std::string& path)
{
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return {};
	std::string text = ReadFd(fd);
	close(fd);
	return text;
}

// Runs crash() in a child with a log file of its own and no core dump.
template <typename F>
Crash RunCrash(F&& crash)
{
	const std::string path = testing::TempDir() + "rec_crash_report_" + std::to_string(getpid()) + ".log";
	int err_pipe[2];
	if (pipe(err_pipe) != 0)
		return {};

	const pid_t pid = fork();
	if (pid == 0)
	{
		close(err_pipe[0]);
		dup2(err_pipe[1], STDERR_FILENO);
		close(err_pipe[1]);
		prctl(PR_SET_DUMPABLE, 0);
		Log::SetFileOutputLevel(LOGLEVEL_INFO, path);
		crash();
		_exit(99);
	}

	close(err_pipe[1]);
	Crash c;
	c.err = ReadFd(err_pipe[0]);
	close(err_pipe[0]);
	waitpid(pid, &c.status, 0);
	c.log = ReadFile(path);
	unlink(path.c_str());
	return c;
}

void LoadFromAddress8()
{
	*reinterpret_cast<volatile int*>(static_cast<uintptr_t>(8));
}

} // namespace

// A fault in C++ code, outside the recompilers: still the real signal.
TEST(RecCrashReport, AnUnhandledFaultKillsTheProcessWithItsOwnSignal)
{
	const Crash c = RunCrash(LoadFromAddress8);
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
	EXPECT_NE(c.err.find("Aborting application."), std::string::npos) << c.err;
}

// A SIGSEGV someone sent is not raised again by retrying an instruction, so the
// handler sends it again.
TEST(RecCrashReport, ASentSignalIsSentAgain)
{
	const Crash c = RunCrash([] { raise(SIGSEGV); });
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
}

// The crash handler installed over the page fault handler (the wrong order):
// it hands the fault back to the page fault handler, which hands it back to
// the crash handler, which must then give up to the default action rather
// than go round again.
TEST(RecCrashReport, OurHandlersHandingTheFaultToEachOtherDoNotLoop)
{
	const Crash c = RunCrash([] {
		CrashHandler::Install();
		LoadFromAddress8();
	});
	ASSERT_TRUE(WIFSIGNALED(c.status)) << "status " << c.status;
	EXPECT_EQ(WTERMSIG(c.status), SIGSEGV);
}

#endif // defined(__linux__)
