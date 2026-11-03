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
import androidx.compose.runtime.remember
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
import java.text.DateFormat
import java.util.Date

/**
 * Past reports: code, date, kind, game and state, with Copy and Retry. This is where someone who
 * closed the "done" screen without writing the code down gets it back.
 */
@Composable
fun MyReportsModal(onClose: () -> Unit) {
    val layer = "bugreport.my"
    val entries = BugReport.history.list()

    FlowCard(layer = layer, onDismiss = onClose, initialFocusId = if (entries.isEmpty()) "$layer.close" else null) {
        Title(str("bugreport.myReports.title"))
        Spacer(Modifier.height(8.dp))
        if (entries.isEmpty()) {
            Text(str("bugreport.myReports.empty"), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        for (entry in entries) {
            ReportRow(entry, layer, onClose = onClose)
            Spacer(Modifier.height(8.dp))
        }
        Spacer(Modifier.height(6.dp))
        Buttons(FlowButtonSpec(str("bugreport.action.close"), "$layer.close", primary = true, onClick = onClose))
    }
}

@Composable
private fun ReportRow(entry: HistoryEntry, layer: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val live = BugReport.progress[entry.localId]
    val code = entry.code.ifEmpty { live?.code.orEmpty() }
    val shownCode = code.takeIf { it.isNotEmpty() }?.let { c -> ReportCode.normalize(c)?.let(ReportCode::format) ?: c }
    val status = when {
        live?.stage == ReportStage.UPLOADING && entry.status != HistoryStatus.SENT -> HistoryStatus.UPLOADING
        else -> entry.status
    }
    val exists = remember(entry.localId, status) { BugReport.localDir(entry.localId) != null }

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
                    DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(entry.createdAt)),
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
            val actions = buildList {
                if (shownCode != null) add(FlowButtonSpec(str("bugreport.action.copy"), "$layer.${entry.localId}.copy") { copyCode(context, shownCode) })
                if (exists && (status == HistoryStatus.UPLOADING || status == HistoryStatus.WAITING_RETRY || status == HistoryStatus.WAITING_WIFI)) {
                    add(FlowButtonSpec(str("bugreport.action.stop"), "$layer.${entry.localId}.stop") { BugReport.stop(entry.localId) })
                }
                when (status) {
                    HistoryStatus.WAITING_WIFI -> if (exists) {
                        add(FlowButtonSpec(str("bugreport.action.sendNow"), "$layer.${entry.localId}.sendNow") { BugReport.retry(entry.localId) })
                    }
                    HistoryStatus.WAITING_RETRY, HistoryStatus.FAILED, HistoryStatus.STOPPED -> if (exists) {
                        add(
                            FlowButtonSpec(str("bugreport.action.retry"), "$layer.${entry.localId}.retry") {
                                if (BugReport.retry(entry.localId) == BugReport.RetryResult.GONE) {
                                    Toast.makeText(context, I18n.get("bugreport.retry.gone"), Toast.LENGTH_LONG).show()
                                }
                            },
                        )
                    }
                    HistoryStatus.NOT_SENT -> if (exists) {
                        add(
                            FlowButtonSpec(str("bugreport.action.sendReport"), "$layer.${entry.localId}.send") {
                                val pending = BugReport.crashFor(entry.localId)
                                if (pending != null) {
                                    onClose()
                                    BugReportFlow.close()
                                    BugReportFlow.openForCrash(pending)
                                }
                            },
                        )
                    }
                    else -> Unit
                }
                if (status == HistoryStatus.NOT_SENT || status == HistoryStatus.FAILED || status == HistoryStatus.STOPPED) {
                    add(
                        FlowButtonSpec(str("bugreport.action.delete"), "$layer.${entry.localId}.delete") {
                            BugReport.delete(entry.localId)
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
