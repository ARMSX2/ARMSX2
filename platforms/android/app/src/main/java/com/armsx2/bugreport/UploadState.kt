// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** How a file is cut into the fixed-length parts the server takes. */
object PartMath {
    /** Parts needed for [size] bytes. A file always has at least one part. */
    fun count(size: Long, partSize: Int): Int {
        require(partSize > 0) { "partSize" }
        if (size <= 0) return 1
        return ((size + partSize - 1) / partSize).toInt()
    }

    /** Byte offset of part [n] (1-based). */
    fun offset(n: Int, partSize: Int): Long = (n - 1).toLong() * partSize

    /** Length of part [n]: [partSize] for all but the last, and 1..[partSize] for the last. */
    fun length(size: Long, n: Int, partSize: Int): Int {
        val remaining = size - offset(n, partSize)
        return remaining.coerceIn(0L, partSize.toLong()).toInt()
    }
}

/** One uploaded part, as the server acknowledged it. */
data class PartRecord(val n: Int, val etag: String)

/**
 * Whether a file that was given up on can be tried again. A file with no kind (a state file from
 * before kinds were recorded) is treated as [TRANSIENT]: Retry gives it another go.
 */
enum class FailureKind(val wire: String) {
    /** The cause may have passed: the file was not there (storage unmounted), for one. */
    TRANSIENT("transient"),

    /** Trying again cannot change it: the server refused the file, or its size changed on disk. */
    PERMANENT("permanent"),
    ;

    companion object {
        fun fromWire(text: String?): FailureKind? = entries.firstOrNull { it.wire == text }
    }
}

/** Where one file of a report stands. */
class FileState(
    val name: String,
    val size: Long,
    val sha256: String,
    /** The multipart upload id from the create response; null before the report is created. */
    var uploadId: String? = null,
    val parts: MutableList<PartRecord> = mutableListOf(),
    /** True once the server has completed the upload and confirmed its size. */
    var complete: Boolean = false,
    /** Why this file was given up on, or null. A file with a reason is not sent again until [retryFailures] clears it. */
    var failure: String? = null,
    var failureKind: FailureKind? = null,
) {
    /** The lowest part number not acknowledged yet, or null if every part is. */
    fun nextPart(partSize: Int): Int? {
        val total = PartMath.count(size, partSize)
        val done = parts.mapTo(HashSet()) { it.n }
        for (n in 1..total) if (n !in done) return n
        return null
    }

    /** Bytes acknowledged so far. */
    fun bytesDone(partSize: Int): Long =
        if (complete) size else parts.sumOf { PartMath.length(size, it.n, partSize).toLong() }

    /** True if nothing more will be sent for this file: sent in full, or given up on. */
    val settled: Boolean get() = complete || failure != null

    /** Gives a file that failed for a reason that may have passed another try. Returns true if it changed. */
    fun retryFailure(): Boolean {
        if (complete || failure == null || failureKind == FailureKind.PERMANENT) return false
        failure = null
        failureKind = null
        return true
    }

    /** Forgets everything the server was told about this file, so it can go up under a new report. */
    fun resetForNewReport() {
        uploadId = null
        parts.clear()
        complete = false
        failure = null
        failureKind = null
    }
}

/** What `POST /v1/reports` returned, kept so a later launch can carry on. */
data class CreatedReport(
    val id: String,
    val code: String,
    val token: String,
    /** Unix seconds after which the token is no longer good. */
    val expires: Long,
    val partSize: Int,
)

/** The phases a report passes through. [wire] is what `state.json` stores. */
enum class ReportPhase(val wire: String) {
    /** Evidence collected (a crash), waiting for the user to decide whether to send it. */
    EVIDENCE("evidence"),

    /** Bundle written and hashed; nothing has reached the server. */
    READY("ready"),

    /** Created on the server, parts going up. */
    UPLOADING("uploading"),

    /** The last attempt failed in a way that may pass; tried again on the next launch or Retry. */
    WAITING_RETRY("waiting-retry"),

    /** `finish` succeeded. */
    FINISHED("finished"),

    /** The user stopped sending. What the server already had was kept and `finish` was called if it could be. */
    STOPPED("stopped"),

    /** Given up on for good. */
    FAILED("failed"),
    ;

    companion object {
        fun fromWire(text: String?): ReportPhase = entries.firstOrNull { it.wire == text } ?: READY
    }
}

/**
 * `state.json` in a report's bundle folder: enough to resume an upload on a later launch without
 * asking the server anything it already answered. Updated after every part.
 */
class UploadState(
    val localId: String,
    val kind: String,
    val createdAt: Long,
    /** The body of `POST /v1/reports`, minus the file list, which is rebuilt from [files]. */
    val request: JSONObject,
    val files: MutableList<FileState>,
    var phase: ReportPhase = ReportPhase.READY,
    var created: CreatedReport? = null,
    var finishedAt: Long = 0L,
    /** The server's or the exception's own words for the last problem, for logs; never shown as they are. Null if none. */
    var lastError: String? = null,
    /** [UploadReason.key] of the last problem, for the screens. Null if none. */
    var lastReason: String? = null,
    /** Files the server reported missing from `finish`. */
    var missing: List<String> = emptyList(),
    /** The server said the upload link is no longer good (`bad_token`, `token_expired`). What is left needs a new report. */
    var tokenDead: Boolean = false,
    /** Codes of the reports this one carries on from, oldest first: the files they took are not in [files]. */
    var earlierCodes: List<String> = emptyList(),
) {
    val totalBytes: Long get() = files.sumOf { it.size }

    fun bytesDone(): Long {
        val partSize = created?.partSize ?: return 0L
        return files.sumOf { it.bytesDone(partSize) }
    }

    /** Bytes still to send: what is not complete and not given up on for good. */
    fun bytesLeft(): Long {
        val partSize = created?.partSize
        return files.filter { !it.complete && it.failureKind != FailureKind.PERMANENT }
            .sumOf { if (partSize == null) it.size else it.size - it.bytesDone(partSize) }
    }

    /** The files a new report would carry: not sent, and not refused for good. */
    fun leftover(): List<FileState> = files.filter { !it.complete && it.failureKind != FailureKind.PERMANENT }

    /**
     * Gets the state ready for another try. Files that failed for a reason that may have passed
     * become unsettled again. If the server said the link is dead, the report cannot go on: what
     * arrived stays under the old code, and this state becomes a new report for [leftover] with the
     * old code remembered in [earlierCodes] and named in the summary. Returns false when there is
     * nothing left to send either way.
     */
    fun prepareRetry(): Boolean {
        if (phase == ReportPhase.FINISHED) return false
        for (f in files) f.retryFailure()
        if (tokenDead) {
            val rest = leftover()
            if (rest.isEmpty()) return false
            val old = created
            if (old != null && old.code !in earlierCodes) earlierCodes = earlierCodes + old.code
            files.retainAll(rest.toSet())
            for (f in files) f.resetForNewReport()
            created = null
            tokenDead = false
            missing = emptyList()
            request.optJSONObject("summary")?.put("continues", earlierCodes.joinToString(", "))
        }
        lastError = null
        lastReason = null
        phase = if (created == null) ReportPhase.READY else ReportPhase.WAITING_RETRY
        return true
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("schema", 1)
        put("localId", localId)
        put("kind", kind)
        put("createdAt", createdAt)
        put("phase", phase.wire)
        put("request", request)
        put("finishedAt", finishedAt)
        lastError?.let { put("lastError", it) }
        lastReason?.let { put("lastReason", it) }
        put("tokenDead", tokenDead)
        put("earlierCodes", JSONArray(earlierCodes))
        put("missing", JSONArray(missing))
        created?.let {
            put(
                "created",
                JSONObject()
                    .put("id", it.id).put("code", it.code).put("token", it.token)
                    .put("expires", it.expires).put("partSize", it.partSize),
            )
        }
        put(
            "files",
            JSONArray().also { array ->
                for (f in files) {
                    array.put(
                        JSONObject()
                            .put("name", f.name).put("size", f.size).put("sha256", f.sha256)
                            .put("uploadId", f.uploadId ?: JSONObject.NULL)
                            .put("complete", f.complete)
                            .put("failure", f.failure ?: JSONObject.NULL)
                            .put("failureKind", f.failureKind?.wire ?: JSONObject.NULL)
                            .put(
                                "parts",
                                JSONArray().also { parts ->
                                    for (p in f.parts) parts.put(JSONObject().put("n", p.n).put("etag", p.etag))
                                },
                            ),
                    )
                }
            },
        )
    }

    /** The body for `POST /v1/reports`: [request] plus the file list. */
    fun createBody(): JSONObject {
        val body = JSONObject(request.toString())
        body.put(
            "files",
            JSONArray().also { array ->
                for (f in files) array.put(JSONObject().put("name", f.name).put("size", f.size).put("sha256", f.sha256))
            },
        )
        return body
    }

    /** Writes [toJson] to [file] through a temporary file, so a kill mid-write leaves the old state. */
    fun save(file: File) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(toJson().toString())
        if (!tmp.renameTo(file)) {
            file.delete()
            tmp.renameTo(file)
        }
    }

    companion object {
        fun fromJson(json: JSONObject): UploadState {
            val files = mutableListOf<FileState>()
            val array = json.optJSONArray("files") ?: JSONArray()
            for (i in 0 until array.length()) {
                val f = array.getJSONObject(i)
                val parts = mutableListOf<PartRecord>()
                val partArray = f.optJSONArray("parts") ?: JSONArray()
                for (j in 0 until partArray.length()) {
                    val p = partArray.getJSONObject(j)
                    parts += PartRecord(p.getInt("n"), p.getString("etag"))
                }
                files += FileState(
                    name = f.getString("name"),
                    size = f.getLong("size"),
                    sha256 = f.getString("sha256"),
                    uploadId = f.optString("uploadId", "").takeIf { it.isNotEmpty() && !f.isNull("uploadId") },
                    parts = parts,
                    complete = f.optBoolean("complete", false),
                    failure = f.optString("failure", "").takeIf { it.isNotEmpty() && !f.isNull("failure") },
                    failureKind = f.optString("failureKind", "").takeIf { !f.isNull("failureKind") }?.let(FailureKind::fromWire),
                )
            }
            val missing = mutableListOf<String>()
            json.optJSONArray("missing")?.let { m -> for (i in 0 until m.length()) missing += m.getString(i) }
            val earlier = mutableListOf<String>()
            json.optJSONArray("earlierCodes")?.let { m -> for (i in 0 until m.length()) earlier += m.getString(i) }
            val created = json.optJSONObject("created")?.let {
                CreatedReport(
                    id = it.getString("id"),
                    code = it.getString("code"),
                    token = it.getString("token"),
                    expires = it.getLong("expires"),
                    partSize = it.getInt("partSize"),
                )
            }
            return UploadState(
                localId = json.getString("localId"),
                kind = json.optString("kind", "other"),
                createdAt = json.optLong("createdAt", 0L),
                request = json.optJSONObject("request") ?: JSONObject(),
                files = files,
                phase = ReportPhase.fromWire(json.optString("phase", "ready")),
                created = created,
                finishedAt = json.optLong("finishedAt", 0L),
                lastError = json.optString("lastError", "").takeIf { it.isNotEmpty() },
                lastReason = json.optString("lastReason", "").takeIf { it.isNotEmpty() },
                missing = missing,
                tokenDead = json.optBoolean("tokenDead", false),
                earlierCodes = earlier,
            )
        }

        fun load(file: File): UploadState? = try {
            fromJson(JSONObject(file.readText()))
        } catch (_: Exception) {
            null
        }
    }
}
