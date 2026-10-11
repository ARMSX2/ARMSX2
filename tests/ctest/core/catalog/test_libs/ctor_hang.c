// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include <unistd.h>

// A driver whose static constructor never returns.
__attribute__((constructor)) static void catalog_test_ctor_hang(void)
{
	for (;;)
		sleep(60);
}

__attribute__((visibility("default"))) int catalog_test_ctor_hang_marker(void)
{
	return 0;
}
