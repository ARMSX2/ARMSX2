// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import android.os.Build
import com.armsx2.BuildConfig
import com.armsx2.CustomDriver
import com.armsx2.DeviceTier
import com.armsx2.GpuInfo
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.zip.GZIPOutputStream

/**
 * Takes a save state, and when the memory card was busy lets the game run for a moment and tries
 * once more. The card's write counter only goes down at vertical sync, so a paused game never
 * clears it; a state taken while it is nonzero would not match the card and is refused.
 */
object SavestateAttempt {
    /** How long the game may run to let the card finish. */
    const val MEMCARD_WAIT_MS = 2_000L

    enum class Result { SAVED, MEMCARD_BUSY, FAILED }

    /** One try with the game as it is: [Result.MEMCARD_BUSY] if it was refused because the card is busy, [Result.FAILED] for any other refusal. */
    fun once(save: () -> Boolean, memcardBusy: () -> Boolean): Result {
        if (save()) return Result.SAVED
        return if (memcardBusy()) Result.MEMCARD_BUSY else Result.FAILED
    }

    fun run(
        save: () -> Boolean,
        memcardBusy: () -> Boolean,
        control: GameControl,
        abandoned: () -> Boolean = { false },
        waitMs: Long = MEMCARD_WAIT_MS,
        now: () -> Long = { System.currentTimeMillis() },
        sleep: (Long) -> Unit = { Thread.sleep(it) },
    ): Result {
        val first = once(save, memcardBusy)
        if (first != Result.MEMCARD_BUSY) return first
        val wasPaused = control.isPaused()
        try {
            if (wasPaused) {
                control.resume()
                control.awaitRunning(waitMs)
            }
            val deadline = now() + waitMs
            while (now() < deadline && !abandoned() && memcardBusy()) sleep(50)
        } finally {
            if (wasPaused) control.pause()
        }
        if (abandoned()) return Result.FAILED
        if (save()) return Result.SAVED
        return if (memcardBusy()) Result.MEMCARD_BUSY else Result.FAILED
    }
}

/** How the collector asks the running game to move, without knowing how the app pauses it. */
interface GameControl {
    fun isPaused(): Boolean
    fun resume()
    fun pause()

    /** Waits up to [timeoutMs] for the game to be running after [resume]. */
    fun awaitRunning(timeoutMs: Long): Boolean

    /**
     * Waits up to [timeoutMs] for the game to be paused after [pause]. The app only reports the
     * game paused once the graphics thread has caught up, so with a graphics dump still being
     * closed this can take as long as the dump does.
     */
    fun awaitPaused(timeoutMs: Long): Boolean
}

/**
 * Why a file is not in a report, in a form the screens can word. [detail] is the raw reason (an
 * exception's text, the platform's message) and goes into `report.json` only; the consent screen
 * says what [kind] means in the app's own words.
 */
enum class CollectFailure(val key: String) {
    /** There was nothing to collect: no log yet, or it was not saved when the app restarted. */
    UNAVAILABLE("bugreport.fail.unavailable"),

    /** The memory card was being written to, so a save state was refused. */
    MEMCARD("bugreport.fail.memcard"),

    /** The graphics dump could not be started. */
    NOT_STARTED("bugreport.fail.notstarted"),

    /** The game drew no frame in time for the graphics dump. */
    NO_FRAME("bugreport.fail.noframe"),

    /** Over the server's limit for one file. */
    TOO_LARGE("bugreport.fail.toolarge"),

    /** Dropped to keep the whole report under the size limit. */
    LEFT_OUT("bugreport.fail.leftout"),

    /** The user chose to send without it. */
    SKIPPED("bugreport.fail.skipped"),

    /** Anything else; [Failure.detail] has the platform's words. */
    ERROR("bugreport.fail.error"),
}

class Failure(val kind: CollectFailure, val detail: String = "")

/**
 * Builds one report's folder: the always-collected files first, the heavy ones (save state, GS
 * dump) when the user agreed, then `report.json` and `state.json`. Everything goes through
 * [Scrubber] on its way into the folder, so what is on disk is what will be sent, with one
 * exception: `tombstone.pb`, Android's native crash report, is a protobuf copied byte for byte
 * (see [CrashEvidence]). It is kept raw because it is the best evidence a native crash leaves, and
 * it can hold file paths and memory contents that a text scrubber cannot reach.
 *
 * Blocking; run on a background thread. A step that fails is recorded in [failed] with its reason
 * and the report carries on without it, because a report with only the logs is still worth having.
 * A file that is already in the folder is kept as it is, which is how a crash report reuses the
 * evidence that was preserved when the app started.
 */
class ReportCollector(
    private val context: Context,
    val dir: File,
    private val origin: ReportOrigin,
    private val scrubber: Scrubber,
) {
    /** File name to size, for everything in the folder so far. */
    val collected = linkedMapOf<String, Long>()

    /** File name to why it is not there. */
    val failed = linkedMapOf<String, Failure>()

    /** Problems in the report's own making that no file name goes with, raw, for `report.json`. */
    val errors = mutableListOf<String>()

    /** Where a copy of the graphics dump is also saved for the user, or null. */
    var keepDumpCopyIn: File? = null

    /** Set when the user chose to send without the save state and graphics dump, or to cancel: captures that finish later are dropped. */
    @Volatile
    private var captureAbandoned = false
    private val captureLock = Any()

    // What the capture did to the game's clock, for `report.json` (see captureJson). Written by the
    // capture thread, read when the report is built.
    private val runClock = RunClock()

    @Volatile
    private var captureRan = false

    @Volatile
    private var savestateTiming: SavestateTiming? = null

    @Volatile
    private var dumpStartFrames: Int? = null

    @Volatile
    private var dumpRecordedFrames: Int? = null

    /** The GS driver report as it will be sent, once either source of it has produced one. */
    private var systemReport: String? = null

    /** Records a problem with building the report for `report.json`; it is never shown as it is. */
    fun noteError(text: String) {
        errors += text
    }

    /** Gathers the files every report carries. */
    fun collectLight() {
        dir.mkdirs()
        readEvidenceNotes()
        // A crash report's logs were set aside when the app started. If one could not be, the
        // current emulog and logcat belong to a different run and must not stand in for it.
        if (origin.crashDir == null) {
            step(ReportFiles.EMULOG) { writeEmulog() }
            // With no backup there is nothing to say, so a missing one is not listed as failed.
            if (previousEmulog()?.let { it.isFile && it.length() > 0 } == true) step(ReportFiles.EMULOG_PREVIOUS) { writePreviousEmulog() }
            step(ReportFiles.LOGCAT) {
                val result = LogcatSnapshot.write(scrubber, File(dir, ReportFiles.LOGCAT))
                if (!result.ok) throw IOException(result.error ?: "logcat failed")
            }
        } else {
            for (name in listOf(ReportFiles.EMULOG, ReportFiles.LOGCAT)) {
                val f = File(dir, name)
                if (f.isFile && f.length() > 0) collected[name] = f.length()
                else failed.putIfAbsent(name, Failure(CollectFailure.UNAVAILABLE, "not saved when the app restarted"))
            }
            // The backup was copied in with the rest of the evidence, as it stood at the crash. A
            // crash folder from before the backup existed has none; that is not a failure either.
            File(dir, ReportFiles.EMULOG_PREVIOUS).takeIf { it.isFile && it.length() > 0 }?.let { collected[ReportFiles.EMULOG_PREVIOUS] = it.length() }
        }
        step(ReportFiles.SETTINGS) { writeSettings() }
        step(ReportFiles.JAVA_CRASHES, optional = true) { writeJavaCrashes() }
        step(ReportFiles.SYSTEM) { writeSystem() }
        // The crash evidence files only exist for a crash; note which are there.
        for (name in listOf(ReportFiles.EXIT_INFO, ReportFiles.TOMBSTONE, ReportFiles.ANR)) {
            val f = File(dir, name)
            if (f.isFile && f.length() > 0) collected[name] = f.length()
        }
    }

    /**
     * Takes the save state and the graphics dump in the order [CaptureSequence] describes, and
     * returns the performance numbers read after the dump (null when no dump was asked for). What
     * the capture did to the game's clock is kept for `report.json`: see [captureJson].
     */
    fun capture(control: GameControl, savestate: Boolean, gsDump: Boolean): String? {
        captureRan = true
        val timed = TimedControl(control, runClock)
        var perf: String? = null
        val outcome = CaptureSequence.run(
            savestate, gsDump, control.isPaused(),
            object : CaptureSequence.Steps {
                override fun saveNow() = saveStateNow()

                override fun saveAfterResume() = saveStateAfterResume(timed)

                override fun dump() {
                    captureGsDump(timed)
                    perf = runCatching { NativeApp.getPerfSnapshot() }.getOrNull()
                }
            },
        )
        synchronized(captureLock) {
            if (!captureAbandoned) savestateTiming = outcome.timing
        }
        return perf
    }

    /**
     * One try at the save state with the game as it is. A refusal for a busy memory card is not
     * recorded: [saveStateAfterResume] gets a second chance and decides.
     */
    private fun saveStateNow(): SavestateAttempt.Result {
        if (captureAbandoned) return SavestateAttempt.Result.FAILED
        val target = File(dir, ReportFiles.SAVESTATE)
        target.delete()
        val result = SavestateAttempt.once(save = { writeSavestate(target) }, memcardBusy = ::memcardBusy)
        if (result != SavestateAttempt.Result.MEMCARD_BUSY) recordSavestate(result, target)
        return result
    }

    /** The retry after a busy memory card: the game may run for a moment to clear it. See [SavestateAttempt]. */
    private fun saveStateAfterResume(control: GameControl): SavestateAttempt.Result {
        if (captureAbandoned) return SavestateAttempt.Result.FAILED
        val target = File(dir, ReportFiles.SAVESTATE)
        target.delete()
        val result = SavestateAttempt.run(
            save = { writeSavestate(target) },
            memcardBusy = ::memcardBusy,
            control = control,
            abandoned = { captureAbandoned },
        )
        recordSavestate(result, target)
        return result
    }

    private fun memcardBusy(): Boolean = runCatching { NativeApp.isMemcardBusy() }.getOrDefault(false)

    private fun writeSavestate(target: File): Boolean {
        val ok = runCatching { NativeApp.saveStateToPath(target.absolutePath) }.getOrDefault(false)
        if (!ok || !target.isFile || target.length() == 0L) {
            target.delete()
            return false
        }
        return true
    }

    /** Notes the final answer for the save state, unless the capture was given up on meanwhile. */
    private fun recordSavestate(result: SavestateAttempt.Result, target: File) {
        synchronized(captureLock) {
            if (captureAbandoned) {
                target.delete()
                return
            }
            if (result != SavestateAttempt.Result.SAVED) {
                failed[ReportFiles.SAVESTATE] = Failure(
                    if (result == SavestateAttempt.Result.MEMCARD_BUSY) CollectFailure.MEMCARD else CollectFailure.ERROR,
                    "saveStateToPath refused: $result",
                )
                return
            }
            collected[ReportFiles.SAVESTATE] = target.length()
        }
    }

    /**
     * Takes a GS dump of the next frame. The dump is opened when the GS draws, so a paused game is
     * resumed, and paused again as soon as the core says the dump's frames are recorded; the zip
     * is then waited for with the game stopped (see [GsDumpRun]).
     */
    private fun captureGsDump(control: GameControl): Boolean {
        if (captureAbandoned) return false
        val work = File(dir, ".gsdump").apply { deleteRecursively(); mkdirs() }
        try {
            val base = File(work, "dump")
            val zip = File(work, "dump.gs.zip")
            val wasPaused = control.isPaused()
            val queued = runCatching { NativeApp.captureGsDumpTo(base.absolutePath, 1) }.getOrDefault(false)
            if (!queued) {
                failed[ReportFiles.GS_DUMP] = Failure(CollectFailure.NOT_STARTED, "captureGsDumpTo returned false")
                return false
            }
            val last = GsDumpRun.run(
                resume = wasPaused,
                control = control,
                status = { GsDumpStatus.fromNative(runCatching { NativeApp.getGsDumpStatus() }.getOrNull()) },
                zipReady = { zip.isFile },
                abandoned = { captureAbandoned },
                budgetMs = GS_DUMP_WAIT_MS,
            )
            // The app calls the game paused once the graphics thread is idle again, which is after the
            // zip. Waiting for it here means the save state retry, if there is one, sees a stopped game.
            if (wasPaused && !captureAbandoned) control.awaitPaused(3_000)
            synchronized(captureLock) {
                if (!captureAbandoned && last != null) {
                    dumpStartFrames = last.vsyncsToOpen.takeIf { it > 0 }
                    dumpRecordedFrames = last.vsyncsToRecorded.takeIf { it > 0 }
                }
            }
            if (!zip.isFile) {
                synchronized(captureLock) {
                    if (!captureAbandoned) {
                        failed[ReportFiles.GS_DUMP] = Failure(
                            CollectFailure.NO_FRAME,
                            "no dump bundle within ${GS_DUMP_WAIT_MS} ms; the core's dump phase was ${last?.phase ?: "not reported"}",
                        )
                    }
                }
                return false
            }
            val target = File(dir, ReportFiles.GS_DUMP)
            synchronized(captureLock) {
                if (captureAbandoned) return false
                val report = scrubber.rewriteGsBundle(zip, target)
                // The dump's own report was written when the renderer was running: better than the
                // standalone one, which has no renderer sections.
                if (report != null) {
                    File(dir, ReportFiles.SYSTEM).writeText(report)
                    systemReport = report
                    collected[ReportFiles.SYSTEM] = File(dir, ReportFiles.SYSTEM).length()
                }
                collected[ReportFiles.GS_DUMP] = target.length()
            }
            copyDumpForUser(target)
            return true
        } catch (e: Exception) {
            synchronized(captureLock) {
                if (!captureAbandoned) failed[ReportFiles.GS_DUMP] = Failure(CollectFailure.ERROR, e.message ?: e.javaClass.simpleName)
            }
            File(dir, ReportFiles.GS_DUMP).delete()
            return false
        } finally {
            work.deleteRecursively()
        }
    }

    /** Saves a copy of the (scrubbed) dump in the snaps folder, named for the game and the time. A failure is only noted. */
    private fun copyDumpForUser(dump: File) {
        val snaps = keepDumpCopyIn ?: return
        try {
            snaps.mkdirs()
            val serial = origin.game?.serial?.takeIf { it.isNotBlank() } ?: "game"
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date())
            dump.copyTo(File(snaps, "${serial}_$stamp.gs.zip"), overwrite = true)
        } catch (t: Throwable) {
            errors += "Could not keep a copy of the graphics dump in snaps: ${t.message ?: t.javaClass.simpleName}"
        }
    }

    /**
     * Gives up on the save state and the graphics dump, whether or not they have finished: the user
     * chose to send without them, or to cancel. A capture that finishes after this is discarded.
     */
    fun abandonCapture() {
        synchronized(captureLock) {
            captureAbandoned = true
            for (name in listOf(ReportFiles.GS_DUMP, ReportFiles.SAVESTATE)) {
                if (name !in collected) failed[name] = Failure(CollectFailure.SKIPPED, "skipped by the user")
            }
        }
    }

    /**
     * Writes `report.json` and `state.json`, hashes every file and returns the state the uploader
     * works from. Files over the server's limits are dropped (the biggest first) and noted.
     */
    fun finish(
        kind: ReportKind,
        description: String,
        perfAfterResume: String?,
        /** True once the user has chosen to send without the save state and graphics dump. */
        skipHeavy: () -> Boolean = { false },
        /** True once the user has cancelled; ends the work with [UploadCancelled]. */
        isCancelled: () -> Boolean = { false },
    ): UploadState {
        // The logs go in last, so a failure during capture is in them.
        refreshLogs()
        enforceLimits()
        val commit = AppCommit.pick(BuildConfig.GIT_COMMIT, systemReport?.let(::appCommitFrom))
        val heavy = setOf(ReportFiles.GS_DUMP, ReportFiles.SAVESTATE)
        // Heavy files are dropped before report.json is written, so it says they were left out.
        if (skipHeavy()) dropHeavy(heavy)
        val report = buildReport(kind, description, commit, perfAfterResume)
        File(dir, ReportFiles.REPORT).writeText(report.toString(2))
        collected[ReportFiles.REPORT] = File(dir, ReportFiles.REPORT).length()

        val files = ReportFiles.ALL.mapNotNull { name ->
            val f = File(dir, name)
            if (!f.isFile || f.length() == 0L) return@mapNotNull null
            if (isCancelled()) throw UploadCancelled()
            if (name in heavy && skipHeavy()) {
                // Chosen after report.json was written: the file is simply not sent.
                f.delete()
                failed[name] = Failure(CollectFailure.SKIPPED, "skipped by the user")
                return@mapNotNull null
            }
            val stop = { isCancelled() || (name in heavy && skipHeavy()) }
            try {
                FileState(name, f.length(), Scrubber.sha256(f, stop))
            } catch (_: UploadCancelled) {
                if (isCancelled()) throw UploadCancelled()
                f.delete()
                failed[name] = Failure(CollectFailure.SKIPPED, "skipped by the user")
                null
            }
        }.toMutableList()

        val summary = ReportJson.summary(
            description = description,
            game = origin.game,
            device = "${Build.MANUFACTURER} ${Build.MODEL}".trim(),
            soc = DeviceTier.socModel(),
            gpu = gpuName(),
            driver = driverLabel(),
            os = "Android ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})",
        )
        val request = ReportJson.createRequest(kind, BuildConfig.VERSION_NAME, commit, BuildConfig.FLAVOR, summary)
        val state = UploadState(
            localId = dir.name,
            kind = kind.wire,
            createdAt = System.currentTimeMillis(),
            request = request,
            files = files,
            phase = ReportPhase.READY,
        )
        state.save(File(dir, ReportUploader.STATE_FILE))
        return state
    }

    // ---- the always-collected files -------------------------------------------------------

    /** Removes the files named in [names] and notes that the user chose to leave them out. */
    private fun dropHeavy(names: Set<String>) {
        for (name in names) {
            val f = File(dir, name)
            if (f.isFile) {
                f.delete()
                collected.remove(name)
                failed[name] = Failure(CollectFailure.SKIPPED, "skipped by the user")
            }
        }
    }

    /**
     * Takes the emulog and logcat again now that capture is over, so that anything that went wrong
     * while capturing is in them. The first copies stay if a new one cannot be made. A crash
     * report's logs are the previous run's and are not touched. The emulog backup is not taken
     * again: nothing the capture does can change it.
     */
    private fun refreshLogs() {
        if (origin.crashDir != null) return
        refresh(ReportFiles.EMULOG) { writeEmulogTo(it) }
        refresh(ReportFiles.LOGCAT) {
            val result = LogcatSnapshot.write(scrubber, it)
            if (!result.ok) throw IOException(result.error ?: "logcat failed")
        }
    }

    private fun refresh(name: String, write: (File) -> Unit) {
        val target = File(dir, name)
        val fresh = File(dir, "$name.new")
        try {
            write(fresh)
            if (fresh.isFile && fresh.length() > 0 && fresh.renameTo(target)) {
                collected[name] = target.length()
                failed.remove(name)
            }
        } catch (t: Throwable) {
            errors += "Could not refresh $name after capture: ${t.message ?: t.javaClass.simpleName}"
        } finally {
            fresh.delete()
        }
    }

    private fun writeEmulog() = writeEmulogTo(File(dir, ReportFiles.EMULOG))

    /** The emulog backup, or null if there is no data folder. It does not change while a report is made: only a launch or a game start moves it. */
    private fun previousEmulog(): File? =
        MainActivityRuntime.dataRootOrNull()?.let { LogBackup.previous(File(it, "logs")) }

    private fun writePreviousEmulog() {
        val src = previousEmulog() ?: throw IOException("the data folder is not available")
        LogBackup.writeScrubbedGz(scrubber, src, File(dir, ReportFiles.EMULOG_PREVIOUS))
    }

    private fun writeEmulogTo(dest: File) {
        val root = MainActivityRuntime.dataRootOrNull() ?: throw IOException("the data folder is not available")
        val src = File(root, "logs/emulog.txt")
        if (!src.isFile) throw IOException("there is no emulog.txt yet (no game has been started)")
        val length = src.length()
        src.inputStream().use { input ->
            scrubber.copyText(input, length, GZIPOutputStream(dest.outputStream().buffered()))
        }
    }

    private fun writeSettings() {
        val doc = JSONObject()
        doc.put("global", scrubber.settings(ConfigStore.loadGlobal().toJson()))
        val key = origin.game?.settingsKey
        if (key != null) {
            ConfigStore.loadOverrides(key)?.let { doc.put("game_overrides", scrubber.settings(it)) }
            doc.put("effective_for_game", scrubber.settings(ConfigStore.resolveForGame(key).toJson()))
        }
        doc.put("settings_key", key ?: JSONObject.NULL)
        GZIPOutputStream(File(dir, ReportFiles.SETTINGS).outputStream().buffered()).use {
            it.write(doc.toString(2).toByteArray(Charsets.UTF_8))
        }
    }

    private fun writeJavaCrashes() {
        val logs = File(context.getExternalFilesDir(null) ?: context.filesDir, "logs")
        val cutoff = System.currentTimeMillis() - 7L * 24 * 3600 * 1000
        val crashes = logs.listFiles { f -> f.isFile && f.name.startsWith("crash-") && f.name.endsWith(".txt") && f.lastModified() >= cutoff }
            ?.sortedBy { it.lastModified() }
            .orEmpty()
        if (crashes.isEmpty()) return
        GZIPOutputStream(File(dir, ReportFiles.JAVA_CRASHES).outputStream().buffered()).bufferedWriter().use { out ->
            for (f in crashes.takeLast(10)) {
                out.write("==== ${f.name} ====\n")
                out.write(scrubber.text(f.readText().take(256 * 1024)))
                out.write("\n")
            }
        }
    }

    private fun writeSystem() {
        val tmp = File(dir, "system.raw.json")
        try {
            val ok = runCatching { NativeApp.writeSystemReport(tmp.absolutePath) }.getOrDefault(false)
            val text = if (ok && tmp.isFile) scrubber.driverReport(tmp.readText()) else fallbackSystem().toString(2)
            File(dir, ReportFiles.SYSTEM).writeText(text)
            systemReport = text
        } finally {
            tmp.delete()
        }
    }

    /** What Kotlin can say about the device when the native report cannot be written. */
    private fun fallbackSystem(): JSONObject = JSONObject()
        .put("schema", 1)
        .put("fallback", "the native driver report could not be written; this is what the app knows")
        .put("device", deviceJson())

    private fun deviceJson(): JSONObject = JSONObject()
        .put("manufacturer", Build.MANUFACTURER).put("model", Build.MODEL).put("device", Build.DEVICE)
        .put("hardware", Build.HARDWARE).put("soc", DeviceTier.socModel()).put("fingerprint", Build.FINGERPRINT)
        .put("sdk", Build.VERSION.SDK_INT).put("release", Build.VERSION.RELEASE)
        .put("abis", JSONArray(Build.SUPPORTED_ABIS.toList()))

    // ---- report.json ----------------------------------------------------------------------

    private fun buildReport(kind: ReportKind, description: String, commit: String, perfAfterResume: String?): JSONObject {
        val report = JSONObject()
        report.put("schema", 1)
        report.put("kind", kind.wire)
        report.put("description", description)
        report.put("created_at_ms", System.currentTimeMillis())
        report.put(
            "app",
            JSONObject()
                .put("version", BuildConfig.VERSION_NAME).put("version_code", BuildConfig.VERSION_CODE)
                .put("commit", commit).put("flavor", BuildConfig.FLAVOR).put("channel", BuildConfig.CHANNEL)
                .put("application_id", BuildConfig.APPLICATION_ID).put("build_type", BuildConfig.BUILD_TYPE),
        )
        report.put("device", deviceJson().put("gpu", gpuName()))
        origin.game?.let {
            report.put("game", JSONObject().put("serial", it.serial).put("crc", it.crc).put("title", it.title))
        }
        report.put("driver", driverJson())
        origin.perfAtOpen?.let { report.put("perf", parse(it)) }
        perfAfterResume?.let { report.put("perf_after_resume", parse(it)) }
        captureJson()?.let { report.put("capture", it) }
        origin.crashMarker?.let {
            report.put(
                "crash_marker",
                JSONObject().put("armed_at_ms", it.armedAt).put("pid", it.pid)
                    .put("serial", it.serial).put("title", it.title)
                    .put("driver_id", it.driverId).put("driver_name", it.driverName)
                    .put("foreground", it.foreground).put("boot_count", it.bootCount),
            )
        }
        report.put("exit_records", exitRecordsJson())
        report.put(
            "collected",
            JSONArray().also { a -> for ((name, size) in collected) a.put(JSONObject().put("name", name).put("size", size)) },
        )
        report.put(
            "failed",
            JSONArray().also { a ->
                for ((name, why) in failed) {
                    a.put(JSONObject().put("name", name).put("kind", why.kind.name).put("reason", why.detail.ifBlank { why.kind.name }))
                }
            },
        )
        report.put("errors", JSONArray(errors))
        return report
    }

    /**
     * How far the capture drifted from the moment the user paused the game, so whoever reads the
     * report knows how much to trust the save state and the dump as "what the user saw":
     *
     *  - `savestate_timing`: `paused_moment`, `after_resume` (a busy memory card made the game run
     *    first) or `running` (the game was not paused when the capture began). Only present when a
     *    save state is in the report.
     *  - `game_ran_ms`: wall-clock milliseconds the capture had the game running, from each resume
     *    request to the next pause request.
     *  - `dump_start_frames`, `dump_recorded_frames`: GS vertical syncs from the dump request, made
     *    while paused, to the one the dump opened on and to its last frame. Absent when the core
     *    could not say.
     *
     * Null when the report had no capture (a crash report, or nothing running).
     */
    private fun captureJson(): JSONObject? {
        if (!captureRan) return null
        val json = JSONObject()
        if (ReportFiles.SAVESTATE in collected) savestateTiming?.let { json.put("savestate_timing", it.wire) }
        json.put("game_ran_ms", runClock.totalMs())
        dumpStartFrames?.let { json.put("dump_start_frames", it) }
        dumpRecordedFrames?.let { json.put("dump_recorded_frames", it) }
        return json
    }

    /** The driver pack the game was set to, what its `meta.json` says, and what the loader reports. */
    private fun driverJson(): JSONObject {
        val json = JSONObject()
        json.put("selected_pack_id", origin.driverId)
        json.put("selected_pack_name", origin.driverName)
        val installed = CustomDriver.listInstalled(context).firstOrNull { it.id == origin.driverId }
        if (installed != null) {
            runCatching { parse(File(installed.driverDir, "meta.json").readText()) }.getOrNull()?.let {
                json.put("pack_meta", scrubber.json(it))
            }
        }
        // A crash report is collected in a new process, where the core's answer would describe a run
        // that never happened. See DriverStatusEntry.
        val entry = DriverStatusEntry.choose(origin.crashMarker) { runCatching { NativeApp.getCustomDriverStatus() }.getOrNull() }
        when (entry) {
            is DriverStatusEntry.Status -> json.put("native_status", scrubber.json(parse(entry.json)))
            is DriverStatusEntry.Note -> json.put("native_status_note", entry.text)
            null -> {}
        }
        return json
    }

    private fun exitRecordsJson(): JSONArray {
        val array = JSONArray()
        for (r in ExitInfo.read(context, 5)) {
            array.put(
                JSONObject().put("pid", r.pid).put("reason", r.reasonName).put("status", r.status)
                    .put("timestamp_ms", r.timestamp).put("description", scrubber.text(r.description)),
            )
        }
        return array
    }

    private fun parse(text: String): Any = try {
        if (text.trimStart().startsWith("[")) JSONArray(text) else JSONObject(text)
    } catch (_: Exception) {
        text
    }

    // ---- small helpers --------------------------------------------------------------------

    /** The label for the driver in the report's summary: the pack's name, or "system". */
    fun driverLabel(): String {
        if (origin.driverId.isBlank()) return "system"
        val installed = CustomDriver.listInstalled(context).firstOrNull { it.id == origin.driverId }
        val name = installed?.name?.takeIf { it.isNotBlank() } ?: origin.driverName.ifBlank { origin.driverId }
        val version = installed?.version.orEmpty()
        return if (version.isBlank() || name.contains(version)) name else "$name $version"
    }

    private fun gpuName(): String = runCatching { GpuInfo.rendererName() }.getOrNull().orEmpty()

    /** The commit the core reports for itself in the driver report, if it knows one. */
    private fun appCommitFrom(systemJson: String): String? = runCatching {
        JSONObject(systemJson).optJSONObject("app")?.optString("commit").orEmpty().ifBlank { null }
    }.getOrNull()

    /** Drops files the server would refuse, the largest first, and says so in [failed]. */
    private fun enforceLimits() {
        for (name in ReportFiles.ALL) {
            val f = File(dir, name)
            if (f.isFile && f.length() > ReportLimits.MAX_FILE_BYTES) {
                failed[name] = Failure(CollectFailure.TOO_LARGE, "${f.length() / (1024 * 1024)} MB")
                f.delete()
                collected.remove(name)
            }
        }
        while (true) {
            val present = ReportFiles.ALL.map { File(dir, it) }.filter { it.isFile }
            val total = present.sumOf { it.length() }
            if (present.size <= ReportLimits.MAX_FILES && total <= ReportLimits.MAX_TOTAL_BYTES) return
            val largest = present.maxByOrNull { it.length() } ?: return
            failed[largest.name] = Failure(CollectFailure.LEFT_OUT, "left out to keep the report under the size limit")
            largest.delete()
            collected.remove(largest.name)
        }
    }

    /** Failures recorded when the evidence was preserved at startup. */
    private fun readEvidenceNotes() {
        val notes = File(dir, CrashEvidence.NOTES_FILE)
        if (!notes.isFile) return
        runCatching {
            val json = JSONObject(notes.readText())
            json.optJSONObject("failed")?.let { f -> for (k in f.keys()) failed[k] = Failure(CollectFailure.UNAVAILABLE, f.getString(k)) }
        }
    }

    /** Runs a step that produces [name]. A file already there is kept; a failure is noted, not thrown. */
    private fun step(name: String, optional: Boolean = false, block: () -> Unit) {
        val target = File(dir, name)
        if (target.isFile && target.length() > 0) {
            collected[name] = target.length()
            return
        }
        try {
            block()
            if (target.isFile && target.length() > 0) {
                collected[name] = target.length()
                failed.remove(name)
            } else if (!optional) {
                failed[name] = Failure(CollectFailure.UNAVAILABLE, "nothing to collect")
            }
        } catch (t: Throwable) {
            target.delete()
            if (!optional) failed[name] = Failure(CollectFailure.ERROR, t.message ?: t.javaClass.simpleName)
        }
    }

    companion object {
        /** How long to wait for the game to draw a frame and the dump to close. */
        private const val GS_DUMP_WAIT_MS = 20_000L

        /** The folder reports are built in, under the data root when there is one. */
        fun bundleRoot(context: Context): File {
            val base = MainActivityRuntime.dataRootOrNull()?.let(::File)?.takeIf { it.isDirectory || it.mkdirs() }
                ?: context.getExternalFilesDir(null)
                ?: context.filesDir
            return File(base, "bugreports").apply { mkdirs() }
        }
    }
}
