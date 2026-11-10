// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import org.json.JSONObject
import java.io.File

/** What the report is about. [wire] is the value the server stores. */
enum class ReportKind(val wire: String) {
    CRASH("crash"),
    GRAPHICS("graphics"),
    SLOW("slow"),
    OTHER("other"),
    ;

    /** Whether the consent screen starts with the GS dump switched on. */
    val gsDumpByDefault: Boolean get() = this == GRAPHICS || this == SLOW

    /**
     * Whether the consent screen starts with the save state switched on. On for graphics too: the
     * GS dump records frames after the pause, so a save state of the paused moment is often the
     * only way back to what the user saw.
     */
    val savestateByDefault: Boolean get() = this == GRAPHICS || this == SLOW

    companion object {
        fun fromWire(text: String?): ReportKind = entries.firstOrNull { it.wire == text } ?: OTHER
    }
}

/** The game a report is about. [crc] is 8 hex digits, or "" if it could not be worked out. */
data class ReportGame(
    val serial: String,
    val crc: String,
    val title: String,
    /** The key per-game settings are stored under, or null if the game has none. */
    val settingsKey: String?,
)

/**
 * CRCs as a report carries them: 8 hex digits, upper case, or "" when not known. The core reports
 * 00000000 for an image it could not identify, which is "not known" and not a CRC.
 */
object ReportCrc {
    private val PATTERN = Regex("[0-9A-Fa-f]{8}")
    private const val UNIDENTIFIED = "00000000"

    fun normalize(raw: String?): String {
        val text = raw?.trim().orEmpty()
        return if (PATTERN.matches(text) && text != UNIDENTIFIED) text.uppercase() else ""
    }

    /** The first of [candidates], in order, that is a usable CRC; "" if none is. */
    fun firstKnown(vararg candidates: String?): String =
        candidates.asSequence().map(::normalize).firstOrNull { it.isNotEmpty() } ?: ""
}

/** The commit the build is from, as `report.json` and the create call name it. */
object AppCommit {
    const val UNKNOWN = "unknown"

    /**
     * The first of [candidates] that names a commit, in order of trust; "unknown" if none does.
     * The Gradle build's own value comes first because it exists whether or not the driver report
     * could be written, and the driver report's value is what is left when the build had no git.
     */
    fun pick(vararg candidates: String?): String =
        candidates.asSequence().map { it?.trim().orEmpty() }
            .firstOrNull { it.isNotEmpty() && !it.equals(UNKNOWN, ignoreCase = true) } ?: UNKNOWN
}

/** The file names a report can contain. All match the server's `^[a-z0-9][a-z0-9._-]{0,63}$`. */
object ReportFiles {
    const val REPORT = "report.json"
    const val SYSTEM = "system.json"
    const val EMULOG = "emulog.txt.gz"

    /** The emulog of the run before: the log of the last earlier run that started a game. See [LogBackup]. */
    const val EMULOG_PREVIOUS = "emulog-previous.txt.gz"
    const val LOGCAT = "logcat.txt.gz"
    const val SETTINGS = "settings.json.gz"
    const val JAVA_CRASHES = "java-crashes.txt.gz"
    const val EXIT_INFO = "exit-info.txt.gz"
    const val TOMBSTONE = "tombstone.pb"
    const val ANR = "anr.txt.gz"
    const val GS_DUMP = "gsdump.gs.zip"
    const val SAVESTATE = "savestate.p2s"

    /** Everything that may be in a bundle, in the order the consent screen lists it. */
    val ALL = listOf(
        REPORT, SYSTEM, EMULOG, EMULOG_PREVIOUS, LOGCAT, SETTINGS, JAVA_CRASHES, EXIT_INFO, TOMBSTONE, ANR, GS_DUMP, SAVESTATE,
    )

    private val NAME = Regex("^[a-z0-9][a-z0-9._-]{0,63}$")

    /** The server writes this one itself when a report is finished, so a client may not send it. */
    private const val RESERVED = "manifest.json"

    /** True for a name the server will accept. */
    fun isValidName(name: String): Boolean = name != RESERVED && NAME.matches(name)
}

/** Limits the server enforces, checked before anything is sent. */
object ReportLimits {
    const val MAX_FILES = 16
    const val MAX_FILE_BYTES = 512L * 1024 * 1024
    const val MAX_TOTAL_BYTES = 768L * 1024 * 1024
    const val MAX_DESCRIPTION = 2000

    /** The consent screen's metered-network note appears over this size. */
    const val METERED_NOTE_BYTES = 25L * 1024 * 1024
}

/**
 * Whether a report may go out without the user pressing anything: on an unmetered connection, or
 * when little is left to send. A report the user just sent, or one they chose to "Send now", is
 * their decision and goes regardless.
 */
object MeteredRule {
    fun allowed(metered: Boolean, bytesLeft: Long, userAsked: Boolean): Boolean =
        userAsked || !metered || bytesLeft < ReportLimits.METERED_NOTE_BYTES
}

/** One item the consent screen lists. */
data class BundleItem(
    val name: String,
    val bytes: Long,
    /** True if the size is a guess made before the item exists (a save state or GS dump). */
    val estimate: Boolean = false,
)

/**
 * What the app needs to know to make a report: set when the flow opens and then fixed.
 * [crashDir] is the folder of evidence preserved at startup, for a report about a crash; for
 * anything else it is null and a new folder is made.
 */
data class ReportOrigin(
    val game: ReportGame?,
    /** A game is running and paused (or running) right now, so a save state and GS dump can be taken. */
    val canCapture: Boolean,
    /** Installed driver pack id, "" for the system driver. */
    val driverId: String,
    val driverName: String,
    val crashDir: File? = null,
    val crashMarker: CrashMarker.Marker? = null,
    /** The stats overlay's numbers when the flow opened, from `getPerfSnapshot`. */
    val perfAtOpen: String? = null,
)

/** `report.json`, and the summary the create call carries, built from plain values. */
object ReportJson {
    private const val SUMMARY_FIELD_MAX = 128
    private const val SUMMARY_MAX_BYTES = 7_800

    /** The `summary` object of `POST /v1/reports`: small, searchable fields only. */
    fun summary(
        description: String,
        game: ReportGame?,
        device: String,
        soc: String,
        gpu: String,
        driver: String,
        os: String,
    ): JSONObject {
        var text = description.take(ReportLimits.MAX_DESCRIPTION)
        while (true) {
            val summary = JSONObject().apply {
                put("description", text)
                put("serial", (game?.serial ?: "").take(SUMMARY_FIELD_MAX))
                put("crc", (game?.crc ?: "").take(SUMMARY_FIELD_MAX))
                put("title", (game?.title ?: "").take(SUMMARY_FIELD_MAX))
                put("device", device.take(SUMMARY_FIELD_MAX))
                put("soc", soc.take(SUMMARY_FIELD_MAX))
                put("gpu", gpu.take(SUMMARY_FIELD_MAX))
                put("driver", driver.take(SUMMARY_FIELD_MAX))
                put("os", os.take(SUMMARY_FIELD_MAX))
            }
            // The server caps the summary at 8 KB of UTF-8. 2000 characters is 6 KB at worst, but the
            // other fields are not all ASCII either, so check rather than assume.
            if (summary.toString().toByteArray(Charsets.UTF_8).size <= SUMMARY_MAX_BYTES || text.isEmpty()) return summary
            text = text.dropLast(100)
        }
    }

    /**
     * The `request` half of `state.json`: the create body without its file list. Kept so a retry on
     * a later launch sends the same thing the user agreed to.
     */
    fun createRequest(kind: ReportKind, appVersion: String, appCommit: String, flavor: String, summary: JSONObject): JSONObject =
        JSONObject().apply {
            put("schema", 1)
            put("kind", kind.wire)
            put("platform", "android")
            put("app", JSONObject().put("version", appVersion).put("commit", appCommit).put("flavor", flavor))
            put("summary", summary)
        }
}

/** The consent screen's list, as a function of what has been collected and what is switched on. */
object ConsentItems {
    /** `report.json` is written last; its size is only roughly known. */
    const val REPORT_JSON_ESTIMATE = 4L * 1024
    const val SAVESTATE_ESTIMATE = 12L * 1024 * 1024
    const val GS_DUMP_ESTIMATE = 25L * 1024 * 1024

    fun build(light: List<BundleItem>, canCapture: Boolean, savestate: Boolean, gsDump: Boolean): List<BundleItem> {
        val items = mutableListOf<BundleItem>()
        items += BundleItem(ReportFiles.REPORT, REPORT_JSON_ESTIMATE, estimate = true)
        items += light.filter { it.name != ReportFiles.REPORT }
        if (canCapture && gsDump) items += BundleItem(ReportFiles.GS_DUMP, GS_DUMP_ESTIMATE, estimate = true)
        if (canCapture && savestate) items += BundleItem(ReportFiles.SAVESTATE, SAVESTATE_ESTIMATE, estimate = true)
        return items.sortedBy { ReportFiles.ALL.indexOf(it.name).let { i -> if (i < 0) Int.MAX_VALUE else i } }
    }
}
