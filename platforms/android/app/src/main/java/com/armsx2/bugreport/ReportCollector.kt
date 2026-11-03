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

/** How the collector asks the running game to move, without knowing how the app pauses it. */
interface GameControl {
    fun isPaused(): Boolean
    fun resume()
    fun pause()

    /** Waits up to [timeoutMs] for the game to be running after [resume]. */
    fun awaitRunning(timeoutMs: Long): Boolean
}

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
    val failed = linkedMapOf<String, String>()

    /** The GS driver report as it will be sent, once either source of it has produced one. */
    private var systemReport: String? = null

    /** Gathers the files every report carries. */
    fun collectLight() {
        dir.mkdirs()
        readEvidenceNotes()
        // A crash report's logs were set aside when the app started. If one could not be, the
        // current emulog and logcat belong to a different run and must not stand in for it.
        if (origin.crashDir == null) {
            step(ReportFiles.EMULOG) { writeEmulog() }
            step(ReportFiles.LOGCAT) {
                val result = LogcatSnapshot.write(scrubber, File(dir, ReportFiles.LOGCAT))
                if (!result.ok) throw IOException(result.error ?: "logcat failed")
            }
        } else {
            for (name in listOf(ReportFiles.EMULOG, ReportFiles.LOGCAT)) {
                val f = File(dir, name)
                if (f.isFile && f.length() > 0) collected[name] = f.length()
                else failed.putIfAbsent(name, "not saved when the app restarted")
            }
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

    /** Writes a save state of the paused game. The game is not resumed. */
    fun captureSavestate(): Boolean {
        val target = File(dir, ReportFiles.SAVESTATE)
        target.delete()
        val ok = runCatching { NativeApp.saveStateToPath(target.absolutePath) }.getOrDefault(false)
        if (!ok || !target.isFile || target.length() == 0L) {
            target.delete()
            failed[ReportFiles.SAVESTATE] =
                "The game could not be saved just now (the memory card may have been busy)."
            return false
        }
        collected[ReportFiles.SAVESTATE] = target.length()
        return true
    }

    /**
     * Takes a GS dump of the next frame. The dump is opened when the GS draws, so the game is
     * resumed for the time it takes and paused again afterwards if it was paused before.
     */
    fun captureGsDump(control: GameControl): Boolean {
        val work = File(dir, ".gsdump").apply { deleteRecursively(); mkdirs() }
        try {
            val base = File(work, "dump")
            val zip = File(work, "dump.gs.zip")
            val wasPaused = control.isPaused()
            val queued = runCatching { NativeApp.captureGsDumpTo(base.absolutePath, 1) }.getOrDefault(false)
            if (!queued) {
                failed[ReportFiles.GS_DUMP] = "The graphics dump could not be started (is a game running?)."
                return false
            }
            try {
                if (wasPaused) {
                    control.resume()
                    control.awaitRunning(3_000)
                }
                val deadline = System.currentTimeMillis() + GS_DUMP_WAIT_MS
                while (!zip.isFile && System.currentTimeMillis() < deadline) Thread.sleep(100)
            } finally {
                if (wasPaused) control.pause()
            }
            if (!zip.isFile) {
                failed[ReportFiles.GS_DUMP] = "The game did not draw a frame in time, so there is no graphics dump."
                return false
            }
            val target = File(dir, ReportFiles.GS_DUMP)
            val report = scrubber.rewriteGsBundle(zip, target)
            // The dump's own report was written when the renderer was running: better than the
            // standalone one, which has no renderer sections.
            if (report != null) {
                File(dir, ReportFiles.SYSTEM).writeText(report)
                systemReport = report
                collected[ReportFiles.SYSTEM] = File(dir, ReportFiles.SYSTEM).length()
            }
            collected[ReportFiles.GS_DUMP] = target.length()
            return true
        } catch (e: Exception) {
            failed[ReportFiles.GS_DUMP] = e.message ?: "The graphics dump could not be read."
            File(dir, ReportFiles.GS_DUMP).delete()
            return false
        } finally {
            work.deleteRecursively()
        }
    }

    /**
     * Writes `report.json` and `state.json`, hashes every file and returns the state the uploader
     * works from. Files over the server's limits are dropped (the biggest first) and noted.
     */
    fun finish(kind: ReportKind, description: String, perfAfterResume: String?): UploadState {
        enforceLimits()
        val commit = systemReport?.let(::appCommitFrom) ?: "unknown"
        val report = buildReport(kind, description, commit, perfAfterResume)
        File(dir, ReportFiles.REPORT).writeText(report.toString(2))
        collected[ReportFiles.REPORT] = File(dir, ReportFiles.REPORT).length()

        val files = ReportFiles.ALL.mapNotNull { name ->
            val f = File(dir, name)
            if (!f.isFile || f.length() == 0L) null else FileState(name, f.length(), Scrubber.sha256(f))
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

    private fun writeEmulog() {
        val root = MainActivityRuntime.dataRootOrNull() ?: throw IOException("the data folder is not available")
        val src = File(root, "logs/emulog.txt")
        if (!src.isFile) throw IOException("there is no emulog.txt yet (no game has been started)")
        val length = src.length()
        src.inputStream().use { input ->
            scrubber.copyText(input, length, GZIPOutputStream(File(dir, ReportFiles.EMULOG).outputStream().buffered()))
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
            JSONArray().also { a -> for ((name, why) in failed) a.put(JSONObject().put("name", name).put("reason", why)) },
        )
        return report
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
        val status = runCatching { NativeApp.getCustomDriverStatus() }.getOrNull()
        if (status != null) json.put("native_status", scrubber.json(parse(status)))
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

    private fun appCommitFrom(systemJson: String): String = runCatching {
        JSONObject(systemJson).optJSONObject("app")?.optString("commit").orEmpty().ifBlank { null }
    }.getOrNull() ?: "unknown"

    /** Drops files the server would refuse, the largest first, and says so in [failed]. */
    private fun enforceLimits() {
        for (name in ReportFiles.ALL) {
            val f = File(dir, name)
            if (f.isFile && f.length() > ReportLimits.MAX_FILE_BYTES) {
                failed[name] = "Too large to send (${f.length() / (1024 * 1024)} MB)."
                f.delete()
                collected.remove(name)
            }
        }
        while (true) {
            val present = ReportFiles.ALL.map { File(dir, it) }.filter { it.isFile }
            val total = present.sumOf { it.length() }
            if (present.size <= ReportLimits.MAX_FILES && total <= ReportLimits.MAX_TOTAL_BYTES) return
            val largest = present.maxByOrNull { it.length() } ?: return
            failed[largest.name] = "Left out to keep the report under the size limit."
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
            json.optJSONObject("failed")?.let { f -> for (k in f.keys()) failed[k] = f.getString(k) }
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
                failed[name] = "nothing to collect"
            }
        } catch (t: Throwable) {
            target.delete()
            if (!optional) failed[name] = t.message ?: t.javaClass.simpleName
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
