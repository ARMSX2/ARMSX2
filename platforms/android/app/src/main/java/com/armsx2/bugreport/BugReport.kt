// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import android.os.Process
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.armsx2.CustomDriver
import java.io.File
import java.util.concurrent.Executors
import kotlin.concurrent.thread
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/** Where a report is in its life, for the progress screen and "My reports". */
enum class ReportStage { CAPTURING, PREPARING, UPLOADING, WAITING, DONE, FAILED }

/** What the progress screen shows for one report. */
data class ReportProgress(
    val localId: String,
    val stage: ReportStage,
    val done: Long = 0,
    val total: Long = 0,
    /** The report code, once the server has created the report. */
    val code: String = "",
    val message: String = "",
    val currentFile: String = "",
)

/** Evidence from a crash, waiting for the user to say whether to send it. */
data class PendingCrash(
    val dir: File,
    val title: String,
    val serial: String,
    val driverLabel: String,
    val armedAt: Long,
    val marker: CrashMarker.Marker,
)

/**
 * The problem-report feature's entry point and its process-wide state.
 *
 *  - [armCrashMarker] / [clearCrashMarker] bracket every game run (see [CrashMarker]).
 *  - [onDataRootReady] runs once the data folder is known, early in startup: it preserves the
 *    evidence of a crash, tidies old bundles and resumes uploads a previous run left unfinished.
 *  - [ReportSession] objects carry one report through the user flow; [prepare], [submit] and
 *    [discard] drive them.
 *  - Uploads run one at a time on a background thread and survive the user leaving the flow. If the
 *    process dies they resume on the next launch from `state.json`.
 */
object BugReport {
    private const val PREFS = "armsx2_bugreport"

    private lateinit var app: Context

    /** For work that outlives the screen that started it. Blocking calls belong here. */
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Process-wide. A report in flight is visible here to every screen that wants to show it. */
    val progress = mutableStateMapOf<String, ReportProgress>()

    /** Set at startup when the previous run crashed; the library asks whether to send it. */
    var pendingCrash by mutableStateOf<PendingCrash?>(null)

    val history: ReportHistory by lazy { ReportHistory(app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)) }

    private val uploader = Executors.newSingleThreadExecutor { r ->
        Thread(r, "bugreport-upload").apply { isDaemon = true }
    }
    private val active = java.util.Collections.synchronizedSet(HashSet<String>())

    fun init(context: Context) {
        app = context.applicationContext
    }

    // ---- crash marker ---------------------------------------------------------------------

    /** Records that a game is about to run. Call right before the VM thread starts. */
    fun armCrashMarker(serial: String, title: String, driverId: String) {
        val name = if (driverId.isBlank()) "" else {
            runCatching { CustomDriver.listInstalled(app).firstOrNull { it.id == driverId }?.name }.getOrNull().orEmpty()
        }
        CrashMarker.arm(app, CrashMarker.Marker(System.currentTimeMillis(), Process.myPid(), serial, title, driverId, name))
    }

    /** Records that the game stopped in an orderly way. */
    fun clearCrashMarker() = CrashMarker.clear(app)

    // ---- startup --------------------------------------------------------------------------

    /**
     * Called once the data folder is pinned and before any game can boot. Cheap on the calling
     * thread (the emulog is renamed, nothing is copied); the rest is done on a background thread.
     */
    fun onDataRootReady(dataRoot: String) {
        val root = File(dataRoot)
        val marker = CrashMarker.read(app)
        var preserved: CrashEvidence.Preserved? = null
        // A marker from this very process is a game that is running now (the activity was recreated).
        if (marker != null && marker.pid != Process.myPid()) {
            CrashMarker.clear(app)
            preserved = runCatching { CrashEvidence.preserve(app, root, marker) }.getOrNull()
        }
        thread(isDaemon = true, name = "bugreport-startup") {
            runCatching { Housekeeping.run(history, ReportCollector.bundleRoot(app)) }
            if (preserved != null) {
                runCatching { CrashEvidence.complete(app, root, preserved) }
                val driver = preserved.marker.driverName.ifBlank { preserved.marker.driverId }.ifBlank { "system" }
                history.upsert(
                    HistoryEntry(
                        localId = preserved.dir.name, code = "", kind = ReportKind.CRASH.wire,
                        createdAt = preserved.marker.armedAt, title = preserved.marker.title,
                        serial = preserved.marker.serial, status = HistoryStatus.NOT_SENT,
                        bytes = dirBytes(preserved.dir),
                    ),
                )
                pendingCrash = PendingCrash(
                    preserved.dir, preserved.marker.title, preserved.marker.serial, driver,
                    preserved.marker.armedAt, preserved.marker,
                )
            }
            resumePending()
        }
    }

    /** Starts every upload an earlier run left unfinished. */
    fun resumePending() {
        val root = ReportCollector.bundleRoot(app)
        val dirs = root.listFiles { f -> f.isDirectory && File(f, ReportUploader.STATE_FILE).isFile }.orEmpty()
        for (dir in dirs) {
            val state = UploadState.load(File(dir, ReportUploader.STATE_FILE)) ?: continue
            if (state.phase == ReportPhase.READY || state.phase == ReportPhase.UPLOADING || state.phase == ReportPhase.WAITING_RETRY) {
                enqueueUpload(dir)
            }
        }
    }

    // ---- one report -----------------------------------------------------------------------

    /**
     * Gathers the always-collected files so the consent screen can list them with their sizes.
     * Safe to call again once it has finished. Blocking: call it from a background thread.
     */
    fun prepare(session: ReportSession) {
        if (session.collector != null) return
        val origin = session.origin
        val dir = origin.crashDir ?: File(ReportCollector.bundleRoot(app), "report-${System.currentTimeMillis()}")
        if (origin.crashDir != null) {
            // The background half of the evidence may still be running; a crash report needs it.
            CrashEvidence.awaitComplete(dir, 45_000)
        }
        val root = com.armsx2.runtime.MainActivityRuntime.dataRootOrNull()
        val scrubber = Scrubber(listOfNotNull(root, "/storage/emulated/0/Android/data/${app.packageName}"))
        val collector = ReportCollector(app, dir, origin, scrubber)
        collector.collectLight()
        session.collector = collector
        session.lightItems = collector.collected.map { (name, bytes) -> BundleItem(name, bytes) }
        session.failedItems = collector.failed.toMap()
    }

    /**
     * Captures what was agreed, builds the bundle and starts the upload. Returns once the upload is
     * queued; progress is in [progress] under the session's folder name. Blocking: call it from a
     * background thread.
     */
    fun submit(session: ReportSession, control: GameControl?) {
        val collector = session.collector ?: return
        val localId = collector.dir.name
        val game = session.origin.game
        fun report(stage: ReportStage, message: String = "") {
            progress[localId] = ReportProgress(localId, stage, message = message)
        }

        var perfAfter: String? = null
        try {
            if (session.origin.canCapture && control != null && (session.includeSavestate || session.includeGsDump)) {
                report(ReportStage.CAPTURING)
                if (session.includeSavestate) collector.captureSavestate()
                if (session.includeGsDump) {
                    collector.captureGsDump(control)
                    perfAfter = runCatching { kr.co.iefriends.pcsx2.NativeApp.getPerfSnapshot() }.getOrNull()
                }
            }
            report(ReportStage.PREPARING)
            val state = collector.finish(session.kind, session.description.trim(), perfAfter)
            history.upsert(
                HistoryEntry(
                    localId = localId, code = "", kind = session.kind.wire, createdAt = state.createdAt,
                    title = game?.title.orEmpty(), serial = game?.serial.orEmpty(),
                    status = HistoryStatus.UPLOADING, bytes = state.totalBytes,
                ),
            )
            progress[localId] = ReportProgress(localId, ReportStage.UPLOADING, 0, state.totalBytes)
            enqueueUpload(collector.dir)
        } catch (t: Throwable) {
            progress[localId] = ReportProgress(localId, ReportStage.FAILED, message = t.message ?: t.javaClass.simpleName)
        }
    }

    /** Drops a report the user backed out of before sending. Crash evidence is kept for later. */
    fun discard(session: ReportSession) {
        val collector = session.collector ?: return
        session.collector = null
        if (session.origin.crashDir == null) collector.dir.deleteRecursively()
    }

    /** Puts an upload on the queue, unless that report is already queued or running. */
    fun enqueueUpload(dir: File) {
        val id = dir.name
        if (!active.add(id)) return
        uploader.execute {
            try {
                runUpload(dir)
            } finally {
                active.remove(id)
            }
        }
    }

    /** Tries a report again from "My reports". Returns false if its files are gone. */
    fun retry(localId: String): Boolean {
        val dir = File(ReportCollector.bundleRoot(app), localId)
        if (!File(dir, ReportUploader.STATE_FILE).isFile) return false
        val state = UploadState.load(File(dir, ReportUploader.STATE_FILE)) ?: return false
        if (state.phase == ReportPhase.FAILED) {
            state.phase = ReportPhase.WAITING_RETRY
            state.save(File(dir, ReportUploader.STATE_FILE))
        }
        progress[localId] = ReportProgress(localId, ReportStage.UPLOADING, state.bytesDone(), state.totalBytes, state.created?.code.orEmpty())
        enqueueUpload(dir)
        return true
    }

    /** The folder of a report still on the device, or null once it has been cleaned up. */
    fun localDir(localId: String): File? =
        File(ReportCollector.bundleRoot(app), localId).takeIf { it.isDirectory }

    /** The crash behind an unsent "My reports" line, rebuilt from its folder, or null if it is gone. */
    fun crashFor(localId: String): PendingCrash? {
        val dir = localDir(localId) ?: return null
        val marker = runCatching { CrashMarker.Marker.fromJson(org.json.JSONObject(File(dir, CrashEvidence.MARKER_FILE).readText())) }.getOrNull() ?: return null
        val driver = marker.driverName.ifBlank { marker.driverId }.ifBlank { "system" }
        return PendingCrash(dir, marker.title, marker.serial, driver, marker.armedAt, marker)
    }

    /** Removes a report's files and its "My reports" entry (crash evidence the user does not want). */
    fun delete(localId: String) {
        File(ReportCollector.bundleRoot(app), localId).deleteRecursively()
        history.remove(localId)
        progress.remove(localId)
    }

    private fun runUpload(dir: File) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
        val localId = dir.name
        var knownCode = ""
        var lastPublish = 0L
        val listener = object : ReportUploader.Listener {
            override fun onCreated(code: String) {
                knownCode = code
                val prior = history.find(localId)
                if (prior != null && prior.code != code) history.upsert(prior.copy(code = code))
                progress[localId] = (progress[localId] ?: ReportProgress(localId, ReportStage.UPLOADING)).copy(code = code)
            }

            override fun onProgress(done: Long, total: Long, file: String) {
                val now = System.currentTimeMillis()
                // Compose redraws on every write; a progress bar does not need more than ten a second.
                if (now - lastPublish < 100 && done < total) return
                lastPublish = now
                progress[localId] = ReportProgress(localId, ReportStage.UPLOADING, done, total, knownCode, currentFile = file)
            }
        }
        val outcome = try {
            ReportUploader(HttpReportApi()).run(dir, listener)
        } catch (t: Throwable) {
            ReportUploader.Outcome.WaitingRetry(t.message ?: t.javaClass.simpleName)
        }
        val prior = history.find(localId)
        when (outcome) {
            is ReportUploader.Outcome.Finished -> {
                progress[localId] = ReportProgress(localId, ReportStage.DONE, code = outcome.code.ifEmpty { knownCode }, message = if (outcome.missing.isEmpty()) "" else outcome.missing.joinToString(", "))
                prior?.let { history.upsert(it.copy(code = outcome.code.ifEmpty { knownCode }, status = HistoryStatus.SENT, note = "")) }
            }
            is ReportUploader.Outcome.WaitingRetry -> {
                progress[localId] = ReportProgress(localId, ReportStage.WAITING, code = knownCode, message = outcome.reason)
                prior?.let { history.upsert(it.copy(status = HistoryStatus.WAITING_RETRY, note = outcome.reason)) }
            }
            is ReportUploader.Outcome.Failed -> {
                progress[localId] = ReportProgress(localId, ReportStage.FAILED, code = knownCode, message = outcome.reason)
                prior?.let { history.upsert(it.copy(status = HistoryStatus.FAILED, note = outcome.reason)) }
            }
        }
    }

    internal fun dirBytes(dir: File): Long = dir.listFiles()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L
}
