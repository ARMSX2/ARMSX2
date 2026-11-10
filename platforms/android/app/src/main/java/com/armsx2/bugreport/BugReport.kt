// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.Build
import android.os.Process
import android.provider.Settings
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.armsx2.CustomDriver
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.Semaphore
import kotlin.concurrent.thread
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kr.co.iefriends.pcsx2.NativeApp

/** Where a report is in its life, for the progress screen and "My reports". */
enum class ReportStage { CAPTURING, PREPARING, UPLOADING, WAITING, WAITING_WIFI, STOPPED, DONE, FAILED }

/** What the progress screen shows for one report. */
data class ReportProgress(
    val localId: String,
    val stage: ReportStage,
    val done: Long = 0,
    val total: Long = 0,
    /** The report code, once the server has created the report. */
    val code: String = "",
    /** Why it is waiting or failed, for the screen to word. */
    val reason: UploadReason? = null,
    /** Files that were declared but did not arrive, once the report is done. */
    val missing: List<String> = emptyList(),
    val currentFile: String = "",
)

/**
 * Progress by report folder name, with a state of its own per report. A screen that shows one report
 * reads only that report's state, so the ten-a-second progress writes of one upload do not redraw
 * the lines of all the others (a plain state map notifies every reader on any write).
 */
class ProgressBoard {
    private val states = mutableStateMapOf<String, androidx.compose.runtime.MutableState<ReportProgress>>()

    operator fun get(localId: String): ReportProgress? = states[localId]?.value

    operator fun set(localId: String, value: ReportProgress) {
        val state = states[localId]
        if (state == null) states[localId] = mutableStateOf(value) else state.value = value
    }

    fun remove(localId: String) {
        states.remove(localId)
    }
}

/**
 * Evidence from a crash, waiting for the user to say whether to send it. [closedBySystem] is true
 * when Android ended the process (memory, resources) rather than it crashing, which the prompt says.
 */
data class PendingCrash(
    val dir: File,
    val title: String,
    val serial: String,
    /** Name of the GPU driver pack it ran on; empty means the system driver (the screens say which). */
    val driverLabel: String,
    val armedAt: Long,
    val marker: CrashMarker.Marker,
    val closedBySystem: Boolean = false,
)

/**
 * A game that could not start: the core reported why instead of running. Nothing crashed and no
 * marker was left, so this is how the app offers a report for it. The game and driver are kept
 * here because the library forgets the game as soon as the boot has failed.
 */
data class BootFailure(
    val message: String,
    val game: ReportGame?,
    val driverId: String,
)

/**
 * The problem-report feature's entry point and its process-wide state.
 *
 *  - [armCrashMarker] / [clearCrashMarker] bracket every game run (see [CrashMarker]).
 *  - [onDataRootReady] runs once the data folder is known, early in startup: it preserves the
 *    evidence of a crash, tidies old bundles and resumes uploads a previous run left unfinished.
 *  - [ReportSession] objects carry one report through the user flow; [prepare], [submit] and
 *    [discard] drive them.
 *  - Uploads run on background threads (one per report, two moving bytes at once) and survive the
 *    user leaving the flow. If the process dies they resume on the next launch from `state.json`,
 *    except that a big report on a metered connection waits for Wi-Fi (see [MeteredRule]).
 */
object BugReport {
    private const val PREFS = "armsx2_bugreport"

    private lateinit var app: Context

    /** For work that outlives the screen that started it. Blocking calls belong here. */
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Process-wide. A report in flight is visible here to every screen that wants to show it. */
    val progress = ProgressBoard()

    /** Set at startup when the previous run crashed; the library asks whether to send it. */
    var pendingCrash by mutableStateOf<PendingCrash?>(null)

    /** Set when a game failed to boot; the screen offers a report for it. */
    var bootFailure by mutableStateOf<BootFailure?>(null)

    val history: ReportHistory by lazy { ReportHistory(app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)) }

    // One thread per report that is being sent, and at most MAX_TRANSFERS of them moving bytes at a
    // time. A report backing off between retries gives its place up, so a new one never waits
    // behind another report's sleep (see runUpload's `sleep`).
    private val uploaders = Executors.newCachedThreadPool { r ->
        Thread(r, "bugreport-upload").apply { isDaemon = true }
    }
    private val transfers = Semaphore(MAX_TRANSFERS)
    private const val MAX_TRANSFERS = 2

    /** Guards [active], [rerun] and [stopRequested] together: a retry asked for as a run ends must not be lost. */
    private val lock = Any()
    private val active = HashSet<String>()
    private val rerun = HashSet<String>()
    private val stopRequested = HashSet<String>()
    private var unmeteredWatcher: ConnectivityManager.NetworkCallback? = null

    fun init(context: Context) {
        app = context.applicationContext
    }

    // ---- crash marker ---------------------------------------------------------------------

    /** True while the activity is started (on screen). The marker records it, see [CrashMarker]. */
    @Volatile
    private var activityStarted = false

    /** Call from the activity's `onStart`. A game that is running is back in view: re-arm its marker as foreground. */
    fun onActivityStarted() {
        activityStarted = true
        CrashMarker.setForeground(app, true)
    }

    /** Call from the activity's `onStop`. A process that dies from here on is not a crash the user saw. */
    fun onActivityStopped() {
        activityStarted = false
        CrashMarker.setForeground(app, false)
    }

    /** Records that a game is about to run. Call right before the VM thread starts. */
    fun armCrashMarker(serial: String, title: String, driverId: String, crc: String = "") {
        // First: the marker's commit also says that the log now holds a game, and the backup needs
        // that fact about the log as it was.
        com.armsx2.runtime.MainActivityRuntime.dataRootOrNull()?.let(::backUpEmulog)
        val name = if (driverId.isBlank()) "" else {
            runCatching { CustomDriver.listInstalled(app).firstOrNull { it.id == driverId }?.name }.getOrNull().orEmpty()
        }
        CrashMarker.arm(
            app,
            CrashMarker.Marker(
                System.currentTimeMillis(), Process.myPid(), serial, title, driverId, name,
                foreground = activityStarted, bootCount = bootCount(app), crc = ReportCrc.normalize(crc),
            ),
        )
    }

    /**
     * Call when the game's renderer has opened (the VM has reached running). Copies the core's
     * custom-driver status into the marker, so that if this run crashes the report can say whether
     * the driver pack opened: the new process that builds it cannot ask the core. Cheap to call
     * again on every resume; the first status of a run is the one kept.
     */
    fun noteRendererOpened() {
        scope.launch {
            val status = runCatching { NativeApp.getCustomDriverStatus() }.getOrNull().orEmpty()
            CrashMarker.setDriverStatus(app, status)
        }
    }

    /** Records that the game stopped in an orderly way. */
    fun clearCrashMarker() = CrashMarker.clear(app)

    /**
     * Makes the emulog the backup if a game was started in it and the core does not have it open
     * (see [LogBackup], which also says when that is). Called at launch, before the core starts, and
     * as a game starts, which is when a later game's boot would truncate the previous one's log.
     * Best effort: nothing here may stop the launch or the boot.
     */
    private fun backUpEmulog(dataRoot: String) {
        runCatching {
            val outcome = LogBackup.rotate(File(dataRoot, "logs"), CrashMarker.logHasGame(app), coreHoldsEmulog())
            if (outcome.clearsFlag) CrashMarker.clearLogHasGame(app)
            println("@@BUGREPORT@@ emulog backup: $outcome")
        }
    }

    /** True while the core has the emulog open. A core that cannot be asked has not opened it. */
    private fun coreHoldsEmulog(): Boolean = runCatching { NativeApp.isEmulogOpen() }.getOrDefault(false)

    /** `Settings.Global.BOOT_COUNT`: how many times the device has booted, or -1 if it will not say. */
    internal fun bootCount(context: Context): Int =
        runCatching { Settings.Global.getInt(context.contentResolver, Settings.Global.BOOT_COUNT, -1) }.getOrDefault(-1)

    /** Looks up what [CrashDecision] needs for [marker] and asks it. Returns the verdict and the exit record it used. */
    private fun assess(marker: CrashMarker.Marker): Pair<CrashDecision.Verdict, ExitRecord?> {
        val sdk = Build.VERSION.SDK_INT
        val record = if (sdk >= 30) ExitInfo.recordFor(app, marker.pid, marker.armedAt) else null
        val logs = File(app.getExternalFilesDir(null) ?: app.filesDir, "logs")
        val javaCrash = runCatching { CrashDecision.javaCrashAfter(logs, marker.armedAt) }.getOrDefault(false)
        return CrashDecision.decide(sdk, marker, record, javaCrash, bootCount(app)) to record
    }

    /** Remembers that the core could not start a game, for [bootFailure]. */
    fun noteBootFailure(message: String) {
        val current = com.armsx2.runtime.MainActivityRuntime.currentGame.value
        // The marker is still armed here (it is cleared once the boot has been dealt with) and holds the CRC.
        val crc = CrashMarker.read(app)?.takeIf { it.pid == Process.myPid() }?.crc.orEmpty()
        val game = current?.let { ReportGame(it.serial.orEmpty(), crc, it.title, it.settingsKey) }
        bootFailure = BootFailure(message, game, com.armsx2.runtime.MainActivityRuntime.customDriverId.value.orEmpty())
    }

    // ---- startup --------------------------------------------------------------------------

    private val startupRan = java.util.concurrent.atomic.AtomicBoolean(false)

    /**
     * Called once the data folder is pinned and before any game can boot. Cheap on the calling
     * thread (the emulog is renamed, nothing is copied); the rest is done on a background thread.
     *
     * The order matters, and is the reverse of what is easy to write: the evidence is kept first
     * (the emulog is renamed here, the logcat snapshot and exit records are taken on the thread),
     * only then is the marker cleared, and only then does housekeeping run. A launch that is killed
     * halfway therefore still finds the marker and does it again.
     *
     * The same rename also keeps the log of a run that was closed by hand (a hang the user force-closed
     * is not a crash anyone is asked about) as the emulog backup, see [LogBackup].
     */
    fun onDataRootReady(dataRoot: String) {
        // Once per process: a second call would find the marker the first is still working on.
        if (!startupRan.compareAndSet(false, true)) return
        val root = File(dataRoot)
        val marker = CrashMarker.read(app)
        var preserved: CrashEvidence.Preserved? = null
        // A marker from this very process is a game that is running now (the activity was recreated).
        val stale = marker?.takeIf { it.pid != Process.myPid() }
        if (stale != null) {
            val (verdict, record) = assess(stale)
            if (verdict is CrashDecision.Verdict.Prompt) {
                preserved = runCatching { CrashEvidence.preserve(app, root, stale, record, verdict.closedBySystem) }.getOrNull()
            }
        }
        // After the evidence was taken (a crash's emulog is the one renamed away above) and before the
        // core starts, which opens the emulog for writing and truncates it.
        backUpEmulog(dataRoot)
        thread(isDaemon = true, name = "bugreport-startup") {
            if (preserved != null) {
                runCatching { CrashEvidence.complete(app, root, preserved) }
                val driver = preserved.marker.driverName.ifBlank { preserved.marker.driverId }
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
                    preserved.marker.armedAt, preserved.marker, preserved.closedBySystem,
                )
            }
            if (stale != null) CrashMarker.clearIfUnchanged(app, stale)
            runCatching { Housekeeping.run(history, ReportCollector.bundleRoot(app)) }
            resumePending()
        }
    }

    /**
     * Starts the uploads an earlier run left unfinished. On a metered connection a report with
     * [ReportLimits.METERED_NOTE_BYTES] or more still to send waits for Wi-Fi instead (see
     * [MeteredRule]); [userAsked] is the "Send now" button, which does not wait.
     */
    fun resumePending(userAsked: Boolean = false) {
        val root = ReportCollector.bundleRoot(app)
        val dirs = root.listFiles { f -> f.isDirectory && File(f, ReportUploader.STATE_FILE).isFile }.orEmpty()
        val metered = isMetered(app)
        var held = false
        for (dir in dirs) {
            val state = UploadState.load(File(dir, ReportUploader.STATE_FILE)) ?: continue
            if (state.phase != ReportPhase.READY && state.phase != ReportPhase.UPLOADING && state.phase != ReportPhase.WAITING_RETRY) continue
            if (MeteredRule.allowed(metered, state.bytesLeft(), userAsked)) {
                enqueueUpload(dir)
            } else {
                held = true
                holdForWifi(dir.name, state)
            }
        }
        if (held) watchForUnmetered()
    }

    /** Marks a report as waiting for an unmetered connection. */
    private fun holdForWifi(localId: String, state: UploadState) {
        progress[localId] = ReportProgress(localId, ReportStage.WAITING_WIFI, state.bytesDone(), state.totalBytes, state.created?.code.orEmpty(), UploadReason.WIFI)
        history.find(localId)?.let { history.upsert(it.copy(status = HistoryStatus.WAITING_WIFI, note = UploadReason.WIFI.key)) }
    }

    /** True on a metered connection. Never throws: no permission or no network reads as "not metered". */
    fun isMetered(context: Context): Boolean = try {
        context.getSystemService(ConnectivityManager::class.java)?.isActiveNetworkMetered == true
    } catch (_: Exception) {
        false
    }

    /** Sends the held-back reports as soon as the default connection stops being metered. */
    private fun watchForUnmetered() {
        val cm = runCatching { app.getSystemService(ConnectivityManager::class.java) }.getOrNull() ?: return
        synchronized(lock) {
            if (unmeteredWatcher != null) return
            val callback = object : ConnectivityManager.NetworkCallback() {
                override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
                    if (!capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)) return
                    unwatchUnmetered()
                    scope.launch { resumePending() }
                }
            }
            unmeteredWatcher = callback
            runCatching { cm.registerDefaultNetworkCallback(callback) }.onFailure { unmeteredWatcher = null }
        }
    }

    private fun unwatchUnmetered() {
        val cm = runCatching { app.getSystemService(ConnectivityManager::class.java) }.getOrNull()
        val callback = synchronized(lock) { unmeteredWatcher.also { unmeteredWatcher = null } } ?: return
        runCatching { cm?.unregisterNetworkCallback(callback) }
    }

    // ---- one report -----------------------------------------------------------------------

    /**
     * Gathers the always-collected files so the consent screen can list them with their sizes.
     * Safe to call again once it has finished. Blocking: call it from a background thread.
     *
     * Never throws. If gathering goes wrong the session says so ([ReportSession.prepareFailed]) and
     * keeps whatever was gathered, so the user can still send the rest. If the user backed out
     * while it ran ([ReportSession.closed]), the folder it made is removed here, as nobody else
     * knows it exists.
     */
    fun prepare(session: ReportSession) = synchronized(session.prepareLock) { prepareLocked(session) }

    private fun prepareLocked(session: ReportSession) {
        if (session.collector != null || session.closed) return
        val origin = session.origin
        val dir = origin.crashDir ?: File(ReportCollector.bundleRoot(app), "report-${System.currentTimeMillis()}")
        var collector: ReportCollector? = null
        try {
            if (origin.crashDir != null) {
                // The background half of the evidence may still be running; a crash report needs it.
                CrashEvidence.awaitComplete(dir, 45_000)
            }
            val root = com.armsx2.runtime.MainActivityRuntime.dataRootOrNull()
            val scrubber = Scrubber(
                listOfNotNull(root, "/storage/emulated/0/Android/data/${app.packageName}"),
                AccountNames.read(root?.let(::File), listOfNotNull(runCatching { com.armsx2.RaLibrary.userName }.getOrNull())),
            )
            collector = ReportCollector(app, dir, origin, scrubber)
            if (session.keepDumpCopy) {
                collector.keepDumpCopyIn = File(com.armsx2.runtime.MainActivityRuntime.assetCopyRoot(app), "snaps")
            }
            collector.collectLight()
        } catch (t: Throwable) {
            println("@@BUGREPORT@@ gathering the report failed: ${t.javaClass.simpleName}: ${t.message}")
            collector?.noteError("gathering: ${t.message ?: t.javaClass.simpleName}")
            session.prepareFailed = true
        }
        if (session.closed) {
            if (origin.crashDir == null) dir.deleteRecursively()
            return
        }
        if (collector != null) {
            session.collector = collector
            session.lightItems = collector.collected.map { (name, bytes) -> BundleItem(name, bytes) }
            session.failedItems = collector.failed.toMap()
        }
    }

    /** What the user can do to a report that is being captured or packed, by folder name. */
    private val submissions = HashMap<String, SubmitControl>()

    /** The user chose to send without the save state and graphics dump. */
    fun skipHeavy(localId: String) {
        synchronized(submissions) { submissions[localId]?.skipHeavy = true }
    }

    /** The user cancelled while the report was being captured or packed. */
    fun cancelSubmit(localId: String) {
        synchronized(submissions) { submissions[localId]?.cancelled = true }
    }

    /**
     * Captures what was agreed, builds the bundle and starts the upload. Returns once the upload is
     * queued; progress is in [progress] under the session's folder name. Blocking: call it from a
     * background thread. A second call for the same session (Send pressed twice) returns at once.
     */
    fun submit(session: ReportSession, control: GameControl?) {
        val collector = session.collector ?: return
        if (!session.submitStarted.compareAndSet(false, true)) return
        val localId = collector.dir.name
        val game = session.origin.game
        val stop = session.stop
        synchronized(submissions) { submissions[localId] = stop }
        fun report(stage: ReportStage) {
            progress[localId] = ReportProgress(localId, stage)
        }

        try {
            var perfAfter: String? = null
            if (session.origin.canCapture && control != null && (session.includeSavestate || session.includeGsDump)) {
                report(ReportStage.CAPTURING)
                perfAfter = capture(session, collector, control)
            }
            if (stop.cancelled) throw UploadCancelled()
            report(ReportStage.PREPARING)
            val state = collector.finish(
                session.kind, session.description.trim(), perfAfter,
                skipHeavy = { stop.skipHeavy }, isCancelled = { stop.cancelled },
            )
            history.upsert(
                HistoryEntry(
                    localId = localId, code = "", kind = session.kind.wire, createdAt = state.createdAt,
                    title = game?.title.orEmpty(), serial = game?.serial.orEmpty(),
                    status = HistoryStatus.UPLOADING, bytes = state.totalBytes,
                ),
            )
            progress[localId] = ReportProgress(localId, ReportStage.UPLOADING, 0, state.totalBytes)
            // The user pressed Send knowing the size (the consent screen said if it was a lot on mobile data).
            enqueueUpload(collector.dir)
        } catch (_: UploadCancelled) {
            collector.abandonCapture()
            if (session.origin.crashDir == null) collector.dir.deleteRecursively()
            progress.remove(localId)
        } catch (t: Throwable) {
            // The raw reason goes to the log and, if the report gets that far, report.json; the screen gets a plain sentence.
            println("@@BUGREPORT@@ making report $localId failed: ${t.javaClass.simpleName}: ${t.message}")
            collector.noteError("submit: ${t.message ?: t.javaClass.simpleName}")
            collector.abandonCapture()
            progress[localId] = ReportProgress(localId, ReportStage.FAILED, reason = UploadReason.LOCAL)
            // A line in My reports, so the failure is not only on a screen that can be closed. There is
            // no state file, so there is nothing to retry; the user can delete the line. A crash keeps
            // the "not sent" line it already has, so it can be sent from there.
            if (session.origin.crashDir == null) history.upsert(
                HistoryEntry(
                    localId = localId, code = "", kind = session.kind.wire, createdAt = System.currentTimeMillis(),
                    title = game?.title.orEmpty(), serial = game?.serial.orEmpty(),
                    status = HistoryStatus.FAILED, bytes = dirBytes(collector.dir), note = UploadReason.LOCAL.key,
                ),
            )
        } finally {
            synchronized(submissions) { submissions.remove(localId) }
        }
    }

    /**
     * Takes the save state and the graphics dump on a helper thread and waits for it, so that the
     * user's "send without it" or "cancel" is heard even if a native call does not return. The
     * order, and why, is [CaptureSequence]'s: the save state first, while the game is still paused
     * where the user left it. Returns the performance numbers read after the dump.
     */
    private fun capture(session: ReportSession, collector: ReportCollector, control: GameControl): String? {
        val perf = java.util.concurrent.atomic.AtomicReference<String?>(null)
        val done = java.util.concurrent.CountDownLatch(1)
        thread(isDaemon = true, name = "bugreport-capture") {
            try {
                perf.set(collector.capture(control, session.includeSavestate, session.includeGsDump))
            } catch (t: Throwable) {
                collector.noteError("capture: ${t.message ?: t.javaClass.simpleName}")
            } finally {
                done.countDown()
            }
        }
        while (!done.await(200, java.util.concurrent.TimeUnit.MILLISECONDS)) {
            if (session.stop.skipHeavy || session.stop.cancelled) {
                collector.abandonCapture()
                break
            }
        }
        return perf.get()
    }

    /** Drops a report the user backed out of before sending. Crash evidence is kept for later. */
    fun discard(session: ReportSession) {
        // If the files are still being gathered, prepare() sees this and removes its own folder.
        session.closed = true
        val collector = session.collector ?: return
        session.collector = null
        if (session.origin.crashDir == null) collector.dir.deleteRecursively()
    }

    /** True if the report's upload state is still on the device, which is what a retry needs. */
    fun canRetry(localId: String): Boolean =
        File(File(ReportCollector.bundleRoot(app), localId), ReportUploader.STATE_FILE).isFile

    /**
     * Puts an upload on the queue. A report that is already queued or running is not started twice:
     * if it is on its way out when this is asked, it runs once more after it ends, so a Retry
     * pressed just as an attempt gives up is not lost.
     */
    fun enqueueUpload(dir: File) {
        val id = dir.name
        synchronized(lock) {
            stopRequested.remove(id)
            if (!active.add(id)) {
                rerun.add(id)
                return
            }
        }
        startWorker(dir)
    }

    private fun startWorker(dir: File) {
        val id = dir.name
        uploaders.execute {
            try {
                do {
                    transfers.acquireUninterruptibly()
                    try {
                        runUpload(dir)
                    } finally {
                        transfers.release()
                    }
                    val again = synchronized(lock) {
                        val wanted = rerun.remove(id)
                        if (!wanted) active.remove(id)
                        wanted
                    }
                } while (again)
            } finally {
                // Only reached with the id still active if runUpload threw something it did not expect.
                synchronized(lock) {
                    if (active.remove(id)) rerun.remove(id)
                }
            }
        }
    }

    /**
     * Tries a report again from "My reports" or the progress screen, or sends one that was held for
     * Wi-Fi ("Send now"). What it does depends on the report ([UploadState.prepareRetry]): files
     * that were missing get another chance, and a report whose upload link ended is turned into a
     * new report for what is left, keeping the old code in the history line.
     */
    fun retry(localId: String): RetryResult {
        val dir = File(ReportCollector.bundleRoot(app), localId)
        val stateFile = File(dir, ReportUploader.STATE_FILE)
        if (!stateFile.isFile) return RetryResult.GONE
        val state = UploadState.load(stateFile) ?: return RetryResult.GONE
        if (!state.prepareRetry()) {
            // Nothing is left to send. A report that only lacked its final confirmation is as sent as it will get.
            if (state.phase != ReportPhase.FINISHED) {
                state.phase = ReportPhase.FINISHED
                state.finishedAt = System.currentTimeMillis()
                state.save(stateFile)
                history.find(localId)?.let { history.upsert(it.copy(status = HistoryStatus.SENT, note = "")) }
                progress[localId] = ReportProgress(localId, ReportStage.DONE, code = state.created?.code ?: state.earlierCodes.lastOrNull().orEmpty())
            }
            return RetryResult.NOTHING_LEFT
        }
        state.save(stateFile)
        val code = state.created?.code.orEmpty()
        history.find(localId)?.let {
            history.upsert(it.copy(status = HistoryStatus.UPLOADING, code = code, note = "", earlierCodes = state.earlierCodes.joinToString(",")))
        }
        progress[localId] = ReportProgress(localId, ReportStage.UPLOADING, state.bytesDone(), state.totalBytes, code)
        enqueueUpload(dir)
        return RetryResult.STARTED
    }

    /** What [retry] did. */
    enum class RetryResult { STARTED, NOTHING_LEFT, GONE }

    /**
     * Stops sending a report: what the server already has is kept, `finish` is called if it will
     * take it, and the line says Stopped. A report that is not running right now is stopped the
     * same way, so it is not picked up again at the next launch.
     */
    fun stop(localId: String) {
        val dir = File(ReportCollector.bundleRoot(app), localId)
        val idle = synchronized(lock) {
            stopRequested.add(localId)
            localId !in active
        }
        if (idle) {
            if (!File(dir, ReportUploader.STATE_FILE).isFile) {
                synchronized(lock) { stopRequested.remove(localId) }
                return
            }
            synchronized(lock) { active.add(localId) }
            startWorker(dir)
        }
    }

    /** The folder of a report still on the device, or null once it has been cleaned up. */
    fun localDir(localId: String): File? =
        File(ReportCollector.bundleRoot(app), localId).takeIf { it.isDirectory }

    /** The crash behind an unsent "My reports" line, rebuilt from its folder, or null if it is gone. */
    fun crashFor(localId: String): PendingCrash? {
        val dir = localDir(localId) ?: return null
        val (marker, closedBySystem) = CrashEvidence.readMarker(dir) ?: return null
        val driver = marker.driverName.ifBlank { marker.driverId }
        return PendingCrash(dir, marker.title, marker.serial, driver, marker.armedAt, marker, closedBySystem)
    }

    /** The newest crash whose evidence is on the device and not yet sent, or null. Reads the disk. */
    fun latestUnsentCrash(): PendingCrash? =
        CrashEvidence.unsent(ReportCollector.bundleRoot(app)).firstNotNullOfOrNull { crashFor(it.name) }

    /** Removes a report's files and its "My reports" entry (crash evidence the user does not want). */
    fun delete(localId: String) {
        // An upload still running for it ends at its next step instead of writing into a folder that is going.
        synchronized(lock) { if (localId in active) stopRequested.add(localId) }
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
        // While a report sleeps between retries it gives its place to another one.
        val uploader = ReportUploader(
            HttpReportApi(),
            sleep = { ms ->
                transfers.release()
                try {
                    Thread.sleep(ms)
                } finally {
                    transfers.acquireUninterruptibly()
                }
            },
        )
        val outcome = try {
            uploader.run(dir, listener, isCancelled = { synchronized(lock) { localId in stopRequested } })
        } catch (t: Throwable) {
            println("@@BUGREPORT@@ upload of $localId stopped on ${t.javaClass.simpleName}: ${t.message}")
            ReportUploader.Outcome.WaitingRetry(UploadReason.LOCAL, t.message ?: t.javaClass.simpleName)
        }
        synchronized(lock) { stopRequested.remove(localId) }
        val state = UploadState.load(File(dir, ReportUploader.STATE_FILE))
        val earlier = state?.earlierCodes?.joinToString(",").orEmpty()
        val prior = history.find(localId)
        when (outcome) {
            is ReportUploader.Outcome.Finished -> {
                val code = outcome.code.ifEmpty { knownCode }
                progress[localId] = ReportProgress(localId, ReportStage.DONE, code = code, missing = outcome.missing)
                prior?.let { history.upsert(it.copy(code = code, status = HistoryStatus.SENT, note = "", earlierCodes = earlier)) }
            }
            is ReportUploader.Outcome.WaitingRetry -> {
                progress[localId] = ReportProgress(localId, ReportStage.WAITING, code = knownCode, reason = outcome.reason)
                prior?.let { history.upsert(it.copy(status = HistoryStatus.WAITING_RETRY, note = outcome.reason.key, earlierCodes = earlier)) }
            }
            is ReportUploader.Outcome.Failed -> {
                progress[localId] = ReportProgress(localId, ReportStage.FAILED, code = knownCode, reason = outcome.reason)
                prior?.let { history.upsert(it.copy(status = HistoryStatus.FAILED, note = outcome.reason.key, earlierCodes = earlier)) }
            }
            is ReportUploader.Outcome.Stopped -> {
                val code = outcome.code.ifEmpty { knownCode }
                progress[localId] = ReportProgress(localId, ReportStage.STOPPED, code = code, missing = outcome.missing)
                prior?.let { history.upsert(it.copy(code = code, status = HistoryStatus.STOPPED, note = "", earlierCodes = earlier)) }
            }
        }
    }

    internal fun dirBytes(dir: File): Long = dir.listFiles()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L
}
