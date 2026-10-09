// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A shared library that loads cleanly: what a working driver pack looks like to dlopen.
__attribute__((visibility("default"))) int catalog_test_ok(void)
{
	return 1;
}
