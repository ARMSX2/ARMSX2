// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.SharedPreferences
import androidx.compose.runtime.mutableIntStateOf
import org.json.JSONArray
import org.json.JSONObject

/** Where a past report stands, as "My reports" shows it. */
enum class HistoryStatus(val wire: String) {
    /** Evidence kept from a crash; the user has not said to send it. */
    NOT_SENT("not-sent"),
    UPLOADING("uploading"),
    WAITING_RETRY("waiting-retry"),

    /** On a metered connection with a big report left: held back until Wi-Fi, or until the user says send now. */
    WAITING_WIFI("waiting-wifi"),

    /** The user stopped sending. */
    STOPPED("stopped"),
    SENT("sent"),
    FAILED("failed"),
    ;

    companion object {
        fun fromWire(text: String?): HistoryStatus = entries.firstOrNull { it.wire == text } ?: FAILED
    }
}

/** One line of "My reports". */
data class HistoryEntry(
    /** The folder name under `bugreports/` while the bundle is still on the device. */
    val localId: String,
    /** The report code, empty until the server has created the report. */
    val code: String,
    val kind: String,
    val createdAt: Long,
    val title: String,
    val serial: String,
    val status: HistoryStatus,
    val bytes: Long,
    /**
     * Why a failed or waiting report is in that state: an i18n key (an [UploadReason.key] or a
     * `bugreport.note.*` one), never English text. "" if there is nothing to say.
     */
    val note: String = "",
    /** Codes of the reports this one carries on from ([UploadState.earlierCodes]), comma separated; "" if none. */
    val earlierCodes: String = "",
) {
    fun toJson(): JSONObject = JSONObject()
        .put("localId", localId).put("code", code).put("kind", kind).put("createdAt", createdAt)
        .put("title", title).put("serial", serial).put("status", status.wire).put("bytes", bytes).put("note", note)
        .put("earlierCodes", earlierCodes)

    companion object {
        fun fromJson(json: JSONObject) = HistoryEntry(
            localId = json.optString("localId"),
            code = json.optString("code"),
            kind = json.optString("kind", "other"),
            createdAt = json.optLong("createdAt"),
            title = json.optString("title"),
            serial = json.optString("serial"),
            status = HistoryStatus.fromWire(json.optString("status")),
            bytes = json.optLong("bytes"),
            note = json.optString("note"),
            earlierCodes = json.optString("earlierCodes"),
        )
    }
}

/**
 * The reports this device has made, newest first, kept in preferences so they outlast the bundle
 * folders (which are deleted a week after a successful send) and a change of data folder. This is
 * what lets someone who lost their report code find it again.
 */
class ReportHistory(private val prefs: SharedPreferences) {
    /** Counts writes. Reading it in [list] makes a composable that lists the history redraw when it changes. */
    private val changes = mutableIntStateOf(0)

    @Synchronized
    fun list(): List<HistoryEntry> {
        if (changes.intValue < 0) return emptyList() // never true; the read is what subscribes
        return read().sortedByDescending { it.createdAt }
    }

    @Synchronized
    fun find(localId: String): HistoryEntry? = read().firstOrNull { it.localId == localId }

    /** Adds [entry], or replaces the one with the same [HistoryEntry.localId]. */
    @Synchronized
    fun upsert(entry: HistoryEntry) {
        val all = read().filterNot { it.localId == entry.localId } + entry
        write(all.sortedByDescending { it.createdAt }.take(MAX_ENTRIES))
    }

    @Synchronized
    fun remove(localId: String) {
        write(read().filterNot { it.localId == localId })
    }

    private fun read(): List<HistoryEntry> {
        val text = prefs.getString(KEY, null) ?: return emptyList()
        return try {
            val array = JSONArray(text)
            (0 until array.length()).map { HistoryEntry.fromJson(array.getJSONObject(it)) }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun write(entries: List<HistoryEntry>) {
        val array = JSONArray()
        for (e in entries) array.put(e.toJson())
        prefs.edit().putString(KEY, array.toString()).apply()
        changes.intValue++
    }

    companion object {
        private const val KEY = "history"
        const val MAX_ENTRIES = 50
    }
}
