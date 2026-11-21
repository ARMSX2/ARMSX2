// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.common

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.DriverUpdateOffer
import com.armsx2.DriverUpdateOffer.Stage
import com.armsx2.i18n.str

/**
 * The driver offer [DriverUpdateOffer] makes after an app update: the setup wizard's driver page,
 * as a modal over the library.
 *
 * The primary button keeps one nav id through every stage, so the selection stays on it while the
 * download runs and lands on OK when it is done. "Not now" goes away once the download starts,
 * and the modal cannot be dismissed until it finishes.
 */
@Composable
fun DriverUpdateOverlay() {
    val offer = DriverUpdateOffer.pending.value ?: return
    val context = LocalContext.current
    val layer = "driver-update-offer"
    val bodyScroll = rememberScrollState()
    val primary = {
        when (offer.stage) {
            Stage.Offer, Stage.Failed -> DriverUpdateOffer.install(context)
            Stage.Installed -> DriverUpdateOffer.close()
            Stage.Installing -> {}
        }
    }
    PadModal(
        key = layer,
        onDismiss = when (offer.stage) {
            Stage.Installing -> null
            Stage.Installed -> DriverUpdateOffer::close
            else -> DriverUpdateOffer::decline
        },
        initialFocusId = "$layer.primary",
        scrollState = bodyScroll,
    ) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 460.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(20.dp)) {
                Text(
                    str(if (offer.upgrade) "driverOffer.title.update" else "driverOffer.title.switch"),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(8.dp))
                Column(
                    Modifier
                        .heightIn(max = 340.dp)
                        .verticalScroll(bodyScroll),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Text(
                        str(if (offer.upgrade) "driverOffer.body.update" else "setup.step.driver.description"),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        str("driverOffer.current").format(offer.currentName ?: str("setup.gpuDriver.default.sublabel")),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    PackCard(offer)
                    when (offer.stage) {
                        Stage.Failed -> Text(str("setup.driver.failed"), color = MaterialTheme.colorScheme.error)
                        Stage.Installed -> Text(str("setup.driver.installed"), color = MaterialTheme.colorScheme.primary)
                        else -> {}
                    }
                    if (offer.mali) {
                        Text(
                            str("setup.driver.mali.note"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    if (offer.stage == Stage.Offer || offer.stage == Stage.Failed) {
                        Text(
                            str("setup.driver.skipHint"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                Spacer(Modifier.height(18.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    if (offer.stage == Stage.Offer || offer.stage == Stage.Failed) {
                        ConfirmButton(
                            label = str("driverOffer.notNow"),
                            id = "$layer.decline",
                            onClick = DriverUpdateOffer::decline,
                            container = MaterialTheme.colorScheme.surfaceVariant,
                            content = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Spacer(Modifier.width(10.dp))
                    }
                    ConfirmButton(
                        label = when (offer.stage) {
                            Stage.Offer -> str("setup.driver.download")
                            Stage.Failed -> str("setup.driver.retry")
                            Stage.Installing -> str("setup.driver.installing")
                            Stage.Installed -> str("action.ok")
                        },
                        id = "$layer.primary",
                        onClick = primary,
                        container = MaterialTheme.colorScheme.primaryContainer,
                        content = MaterialTheme.colorScheme.onPrimaryContainer,
                    )
                }
            }
        }
    }
}

/** Release name, then source, tag and size, as on the setup wizard's driver page. */
@Composable
private fun PackCard(offer: DriverUpdateOffer.Offer) {
    val pack = offer.pack
    val megabytes = pack.sizeBytes / (1024.0 * 1024.0)
    val detail = listOf(pack.source, pack.tagName, if (pack.sizeBytes > 0) "%.0f MB".format(megabytes) else "")
        .filter(String::isNotBlank).joinToString(" · ")
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.74f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.36f)),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(pack.title.ifBlank { pack.assetName }, style = MaterialTheme.typography.titleSmall)
            if (detail.isNotBlank()) {
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}
