// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

import androidx.compose.runtime.Composable
import com.armsx2.i18n.str
import com.armsx2.ui.common.NotifyOverlay

/** What there is to say about the last arcade launch ([Arcade.notice]), over whatever is on screen: why it
 *  did not start, or Soul Calibur II's new Conquest card. */
@Composable
fun ArcadeNotice() {
    val message = Arcade.notice.value ?: return
    NotifyOverlay(
        title = str("arcade.notice.title"),
        message = message,
        onDismiss = { Arcade.notice.value = null },
        idPrefix = "arcade.notice",
    )
}
