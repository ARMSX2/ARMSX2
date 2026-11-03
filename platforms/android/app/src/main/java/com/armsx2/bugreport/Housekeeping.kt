// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File

/**
 * Deletes report folders that have outlived their purpose, so reports do not pile up in the data
 * folder: a sent report's files after a week (the "My reports" line stays), crash evidence nobody
 * sent after a week or beyond the newest few, and folders a cancelled report left behind.
 */
object Housekeeping {
    /** The history note (an i18n key) for a report whose files were cleaned up before it could be sent. */
    const val NOTE_EXPIRED = "bugreport.note.expired"

    private const val WEEK_MS = 7L * 24 * 3600 * 1000
    private const val DAY_MS = 24L * 3600 * 1000

    /** What is known about one folder under `bugreports/`. */
    data class Info(
        val name: String,
        /** `state.json` exists: the folder is a report, not loose evidence. */
        val hasState: Boolean,
        val phase: ReportPhase?,
        val createdAt: Long,
        val finishedAt: Long,
        val lastModified: Long,
        /** For unsent crash evidence: 0 for the newest, 1 for the next... */
        val unsentRank: Int,
    )

    enum class Verdict { KEEP, DELETE, DELETE_AND_MARK_EXPIRED }

    /** The decision for one folder. Pure, so the rules can be tested without a file system. */
    fun decide(info: Info, now: Long): Verdict {
        if (!info.hasState) {
            return if (info.name.startsWith(CrashEvidence.FOLDER_PREFIX)) {
                // Unsent crash evidence: a week, and no more than a few of them.
                if (now - info.lastModified > CrashEvidence.KEEP_MS || info.unsentRank >= CrashEvidence.KEEP_COUNT) {
                    Verdict.DELETE
                } else {
                    Verdict.KEEP
                }
            } else {
                // A report the user backed out of, or one whose process died while it was being made.
                if (now - info.lastModified > DAY_MS) Verdict.DELETE else Verdict.KEEP
            }
        }
        return when (info.phase) {
            ReportPhase.FINISHED -> if (now - info.finishedAt > WEEK_MS) Verdict.DELETE else Verdict.KEEP
            // The user chose to stop; the line stays, the files go with the rest.
            ReportPhase.STOPPED -> if (now - info.createdAt > WEEK_MS) Verdict.DELETE else Verdict.KEEP
            ReportPhase.FAILED, ReportPhase.WAITING_RETRY, ReportPhase.UPLOADING, ReportPhase.READY ->
                if (now - info.createdAt > WEEK_MS) Verdict.DELETE_AND_MARK_EXPIRED else Verdict.KEEP
            else -> Verdict.KEEP
        }
    }

    fun run(history: ReportHistory, root: File, now: Long = System.currentTimeMillis()) {
        val unsent = CrashEvidence.unsent(root).map { it.name }
        val dirs = root.listFiles { f -> f.isDirectory }.orEmpty()
        for (dir in dirs) {
            val stateFile = File(dir, ReportUploader.STATE_FILE)
            val state = if (stateFile.isFile) UploadState.load(stateFile) else null
            val info = Info(
                name = dir.name,
                hasState = state != null,
                phase = state?.phase,
                createdAt = state?.createdAt ?: dir.lastModified(),
                finishedAt = state?.finishedAt ?: 0L,
                lastModified = dir.lastModified(),
                unsentRank = unsent.indexOf(dir.name).coerceAtLeast(0),
            )
            when (decide(info, now)) {
                Verdict.KEEP -> Unit
                Verdict.DELETE -> {
                    dir.deleteRecursively()
                    // Unsent evidence has no further life; a sent report keeps its line.
                    if (!info.hasState) history.remove(dir.name)
                }
                Verdict.DELETE_AND_MARK_EXPIRED -> {
                    dir.deleteRecursively()
                    history.find(dir.name)?.let {
                        if (it.status != HistoryStatus.SENT) history.upsert(it.copy(status = HistoryStatus.FAILED, note = NOTE_EXPIRED))
                    }
                }
            }
        }
    }
}
