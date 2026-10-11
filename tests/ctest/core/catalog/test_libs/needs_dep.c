// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Links against dep.c's library (DT_NEEDED) and finds it through RUNPATH $ORIGIN.
extern int catalog_test_dep(void);

__attribute__((visibility("default"))) int catalog_test_needs_dep(void)
{
	return catalog_test_dep() + 1;
}
