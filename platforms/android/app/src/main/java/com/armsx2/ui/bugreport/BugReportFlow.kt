// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.bugreport

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.text.format.Formatter
import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.EmuState
import com.armsx2.GameInfo
import com.armsx2.bugreport.BootFailure
import com.armsx2.bugreport.BugReport
import com.armsx2.bugreport.BundleItem
import com.armsx2.bugreport.GameControl
import com.armsx2.bugreport.PendingCrash
import com.armsx2.bugreport.ReportCode
import com.armsx2.bugreport.ReportFiles
import com.armsx2.bugreport.ReportKind
import com.armsx2.bugreport.ReportLimits
import com.armsx2.bugreport.ReportOrigin
import com.armsx2.bugreport.ReportOrigins
import com.armsx2.bugreport.ReportSession
import com.armsx2.bugreport.ReportStage
import com.armsx2.bugreport.UploadReason
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.WindowImpl
import com.armsx2.ui.common.ConfirmOverlay
import com.armsx2.ui.common.PadModal
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.ToggleRow
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

/** How long capturing and packing may take before the working screen offers a way out. */
private const val TOO_LONG_MS = 60_000L

/** One formatter for every date on these screens, made once. Used from the main thread only. */
internal val reportDateFormat: DateFormat = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)

/** The screens of the report flow, in order. */
enum class FlowStage { KIND, DESCRIBE, CONSENT, WORKING }

/** The choice raised from the pause menu's GS dump action. */
class GsDumpChoice(val saveOnly: () -> Unit)

/**
 * The state of the "Report a problem" flow, and the ways into it. One object so every entry point
 * (pause menu, GS dump choice, drawer, a game's menu, the crash prompt, My reports) starts the same
 * flow, and so the flow survives the screen that started it going away: [BugReportFlowHost] is
 * mounted once at the app root, like the confirmation host.
 */
object BugReportFlow {
    var session by mutableStateOf<ReportSession?>(null)
        private set
    var stage by mutableStateOf(FlowStage.KIND)
        private set

    /** The report being shown on the working screen (capturing, uploading, done). */
    var workingId by mutableStateOf<String?>(null)
        private set

    var gsDumpChoice by mutableStateOf<GsDumpChoice?>(null)
    var showMyReports by mutableStateOf(false)

    /** True while an entry point is reading the app's state off the main thread; the host says "Preparing". */
    var opening by mutableStateOf(false)
        private set
    private var openToken = 0

    /**
     * From the pause menu and the GS dump choice: the running game, with a save state and GS dump
     * available. Graphics is preselected unless [kind] says otherwise, since the scene on screen is
     * what the pause menu is best for. [keepDumpInSnaps] also saves the dump to the snaps folder,
     * which is what "Save and send" promises.
     */
    fun openInGame(context: Context, kind: ReportKind? = null, keepDumpInSnaps: Boolean = false): Boolean =
        openAsync(kind ?: ReportKind.GRAPHICS, keepDumpInSnaps) { ReportOrigins.running(context) }

    /** From the drawer. */
    fun openGeneral(context: Context): Boolean = openAsync(ReportKind.OTHER) { ReportOrigins.general(context) }

    /** From a game's menu in the library. Reads the disc for its CRC, so it suspends. */
    suspend fun openForGame(context: Context, game: GameInfo) {
        if (session != null || opening) return
        val token = beginOpening()
        val origin = withContext(Dispatchers.IO) { runCatching { ReportOrigins.libraryGame(context, game) }.getOrNull() }
        finishOpening(token, origin, ReportKind.OTHER, false)
    }

    /** From the crash prompt, or an unsent crash in My reports. False if a report is already being made. */
    fun openForCrash(pending: PendingCrash): Boolean = start(ReportOrigins.crash(pending), ReportKind.CRASH)

    /** From the "the game could not start" notice. The kind is preselected as a crash; the user can change it. */
    fun openForBootFailure(context: Context, failure: BootFailure): Boolean =
        openAsync(ReportKind.CRASH) { ReportOrigins.bootFailure(context, failure) }

    /** The user gave up waiting for an entry point to finish reading the app's state. */
    fun cancelOpening() {
        openToken++
        opening = false
    }

    private fun beginOpening(): Int {
        opening = true
        return ++openToken
    }

    private fun finishOpening(token: Int, origin: ReportOrigin?, kind: ReportKind, keepDump: Boolean) {
        if (token != openToken) return // cancelled meanwhile
        opening = false
        if (origin != null) start(origin, kind, keepDump)
    }

    /** Works out the origin off the main thread (it reads disk and the core), then starts the flow. */
    private fun openAsync(kind: ReportKind, keepDump: Boolean = false, makeOrigin: () -> ReportOrigin): Boolean {
        if (session != null || opening) return false
        val token = beginOpening()
        BugReport.scope.launch {
            val origin = runCatching { makeOrigin() }.getOrNull()
            finishOpening(token, origin, kind, keepDump)
        }
        return true
    }

    private fun start(origin: ReportOrigin, kind: ReportKind, keepDump: Boolean = false): Boolean {
        if (session != null) return false
        session = ReportSession(origin, kind).also { it.keepDumpCopy = keepDump }
        stage = FlowStage.KIND
        workingId = null
        return true
    }

    /** The user chose to send a crash that was saved earlier instead of describing a new one. */
    fun switchToSavedCrash(pending: PendingCrash) {
        val old = session ?: return
        BugReport.scope.launch { BugReport.discard(old) }
        session = ReportSession(ReportOrigins.crash(pending), ReportKind.CRASH).also { it.description = old.description }
        stage = FlowStage.DESCRIBE
    }

    fun goTo(next: FlowStage) {
        stage = next
    }

    /** Backs out. Throws away a half-made report; an upload already under way carries on. */
    fun close() {
        val s = session
        val working = workingId
        session = null
        workingId = null
        stage = FlowStage.KIND
        if (s != null && working == null) {
            BugReport.scope.launch { BugReport.discard(s) }
        }
    }

    fun beginWorking(id: String) {
        workingId = id
        stage = FlowStage.WORKING
    }
}

/** Pauses and resumes the real game for the collector. */
private object RuntimeGameControl : GameControl {
    override fun isPaused(): Boolean = MainActivityRuntime.eState.value == EmuState.PAUSED

    override fun resume() = MainActivityRuntime.resume()

    override fun pause() = MainActivityRuntime.pauseForOverlay()

    override fun awaitRunning(timeoutMs: Long): Boolean = awaitState(EmuState.RUNNING, timeoutMs)

    override fun awaitPaused(timeoutMs: Long): Boolean = awaitState(EmuState.PAUSED, timeoutMs)

    private fun awaitState(wanted: EmuState, timeoutMs: Long): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (MainActivityRuntime.eState.value != wanted) {
            if (System.currentTimeMillis() > deadline) return false
            Thread.sleep(10)
        }
        return true
    }
}

/**
 * Draws whichever screen of the flow is current. Mount once at the app root. Every screen is a
 * [PadModal], so a controller drives it; the modals have no window of their own.
 */
@Composable
fun BugReportFlowHost() {
    BugReportFlow.gsDumpChoice?.let { GsDumpChoiceModal(it) }
    BugReport.bootFailure?.let { BootFailureModal(it) }
    if (BugReportFlow.opening) OpeningModal()
    if (BugReportFlow.showMyReports) MyReportsModal(onClose = { BugReportFlow.showMyReports = false })
    val session = BugReportFlow.session ?: return
    when (BugReportFlow.stage) {
        FlowStage.KIND -> KindModal(session)
        FlowStage.DESCRIBE -> DescribeModal(session)
        FlowStage.CONSENT -> ConsentModal(session)
        FlowStage.WORKING -> BugReportFlow.workingId?.let { WorkingModal(it) }
    }
}

// ---- screens -------------------------------------------------------------------------------

@Composable
private fun KindModal(session: ReportSession) {
    val layer = "bugreport.kind"
    val kinds = listOf(
        Triple(ReportKind.CRASH, "bugreport.kind.crash", "bugreport.kind.crash.detail"),
        Triple(ReportKind.GRAPHICS, "bugreport.kind.graphics", "bugreport.kind.graphics.detail"),
        Triple(ReportKind.SLOW, "bugreport.kind.slow", "bugreport.kind.slow.detail"),
        Triple(ReportKind.OTHER, "bugreport.kind.other", "bugreport.kind.other.detail"),
    )
    val scope = rememberCoroutineScope()
    // A crash that was saved at the last launch and never sent: offered before a new one is described.
    var offer by remember { mutableStateOf<PendingCrash?>(null) }
    val next = {
        if (session.kind == ReportKind.CRASH && session.origin.crashDir == null) {
            scope.launch {
                val saved = withContext(Dispatchers.IO) { BugReport.latestUnsentCrash() }
                if (saved != null) offer = saved else BugReportFlow.goTo(FlowStage.DESCRIBE)
            }
        } else {
            BugReportFlow.goTo(FlowStage.DESCRIBE)
        }
        Unit
    }
    FlowCard(layer = layer, onDismiss = { BugReportFlow.close() }, initialFocusId = "$layer.${session.kind.wire}") {
        Title(str("bugreport.kind.title"))
        Spacer(Modifier.height(10.dp))
        for ((kind, label, detail) in kinds) {
            ChoiceRow(
                id = "$layer.${kind.wire}",
                label = str(label),
                detail = str(detail),
                selected = session.kind == kind,
                onClick = { session.chooseKind(kind) },
            )
            Spacer(Modifier.height(6.dp))
        }
        Spacer(Modifier.height(8.dp))
        Buttons(
            FlowButtonSpec(str("action.cancel"), "$layer.cancel") { BugReportFlow.close() },
            FlowButtonSpec(str("bugreport.action.next"), "$layer.next", primary = true, onClick = next),
        )
    }
    offer?.let { saved ->
        ConfirmOverlay(
            title = str("bugreport.crash.offer.title"),
            message = String.format(str("bugreport.crash.offer"), reportDateFormat.format(Date(saved.armedAt))),
            confirmLabel = str("bugreport.crash.offer.use"),
            dismissLabel = str("bugreport.crash.offer.new"),
            idPrefix = "bugreport.offer",
            onConfirm = {
                offer = null
                BugReportFlow.switchToSavedCrash(saved)
            },
            onDismiss = {
                offer = null
                BugReportFlow.goTo(FlowStage.DESCRIBE)
            },
        )
    }
}

@Composable
private fun DescribeModal(session: ReportSession) {
    val layer = "bugreport.describe"
    val fieldLabel = str("bugreport.describe.field")
    // A controller gets the key grid, with a page of punctuation; a finger gets the system keyboard,
    // which is what someone typing a sentence wants. The description survives going back and forward.
    fun edit(systemKeyboard: Boolean) {
        LibraryKeyboard.open(
            session.description,
            { session.description = it.take(ReportLimits.MAX_DESCRIPTION) },
            fieldLabel,
            punctuation = true,
            systemKeyboard = systemKeyboard,
        )
    }
    FlowCard(layer = layer, onDismiss = { BugReportFlow.goTo(FlowStage.KIND) }, initialFocusId = "$layer.field") {
        Title(str("bugreport.describe.title"))
        Spacer(Modifier.height(10.dp))
        Surface(
            onClick = { edit(systemKeyboard = true) },
            modifier = Modifier.fillMaxWidth().controllerFocusable("$layer.field", RoundedCornerShape(12.dp), onConfirm = { edit(systemKeyboard = false) }),
            shape = RoundedCornerShape(12.dp),
            color = MaterialTheme.colorScheme.surfaceVariant,
        ) {
            Column(Modifier.padding(horizontal = 12.dp, vertical = 10.dp)) {
                Text(fieldLabel, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(
                    session.description.ifEmpty { str("bugreport.describe.hint") },
                    style = MaterialTheme.typography.bodyLarge,
                    color = if (session.description.isEmpty()) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.heightIn(max = 160.dp).verticalScroll(rememberScrollState()),
                )
            }
        }
        Text(
            String.format(str("bugreport.describe.count"), session.description.length, ReportLimits.MAX_DESCRIPTION),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 4.dp),
        )
        Spacer(Modifier.height(12.dp))
        Buttons(
            FlowButtonSpec(str("bugreport.action.back"), "$layer.back") { BugReportFlow.goTo(FlowStage.KIND) },
            FlowButtonSpec(str("bugreport.action.next"), "$layer.next", primary = true) { BugReportFlow.goTo(FlowStage.CONSENT) },
        )
    }
}

@Composable
private fun ConsentModal(session: ReportSession) {
    val layer = "bugreport.consent"
    val context = LocalContext.current
    val scroll = rememberScrollState()

    // Gather the always-collected files now, so the list below shows real sizes. prepare() does not
    // throw; the flag is reset however this ends (including the screen being left mid-way).
    LaunchedEffect(session) {
        if (session.collector == null && !session.prepareFailed) {
            session.preparing = true
            try {
                withContext(Dispatchers.IO) { BugReport.prepare(session) }
            } finally {
                session.preparing = false
            }
        }
    }
    val items = session.consentItems()
    val total = session.totalBytes()
    val metered = remember { BugReport.isMetered(context) }

    // Opens on Cancel, the safe button, at the top of the list. Focusing a button at the bottom
    // scrolls it into view, so the list is put back at the top once that has happened.
    LaunchedEffect(Unit) {
        withFrameNanos { }
        withFrameNanos { }
        scroll.scrollTo(0)
    }

    FlowCard(layer = layer, onDismiss = { BugReportFlow.goTo(FlowStage.DESCRIBE) }, initialFocusId = "$layer.cancel", scroll = scroll) {
        Title(str("bugreport.consent.title"))
        Spacer(Modifier.height(8.dp))
        Text(str("bugreport.consent.intro"), style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.height(6.dp))
        if (session.preparing) {
            Text(str("bugreport.consent.gathering"), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        for (item in items) ItemLine(context, item)
        if (session.prepareFailed) {
            Spacer(Modifier.height(6.dp))
            Text(
                str(if (session.collector == null) "bugreport.consent.prepareFailed" else "bugreport.consent.prepareFailedSome"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }
        if (session.failedItems.isNotEmpty()) {
            Spacer(Modifier.height(6.dp))
            Text(str("bugreport.consent.failedHeader"), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            for ((name, why) in session.failedItems) {
                Text("${itemLabel(name)}: ${str(why.kind.key)}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        if (session.origin.canCapture) {
            Spacer(Modifier.height(8.dp))
            ToggleRow(
                label = str("bugreport.toggle.gsdump"),
                value = session.includeGsDump,
                description = str("bugreport.toggle.gsdump.detail"),
            ) {
                session.includeGsDump = it
                session.switchesTouched = true
            }
            ToggleRow(
                label = str("bugreport.toggle.savestate"),
                value = session.includeSavestate,
                description = str("bugreport.toggle.savestate.detail"),
            ) {
                session.includeSavestate = it
                session.switchesTouched = true
            }
        }
        Spacer(Modifier.height(8.dp))
        Text(
            String.format(str("bugreport.consent.total"), Formatter.formatShortFileSize(context, total)),
            style = MaterialTheme.typography.labelLarge,
        )
        // A game picked in the library is not running, so there is no scene to capture.
        if (!session.origin.canCapture && session.origin.crashDir == null &&
            (session.kind == ReportKind.GRAPHICS || session.kind == ReportKind.SLOW)
        ) {
            Spacer(Modifier.height(6.dp))
            Text(str("bugreport.consent.startGame"), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (metered && total > ReportLimits.METERED_NOTE_BYTES) {
            Text(
                String.format(str("bugreport.consent.metered"), Formatter.formatShortFileSize(context, total)),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
        Spacer(Modifier.height(6.dp))
        Text(str("bugreport.consent.retention"), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(12.dp))
        Buttons(
            FlowButtonSpec(str("action.cancel"), "$layer.cancel") { BugReportFlow.close() },
            FlowButtonSpec(str("bugreport.action.send"), "$layer.send", primary = true, enabled = !session.preparing && session.collector != null) {
                val id = session.collector?.dir?.name ?: return@FlowButtonSpec
                BugReportFlow.beginWorking(id)
                BugReport.scope.launch {
                    BugReport.submit(session, if (session.origin.canCapture) RuntimeGameControl else null)
                }
            },
        )
    }
}

@Composable
private fun WorkingModal(localId: String) {
    val layer = "bugreport.working"
    val context = LocalContext.current
    val progress = BugReport.progress[localId]
    val stage = progress?.stage ?: ReportStage.PREPARING
    val code = progress?.code.orEmpty()
    val running = stage == ReportStage.CAPTURING || stage == ReportStage.PREPARING
    val done = stage == ReportStage.DONE
    val shownCode = code.takeIf { it.isNotEmpty() }?.let { c -> ReportCode.normalize(c)?.let(ReportCode::format) ?: c }

    val scope = rememberCoroutineScope()
    // Whether the files a retry needs are still on the device. Looked up off the main thread.
    val canRetry by produceState(initialValue = false, localId, stage) {
        value = withContext(Dispatchers.IO) { BugReport.canRetry(localId) }
    }
    val retry = {
        scope.launch {
            val result = withContext(Dispatchers.IO) { BugReport.retry(localId) }
            if (result == BugReport.RetryResult.GONE) Toast.makeText(context, I18n.get("bugreport.retry.gone"), Toast.LENGTH_LONG).show()
        }
        Unit
    }

    // Capturing and packing normally take seconds. After a minute the game is probably stuck, and
    // the user is offered a way out that does not lose the report.
    var tooLong by remember(localId) { mutableStateOf(false) }
    LaunchedEffect(localId, running) {
        tooLong = false
        if (running) {
            delay(TOO_LONG_MS)
            tooLong = true
        }
    }

    FlowCard(
        layer = layer,
        // Capturing and packing cannot be interrupted; anything else just closes this screen.
        onDismiss = if (running) null else ({ BugReportFlow.close() }),
        initialFocusId = if (shownCode != null) "$layer.copy" else "$layer.close",
    ) {
        Title(
            when (stage) {
                ReportStage.CAPTURING -> str("bugreport.progress.capturing.title")
                ReportStage.PREPARING -> str("bugreport.progress.packing")
                ReportStage.UPLOADING -> str("bugreport.progress.uploading.title")
                ReportStage.WAITING -> str("bugreport.progress.waiting.title")
                ReportStage.WAITING_WIFI -> str("bugreport.progress.wifi.title")
                ReportStage.STOPPED -> str("bugreport.progress.stopped.title")
                ReportStage.DONE -> str("bugreport.done.title")
                ReportStage.FAILED -> str("bugreport.progress.failed.title")
            },
        )
        Spacer(Modifier.height(10.dp))
        when (stage) {
            ReportStage.CAPTURING -> Text(str("bugreport.progress.capturing"), style = MaterialTheme.typography.bodyMedium)
            ReportStage.PREPARING -> Text(str("bugreport.progress.preparing"), style = MaterialTheme.typography.bodyMedium)
            ReportStage.UPLOADING -> {
                val total = progress?.total ?: 0L
                val sent = progress?.done ?: 0L
                if (total > 0) {
                    LinearProgressIndicator(progress = { (sent.toFloat() / total).coerceIn(0f, 1f) }, modifier = Modifier.fillMaxWidth())
                    Spacer(Modifier.height(6.dp))
                    Text(
                        String.format(
                            str("bugreport.progress.uploading"),
                            Formatter.formatShortFileSize(context, sent),
                            Formatter.formatShortFileSize(context, total),
                        ),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                Spacer(Modifier.height(6.dp))
                Text(str("bugreport.progress.keepGoing"), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            ReportStage.WAITING -> {
                Text(str("bugreport.progress.waiting"), style = MaterialTheme.typography.bodyMedium)
                ReasonLine(progress?.reason)
            }
            ReportStage.WAITING_WIFI -> Text(str("bugreport.progress.wifi"), style = MaterialTheme.typography.bodyMedium)
            ReportStage.STOPPED -> Text(str("bugreport.progress.stopped"), style = MaterialTheme.typography.bodyMedium)
            ReportStage.DONE -> {
                val missing = progress?.missing.orEmpty()
                if (missing.isNotEmpty()) {
                    Text(
                        String.format(str("bugreport.done.missing"), missing.map { itemLabel(it) }.joinToString(", ")),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
            ReportStage.FAILED -> {
                Text(str("bugreport.progress.failed"), style = MaterialTheme.typography.bodyMedium)
                ReasonLine(progress?.reason)
            }
        }
        // A report that failed before it was saved has nothing to retry; say so rather than offer a button that does nothing.
        if (stage == ReportStage.FAILED && !canRetry) {
            Text(
                str("bugreport.progress.noRetry"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
        if (shownCode != null) {
            Spacer(Modifier.height(12.dp))
            Text(
                if (done) str("bugreport.done.code") else str("bugreport.progress.codeStill"),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                shownCode,
                fontSize = 30.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.primary,
            )
            if (done) {
                Spacer(Modifier.height(4.dp))
                Text(str("bugreport.done.paste"), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Spacer(Modifier.height(14.dp))
        if (running && tooLong) {
            Text(str("bugreport.progress.tooLong.title"), style = MaterialTheme.typography.titleSmall)
            Text(str("bugreport.progress.tooLong"), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.padding(top = 4.dp))
            Spacer(Modifier.height(10.dp))
            Buttons(
                FlowButtonSpec(str("action.cancel"), "$layer.cancel") {
                    BugReport.cancelSubmit(localId)
                    BugReportFlow.close()
                },
                FlowButtonSpec(str("bugreport.action.sendWithout"), "$layer.without", primary = true) {
                    BugReport.skipHeavy(localId)
                    tooLong = false
                },
            )
        }
        if (!running) {
            val specs = buildList {
                if (shownCode != null) add(FlowButtonSpec(str("bugreport.action.copy"), "$layer.copy") { copyCode(context, shownCode) })
                if (stage == ReportStage.UPLOADING) {
                    add(FlowButtonSpec(str("bugreport.action.stop"), "$layer.stop") { BugReport.stop(localId) })
                }
                if (stage == ReportStage.WAITING_WIFI && canRetry) {
                    add(FlowButtonSpec(str("bugreport.action.sendNow"), "$layer.sendNow", onClick = retry))
                }
                if ((stage == ReportStage.WAITING || stage == ReportStage.FAILED || stage == ReportStage.STOPPED) && canRetry) {
                    add(FlowButtonSpec(str("bugreport.action.retry"), "$layer.retry", onClick = retry))
                }
                add(FlowButtonSpec(str("bugreport.myReports.title"), "$layer.my") { BugReportFlow.showMyReports = true })
                add(FlowButtonSpec(str("bugreport.action.close"), "$layer.close", primary = true) { BugReportFlow.close() })
            }
            Buttons(*specs.toTypedArray())
        }
    }
}

/** One line saying why an upload is waiting or failed, in the app's words. Nothing for no reason. */
@Composable
internal fun ReasonLine(reason: UploadReason?) {
    if (reason == null) return
    Text(
        str(reason.key),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 4.dp),
    )
}

@Composable
private fun GsDumpChoiceModal(choice: GsDumpChoice) {
    val layer = "bugreport.gsdump"
    val context = LocalContext.current
    val dismiss = { BugReportFlow.gsDumpChoice = null }
    FlowCard(layer = layer, onDismiss = dismiss, initialFocusId = "$layer.save") {
        Title(str("bugreport.gsdump.title"))
        Spacer(Modifier.height(10.dp))
        ChoiceRow(
            id = "$layer.save", label = str("bugreport.gsdump.save"), detail = str("bugreport.gsdump.save.detail"), selected = false,
            onClick = {
                dismiss()
                choice.saveOnly()
            },
        )
        Spacer(Modifier.height(6.dp))
        ChoiceRow(
            id = "$layer.send", label = str("bugreport.gsdump.send"), detail = str("bugreport.gsdump.send.detail"), selected = false,
            onClick = {
                dismiss()
                BugReportFlow.openInGame(context, ReportKind.GRAPHICS, keepDumpInSnaps = true)
            },
        )
        Spacer(Modifier.height(10.dp))
        Buttons(FlowButtonSpec(str("action.cancel"), "$layer.cancel", onClick = dismiss))
    }
}

/** Shown when the core reported that a game could not start: why, and a way to report it. */
@Composable
private fun BootFailureModal(failure: BootFailure) {
    val layer = "bugreport.boot"
    val context = LocalContext.current
    val close = { BugReport.bootFailure = null }
    FlowCard(layer = layer, onDismiss = close, initialFocusId = "$layer.close") {
        Title(str("bugreport.boot.title"))
        Spacer(Modifier.height(8.dp))
        Text(failure.message, style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.height(14.dp))
        Buttons(
            FlowButtonSpec(str("bugreport.action.close"), "$layer.close", onClick = close),
            FlowButtonSpec(str("bugreport.menu.title"), "$layer.report", primary = true) {
                if (BugReportFlow.openForBootFailure(context, failure)) close()
            },
        )
    }
}

/**
 * The library's prompt after a crash: what closed, which driver it was on, and whether to send it.
 * Shown in the library only; the evidence is kept until sent or a week has passed.
 */
@Composable
fun CrashReportPrompt() {
    val pending = BugReport.pendingCrash ?: return
    val context = LocalContext.current
    val layer = "bugreport.crash"
    // Not now: the evidence stays, and the line below says where to find it.
    val notNow = {
        BugReport.pendingCrash = null
        Toast.makeText(context, I18n.get("bugreport.crash.savedNote"), Toast.LENGTH_LONG).show()
    }
    FlowCard(layer = layer, onDismiss = notNow, initialFocusId = "$layer.send") {
        Title(str("bugreport.crash.title"))
        Spacer(Modifier.height(8.dp))
        Text(
            String.format(
                str(if (pending.closedBySystem) "bugreport.crash.messageSystem" else "bugreport.crash.message"),
                pending.title.ifBlank { str("bugreport.crash.unknownGame") },
                pending.driverLabel.ifBlank { str("bugreport.driver.system") },
            ),
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(14.dp))
        Buttons(
            FlowButtonSpec(str("bugreport.action.notNow"), "$layer.later", onClick = notNow),
            FlowButtonSpec(str("bugreport.action.sendReport"), "$layer.send", primary = true) {
                // Only forgotten once the flow has really opened; if another report is open, the prompt stays.
                if (BugReportFlow.openForCrash(pending)) BugReport.pendingCrash = null
            },
        )
    }
}

/** A short "Preparing" card while an entry point reads the app's state. Shown only if it takes a moment. */
@Composable
private fun OpeningModal() {
    val layer = "bugreport.opening"
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(250)
        visible = true
    }
    if (!visible) return
    FlowCard(layer = layer, onDismiss = { BugReportFlow.cancelOpening() }, initialFocusId = "$layer.cancel") {
        Title(str("bugreport.opening.title"))
        Spacer(Modifier.height(8.dp))
        Text(str("bugreport.opening"), style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.height(14.dp))
        Buttons(FlowButtonSpec(str("action.cancel"), "$layer.cancel") { BugReportFlow.cancelOpening() })
    }
}

// ---- pieces --------------------------------------------------------------------------------

@Composable
private fun ItemLine(context: Context, item: BundleItem) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.Top) {
        Text(
            itemLabel(item.name),
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f),
        )
        Spacer(Modifier.width(8.dp))
        val size = Formatter.formatShortFileSize(context, item.bytes)
        Text(
            if (item.estimate) String.format(str("bugreport.consent.estimate"), size) else size,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun itemLabel(name: String): String = str(
    when (name) {
        ReportFiles.REPORT -> "bugreport.item.report"
        ReportFiles.SYSTEM -> "bugreport.item.system"
        ReportFiles.EMULOG -> "bugreport.item.emulog"
        ReportFiles.EMULOG_PREVIOUS -> "bugreport.item.emulogPrevious"
        ReportFiles.LOGCAT -> "bugreport.item.logcat"
        ReportFiles.SETTINGS -> "bugreport.item.settings"
        ReportFiles.JAVA_CRASHES -> "bugreport.item.javacrashes"
        ReportFiles.EXIT_INFO -> "bugreport.item.exitinfo"
        ReportFiles.TOMBSTONE -> "bugreport.item.tombstone"
        ReportFiles.ANR -> "bugreport.item.anr"
        ReportFiles.GS_DUMP -> "bugreport.item.gsdump"
        ReportFiles.SAVESTATE -> "bugreport.item.savestate"
        else -> "bugreport.item.other"
    },
)

/** The shared card every screen of the flow is drawn on. */
@Composable
internal fun FlowCard(
    layer: String,
    onDismiss: (() -> Unit)?,
    initialFocusId: String?,
    scroll: androidx.compose.foundation.ScrollState? = null,
    content: @Composable () -> Unit,
) {
    val bodyScroll = scroll ?: rememberScrollState()
    PadModal(key = layer, onDismiss = onDismiss, initialFocusId = initialFocusId, scrollState = bodyScroll) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 520.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(20.dp).heightIn(max = 520.dp).verticalScroll(bodyScroll)) { content() }
        }
    }
}

@Composable
internal fun Title(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
}

internal class FlowButtonSpec(
    val label: String,
    val id: String,
    val primary: Boolean = false,
    val enabled: Boolean = true,
    val onClick: () -> Unit,
)

/** A row of buttons that wraps onto more lines on a narrow screen instead of squeezing or clipping. */
@Composable
internal fun Buttons(vararg specs: FlowButtonSpec) {
    FlowRow(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp, Alignment.End),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        for (spec in specs) FlowButton(spec)
    }
}

@Composable
private fun FlowButton(spec: FlowButtonSpec) {
    val click = { if (spec.enabled) spec.onClick() }
    Surface(
        onClick = click,
        enabled = spec.enabled,
        modifier = Modifier.controllerFocusable(spec.id, RoundedCornerShape(14.dp), onConfirm = click),
        shape = RoundedCornerShape(14.dp),
        color = if (spec.primary) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Text(
            spec.label,
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
            style = MaterialTheme.typography.labelLarge,
            color = (if (spec.primary) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant)
                .copy(alpha = if (spec.enabled) 1f else 0.5f),
        )
    }
}

/** A row that picks one option, with a ring when it is the current choice. */
@Composable
internal fun ChoiceRow(id: String, label: String, detail: String?, selected: Boolean, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth().controllerFocusable(id, RoundedCornerShape(14.dp), onConfirm = onClick),
        shape = RoundedCornerShape(14.dp),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = if (selected) 0.8f else 0.35f)),
    ) {
        Row(Modifier.padding(horizontal = 14.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(if (selected) "◉" else "○", fontSize = 18.sp, modifier = Modifier.width(28.dp))
            Column(Modifier.weight(1f)) {
                Text(label, style = MaterialTheme.typography.titleSmall)
                if (detail != null) {
                    Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}

internal fun copyCode(context: Context, code: String) {
    val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText("ARMSX2 report code", code))
    Toast.makeText(context, I18n.get("bugreport.copied"), Toast.LENGTH_SHORT).show()
}

/**
 * Entry used by the pause menu's GS dump action and the Renderer page: asks whether to just save the
 * dump or to send it. [saveOnly] is what the action did before this choice existed.
 */
fun askGsDumpChoice(saveOnly: () -> Unit) {
    BugReportFlow.gsDumpChoice = GsDumpChoice(saveOnly)
}
