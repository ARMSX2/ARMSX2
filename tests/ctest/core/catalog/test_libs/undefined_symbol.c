// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// References a function nothing provides. dlopen with RTLD_NOW fails; RTLD_LAZY would not.
extern int catalog_test_no_such_function(void);

__attribute__((visibility("default"))) int catalog_test_undefined_symbol(void)
{
	return catalog_test_no_such_function();
}
