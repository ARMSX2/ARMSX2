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
    /** Why this file was given up on, or null. A file with a reason is not tried again. */
    var failure: String? = null,
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
    /** The last error shown to the user, or null. */
    var lastError: String? = null,
    /** Files the server reported missing from `finish`. */
    var missing: List<String> = emptyList(),
) {
    val totalBytes: Long get() = files.sumOf { it.size }

    fun bytesDone(): Long {
        val partSize = created?.partSize ?: return 0L
        return files.sumOf { it.bytesDone(partSize) }
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
                )
            }
            val missing = mutableListOf<String>()
            json.optJSONArray("missing")?.let { m -> for (i in 0 until m.length()) missing += m.getString(i) }
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
                missing = missing,
            )
        }

        fun load(file: File): UploadState? = try {
            fromJson(JSONObject(file.readText()))
        } catch (_: Exception) {
            null
        }
    }
}
