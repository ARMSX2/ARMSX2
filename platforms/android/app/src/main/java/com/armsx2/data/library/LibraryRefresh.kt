// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.data.library

import androidx.compose.runtime.mutableIntStateOf

/**
 * A request for the library to rescan its folders, from a screen that changed what is in them
 * (the arcade screen, after an import). The library handles each request once, the next time it
 * is on screen, so the player does not have to refresh by hand.
 */
object LibraryRefresh {
    val requests = mutableIntStateOf(0)

    fun request() {
        requests.intValue++
    }
}
