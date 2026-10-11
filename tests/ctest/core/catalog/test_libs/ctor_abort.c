// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include <stdlib.h>

// A driver whose static constructor aborts. The test load must survive this.
__attribute__((constructor)) static void catalog_test_ctor_abort(void)
{
	abort();
}

__attribute__((visibility("default"))) int catalog_test_ctor_abort_marker(void)
{
	return 0;
}
