// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The dependency of needs_dep.c. The test copies both into one folder, loads the pair, then
// deletes this one to reproduce a pack whose shared-library dependency is missing.
__attribute__((visibility("default"))) int catalog_test_dep(void)
{
	return 2;
}
