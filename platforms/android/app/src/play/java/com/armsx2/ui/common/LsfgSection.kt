package com.armsx2.ui.common

import androidx.compose.runtime.Composable

/**
 * Frame-generation stubs. The Play build has no frame generation of any kind — not the section,
 * not its strings, not the native library — so these render nothing and exist only to keep the
 * two shared call sites compiling.
 *
 * The real implementations are in the github source set. Deliberately arranged this way rather
 * than as a runtime flag in a shared file: a flag stops the rows being drawn, it does not stop
 * the code and the text shipping.
 */
@Composable
@Suppress("UNUSED_PARAMETER")
fun LsfgSection(
    enabled: Boolean,
    multiplier: Int,
    dllPath: String,
    performance: Boolean,
    flowScale: Int,
    onChange: (enabled: Boolean, multiplier: Int, dllPath: String, performance: Boolean, flowScale: Int) -> Unit,
) {
}

@Composable
@Suppress("UNUSED_PARAMETER")
fun LsfgEmulationCard(
    enabled: Boolean,
    multiplier: Int,
    dllPath: String,
    performance: Boolean,
    flowScale: Int,
    onChange: (enabled: Boolean, multiplier: Int, dllPath: String, performance: Boolean, flowScale: Int) -> Unit,
) {
}
