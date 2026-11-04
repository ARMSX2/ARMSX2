// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.bugreport

import android.text.format.Formatter
import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.bugreport.BugReport
import com.armsx2.bugreport.HistoryEntry
import com.armsx2.bugreport.HistoryStatus
import com.armsx2.bugreport.ReportCode
import com.armsx2.bugreport.ReportKind
import com.armsx2.bugreport.ReportStage
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.common.GlobalConfirm
import com.armsx2.ui.settings.SettingsControllerNav
import java.util.Date
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** What is still on the device for a report: its folder, and the state file a retry needs. */
private class LocalFiles(val folder: Boolean, val state: Boolean)

/**
 * Past reports: code, date, kind, game and state, with Copy and Retry. This is where someone who
 * closed the "done" screen without writing the code down gets it back.
 */
@Composable
fun MyReportsModal(onClose: () -> Unit) {
    val layer = "bugreport.my"
    val entries = BugReport.history.list()
    val scope = rememberCoroutineScope()

    // After Retry or Delete a row's buttons change or go. The pad is put back on the same row (a
    // row whose buttons still exist) or, if the row went, the next one, rather than dropping to the
    // top of the list. An empty string means "the Close button".
    var refocus by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(refocus) {
        val row = refocus ?: return@LaunchedEffect
        repeat(10) {
            withFrameNanos { }
            val ids = if (row.isEmpty()) listOf("$layer.close") else ROW_BUTTONS.map { "$layer.$row.$it" }
            if (ids.any { SettingsControllerNav.selectById(it) }) {
                refocus = null
                return@LaunchedEffect
            }
        }
        if (!SettingsControllerNav.selectById("$layer.close")) SettingsControllerNav.selectFirstInLayer()
        refocus = null
    }
    val nextRowAfter = { id: String ->
        val i = entries.indexOfFirst { it.localId == id }
        (entries.getOrNull(i + 1) ?: entries.getOrNull(i - 1))?.localId.orEmpty()
    }

    FlowCard(layer = layer, onDismiss = onClose, initialFocusId = if (entries.isEmpty()) "$layer.close" else null) {
        Title(str("bugreport.myReports.title"))
        Spacer(Modifier.height(8.dp))
        if (entries.isEmpty()) {
            Text(str("bugreport.myReports.empty"), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        for (entry in entries) {
            ReportRow(
                entry, layer, scope, onClose = onClose,
                keepFocusOnRow = { refocus = entry.localId },
                focusNextRow = { refocus = nextRowAfter(entry.localId) },
            )
            Spacer(Modifier.height(8.dp))
        }
        Spacer(Modifier.height(6.dp))
        Buttons(FlowButtonSpec(str("bugreport.action.close"), "$layer.close", primary = true, onClick = onClose))
    }
}

/** The buttons a row can have, most useful first: the pad lands on the first one that exists. */
private val ROW_BUTTONS = listOf("stop", "copy", "retry", "sendNow", "send", "delete")

@Composable
private fun ReportRow(
    entry: HistoryEntry,
    layer: String,
    scope: CoroutineScope,
    onClose: () -> Unit,
    keepFocusOnRow: () -> Unit,
    focusNextRow: () -> Unit,
) {
    val context = LocalContext.current
    // Only this report's own progress is read, so another report's upload does not redraw this line.
    val live = BugReport.progress[entry.localId]
    val code = entry.code.ifEmpty { live?.code.orEmpty() }
    val shownCode = code.takeIf { it.isNotEmpty() }?.let { c -> ReportCode.normalize(c)?.let(ReportCode::format) ?: c }
    val status = when {
        live?.stage == ReportStage.UPLOADING && entry.status != HistoryStatus.SENT -> HistoryStatus.UPLOADING
        else -> entry.status
    }
    val files by produceState(LocalFiles(folder = false, state = false), entry.localId, status) {
        value = withContext(Dispatchers.IO) {
            LocalFiles(BugReport.localDir(entry.localId) != null, BugReport.canRetry(entry.localId))
        }
    }

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.35f)),
    ) {
        Column(Modifier.padding(12.dp)) {
            Row {
                Text(
                    shownCode ?: "—",
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.weight(1f),
                )
                Text(statusLabel(status, live), style = MaterialTheme.typography.labelLarge)
            }
            Text(
                listOf(kindLabel(entry.kind), entry.title.ifBlank { str("bugreport.myReports.noGame") }).joinToString(" · "),
                style = MaterialTheme.typography.bodyMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                listOf(
                    reportDateFormat.format(Date(entry.createdAt)),
                    Formatter.formatShortFileSize(context, entry.bytes),
                ).joinToString(" · "),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (entry.earlierCodes.isNotBlank()) {
                val earlier = entry.earlierCodes.split(',').filter { it.isNotBlank() }
                    .joinToString(", ") { c -> ReportCode.normalize(c)?.let(ReportCode::format) ?: c }
                Text(
                    String.format(str("bugreport.myReports.earlier"), earlier),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            noteText(entry.note)?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 3, overflow = TextOverflow.Ellipsis)
            }
            val id = entry.localId
            val actions = buildList {
                if (shownCode != null) add(FlowButtonSpec(str("bugreport.action.copy"), "$layer.$id.copy") { copyCode(context, shownCode) })
                if (files.state && (status == HistoryStatus.UPLOADING || status == HistoryStatus.WAITING_RETRY || status == HistoryStatus.WAITING_WIFI)) {
                    add(
                        FlowButtonSpec(str("bugreport.action.stop"), "$layer.$id.stop") {
                            keepFocusOnRow()
                            scope.launch(Dispatchers.IO) { BugReport.stop(id) }
                        },
                    )
                }
                when (status) {
                    HistoryStatus.WAITING_WIFI -> if (files.state) {
                        add(
                            FlowButtonSpec(str("bugreport.action.sendNow"), "$layer.$id.sendNow") {
                                keepFocusOnRow()
                                retryInBackground(scope, context, id)
                            },
                        )
                    }
                    HistoryStatus.WAITING_RETRY, HistoryStatus.FAILED, HistoryStatus.STOPPED -> if (files.state) {
                        add(
                            FlowButtonSpec(str("bugreport.action.retry"), "$layer.$id.retry") {
                                keepFocusOnRow()
                                retryInBackground(scope, context, id)
                            },
                        )
                    }
                    HistoryStatus.NOT_SENT -> if (files.folder) {
                        add(
                            FlowButtonSpec(str("bugreport.action.sendReport"), "$layer.$id.send") {
                                scope.launch {
                                    val pending = withContext(Dispatchers.IO) { BugReport.crashFor(id) }
                                    if (pending != null) {
                                        onClose()
                                        BugReportFlow.close()
                                        BugReportFlow.openForCrash(pending)
                                    }
                                }
                            },
                        )
                    }
                    else -> Unit
                }
                if (status == HistoryStatus.NOT_SENT || status == HistoryStatus.FAILED || status == HistoryStatus.STOPPED) {
                    add(
                        FlowButtonSpec(str("bugreport.action.delete"), "$layer.$id.delete") {
                            // Deleting throws the files away; ask first. Cancel is the focused button.
                            GlobalConfirm.ask(
                                title = I18n.get("bugreport.delete.title"),
                                message = I18n.get("bugreport.delete.message"),
                                confirmLabel = I18n.get("bugreport.action.delete"),
                                destructive = true,
                            ) {
                                focusNextRow()
                                scope.launch(Dispatchers.IO) { BugReport.delete(id) }
                            }
                        },
                    )
                }
            }
            if (actions.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                Buttons(*actions.toTypedArray())
            }
        }
    }
}

/** Retry or Send now, off the main thread; says so if the files are gone. */
private fun retryInBackground(scope: CoroutineScope, context: android.content.Context, localId: String) {
    scope.launch {
        val result = withContext(Dispatchers.IO) { BugReport.retry(localId) }
        if (result == BugReport.RetryResult.GONE) {
            Toast.makeText(context, I18n.get("bugreport.retry.gone"), Toast.LENGTH_LONG).show()
        }
    }
}

@Composable
private fun statusLabel(status: HistoryStatus, live: com.armsx2.bugreport.ReportProgress?): String = when (status) {
    HistoryStatus.NOT_SENT -> str("bugreport.status.notSent")
    HistoryStatus.UPLOADING -> {
        val total = live?.total ?: 0L
        if (total > 0) String.format(str("bugreport.status.uploadingPct"), ((live?.done ?: 0L) * 100 / total).toInt())
        else str("bugreport.status.uploading")
    }
    HistoryStatus.WAITING_RETRY -> str("bugreport.status.waiting")
    HistoryStatus.WAITING_WIFI -> str("bugreport.status.wifi")
    HistoryStatus.STOPPED -> str("bugreport.status.stopped")
    HistoryStatus.SENT -> str("bugreport.status.sent")
    HistoryStatus.FAILED -> str("bugreport.status.failed")
}

/**
 * A history line's note in the app's words. Notes are string keys; one from an earlier build that
 * stored English text is not shown.
 */
@Composable
private fun noteText(note: String): String? = note.takeIf { it.startsWith("bugreport.") }?.let { str(it) }

@Composable
private fun kindLabel(wire: String): String = str(
    when (ReportKind.fromWire(wire)) {
        ReportKind.CRASH -> "bugreport.kind.crash"
        ReportKind.GRAPHICS -> "bugreport.kind.graphics"
        ReportKind.SLOW -> "bugreport.kind.slow"
        ReportKind.OTHER -> "bugreport.kind.other"
    },
)
