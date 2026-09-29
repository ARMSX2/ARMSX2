package com.armsx2.ui.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.memcard.OnlineIcons
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * "Online Icons", from the library menu: the PS2 Icon Open Database's save icons, downloaded once,
 * with the size before and a progress bar and percentage while it runs. The download goes on if
 * this is closed ([OnlineIcons.start] runs it by itself); opening it again shows where it is.
 */
@Composable
internal fun OnlineIconsPrompt(onClose: () -> Unit) {
    val status = OnlineIcons.status.value
    val generation = OnlineIcons.generation.intValue
    val installed = remember(generation) { OnlineIcons.installed }
    val count by produceState(0, generation) {
        value = withContext(Dispatchers.IO) { if (OnlineIcons.installed) OnlineIcons.iconCount() else 0 }
    }
    // The server's set: its size, and whether it is newer than the one here. Null while asking and
    // when it can't be reached; Download still tries then.
    var remote by remember { mutableStateOf<OnlineIcons.Remote?>(null) }
    var asked by remember { mutableStateOf(false) }
    LaunchedEffect(generation) {
        remote = OnlineIcons.remote()
        asked = true
    }
    val busy = status is OnlineIcons.Status.Downloading || status is OnlineIcons.Status.Checking
    val update = remote?.let { OnlineIcons.isUpdate(it) } == true

    val sizeFormat = str("onlineicons.size")
    val readyFormat = str("onlineicons.ready")
    val progressFormat = str("onlineicons.progress")
    val failedFormat = str("onlineicons.failed")
    val focus = when {
        busy -> "online-icons.cancel"
        !installed || update || status is OnlineIcons.Status.Failed -> "online-icons.download"
        else -> "online-icons.ok"
    }
    com.armsx2.ui.common.PadModal(key = "online-icons", onDismiss = onClose, initialFocusId = focus) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 520.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(22.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(str("onlineicons.title"), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text(
                    str("onlineicons.body"),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                when (status) {
                    is OnlineIcons.Status.Downloading -> {
                        val fraction = if (status.total > 0) (status.done.toFloat() / status.total).coerceIn(0f, 1f) else null
                        if (fraction != null) {
                            LinearProgressIndicator(progress = { fraction }, modifier = Modifier.fillMaxWidth())
                        } else {
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                        }
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            Text(fraction?.let { "${(it * 100).toInt()}%" } ?: "", fontWeight = FontWeight.Bold)
                            Text(
                                if (status.total > 0) {
                                    progressFormat.replace("%1\$s", megabytes(status.done)).replace("%2\$s", megabytes(status.total))
                                } else {
                                    megabytes(status.done)
                                },
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    OnlineIcons.Status.Checking -> {
                        LinearProgressIndicator(Modifier.fillMaxWidth())
                        Text(str("onlineicons.checking"), color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    is OnlineIcons.Status.Failed -> Text(failedFormat.replace("%s", status.why), color = MaterialTheme.colorScheme.error)
                    OnlineIcons.Status.Idle -> {
                        val r = remote
                        Text(
                            when {
                                installed && update && r != null -> str("onlineicons.updateAvailable") + " " + sizeFormat.replace("%s", megabytes(r.bytes))
                                installed -> readyFormat.replace("%s", "%,d".format(count))
                                r != null -> sizeFormat.replace("%s", megabytes(r.bytes))
                                asked -> str("onlineicons.offline")
                                else -> str("onlineicons.asking")
                            },
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    if (busy) {
                        TextButton(
                            onClick = { OnlineIcons.cancel() },
                            modifier = Modifier.controllerFocusable(controllerId = "online-icons.cancel", onConfirm = { OnlineIcons.cancel() }),
                        ) { Text(str("action.cancel")) }
                    } else {
                        if (installed) {
                            TextButton(
                                onClick = { OnlineIcons.remove() },
                                modifier = Modifier.controllerFocusable(controllerId = "online-icons.delete", onConfirm = { OnlineIcons.remove() }),
                            ) { Text(str("action.delete")) }
                        }
                        if (!installed || update || status is OnlineIcons.Status.Failed) {
                            TextButton(
                                onClick = { OnlineIcons.start() },
                                modifier = Modifier.controllerFocusable(controllerId = "online-icons.download", onConfirm = { OnlineIcons.start() }),
                            ) { Text(str(if (installed) "onlineicons.update" else "onlineicons.download")) }
                        }
                    }
                    TextButton(
                        onClick = onClose,
                        modifier = Modifier.controllerFocusable(controllerId = "online-icons.ok", onConfirm = onClose),
                    ) { Text(str(if (busy) "onlineicons.hide" else "action.ok")) }
                }
            }
        }
    }
}

/** A download size the way people say it: "63 MB", "3.2 MB". */
private fun megabytes(bytes: Long): String {
    val mb = bytes / 1_000_000.0
    return if (mb < 10) "%.1f MB".format(mb) else "%.0f MB".format(mb)
}
