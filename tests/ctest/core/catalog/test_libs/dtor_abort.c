// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include <stdlib.h>

// Loads fine, aborts when it is unloaded (dlclose).
__attribute__((destructor)) static void catalog_test_dtor_abort(void)
{
	abort();
}

__attribute__((visibility("default"))) int catalog_test_dtor_abort_marker(void)
{
	return 0;
}
