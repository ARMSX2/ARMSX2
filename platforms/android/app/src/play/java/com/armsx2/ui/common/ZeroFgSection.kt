package com.armsx2.ui.common

import androidx.compose.runtime.Composable

/**
 * ZeroFG stub. The Play build has no ZeroFG — not the section, not its strings, not the native
 * engine — so this renders nothing and exists only to keep the shared call site compiling. The
 * real implementation is in the github source set.
 */
@Composable
@Suppress("UNUSED_PARAMETER")
fun ZeroFgSection(
    enabled: Boolean,
    mode: Int,
    onChange: (enabled: Boolean, mode: Int) -> Unit,
) {
}

@Composable
@Suppress("UNUSED_PARAMETER")
fun ZeroFgEmulationCard(
    enabled: Boolean,
    mode: Int,
    onChange: (enabled: Boolean, mode: Int) -> Unit,
) {
}
