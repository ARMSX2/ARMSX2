// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include <fcntl.h>
#include <stdlib.h>

// A driver whose static constructor aborts when the process holds more than one descriptor above stderr. The
// test-load child keeps only the pipe it answers on, so a driver (or the vendor library it pulls in) never
// runs with the emulator's open files, sockets and save-state handles in reach.
__attribute__((constructor)) static void catalog_test_ctor_fds(void)
{
	int open_fds = 0;
	for (int fd = 3; fd < 4096; fd++)
	{
		if (fcntl(fd, F_GETFD) != -1)
			open_fds++;
	}
	if (open_fds > 1)
		abort();
}

__attribute__((visibility("default"))) int catalog_test_ctor_fds_marker(void)
{
	return 0;
}
