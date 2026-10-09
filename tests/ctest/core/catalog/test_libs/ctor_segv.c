// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A driver whose static constructor dereferences null.
__attribute__((constructor)) static void catalog_test_ctor_segv(void)
{
	*(volatile int*)0 = 1;
}

__attribute__((visibility("default"))) int catalog_test_ctor_segv_marker(void)
{
	return 0;
}
