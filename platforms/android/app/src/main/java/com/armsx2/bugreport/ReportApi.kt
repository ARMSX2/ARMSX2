// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL

/** The report server's address. One constant, so moving to another host is a one-line change. */
internal const val REPORT_SERVER = "https://reports.ps2ktxpak.net"

/**
 * A call the server refused or that never got an answer.
 *
 * @param status HTTP status, or 0 when the request failed before one came back (no network, timeout).
 * @param code the server's machine-readable error, or "" if it sent none (or sent something that was
 *   not the server's JSON at all, which is what a captive portal or the CDN's own error page looks like).
 */
class ApiException(
    val status: Int,
    val code: String,
    message: String,
    cause: Throwable? = null,
    /** The server's `Retry-After`, in seconds, or 0 if it sent none. */
    val retryAfterSeconds: Int = 0,
) : IOException(message, cause) {
    /**
     * The token was refused by the server itself: 401 with `bad_token` or `token_expired`. Nothing
     * else counts. A 403, or an answer that is not the server's JSON, comes from something in front of
     * the server (the CDN, a captive portal) and says nothing about the token.
     */
    val tokenRejected: Boolean get() = status == 401 && (code == "bad_token" || code == "token_expired")

    /**
     * A failure that may pass on its own: no connection, a server error, being rate limited, or an
     * answer the server did not write (a 403 or any error without the server's JSON, an unreadable
     * success), which is how a CDN or a captive portal says "not now".
     */
    val retryable: Boolean
        get() = !tokenRejected && (
            status == 0 || status >= 500 || status == 408 || status == 429 || status == 403 ||
                code == "bad_response" || (status >= 400 && code.isEmpty())
            )

    /**
     * True if trying again within the next minute or two is worth it. The daily byte budget is spent
     * for hours (`budget_exhausted`, Retry-After 3600), so waiting out a few seconds of backoff
     * only delays the answer; that failure goes straight to "waiting to retry".
     */
    val worthBackoff: Boolean get() = retryable && code != "budget_exhausted" && retryAfterSeconds <= MAX_BACKOFF_WAIT_SECONDS

    /** What went wrong, for the screens, without the server's or the exception's own wording. */
    val reason: UploadReason
        get() = when {
            tokenRejected -> UploadReason.TOKEN
            status == 0 -> UploadReason.NETWORK
            status == 429 || code == "budget_exhausted" -> UploadReason.BUSY
            retryable -> UploadReason.SERVER
            else -> UploadReason.REFUSED
        }

    companion object {
        /** A Retry-After longer than this is not slept through. The rate limiter asks for 60. */
        const val MAX_BACKOFF_WAIT_SECONDS = 90
    }
}

/** Why an upload is not going through, as the screens say it. Each has a string; the server's own text is only detail. */
enum class UploadReason(val key: String) {
    /** No connection, or it dropped. */
    NETWORK("bugreport.reason.network"),

    /** The server or something in front of it had a problem; it may pass. */
    SERVER("bugreport.reason.server"),

    /** Rate limited, or today's budget is spent. */
    BUSY("bugreport.reason.busy"),

    /** A metered connection and a big report: waiting for Wi-Fi. */
    WIFI("bugreport.reason.wifi"),

    /** The server will not take it and asking again will not change that. */
    REFUSED("bugreport.reason.refused"),

    /** The upload link for this report ended; what is left goes in a new report. */
    TOKEN("bugreport.reason.token"),

    /** Something on this device: the state file or a report file is gone or changed. */
    LOCAL("bugreport.reason.local"),

    /** The user stopped sending. */
    STOPPED("bugreport.reason.stopped"),
    ;

    companion object {
        fun fromKey(key: String?): UploadReason? = entries.firstOrNull { it.key == key }
    }
}

/** The five calls the client makes. A seam so the upload logic can be exercised without a network. */
interface ReportApi {
    fun create(body: JSONObject): CreatedResponse

    /**
     * Sends bytes [offset, offset+length) of [file] as part [n] and returns the part's etag.
     * [onBytes] is told how many bytes of this part have gone out, for a progress bar.
     */
    fun putPart(
        reportId: String,
        name: String,
        n: Int,
        token: String,
        uploadId: String,
        file: File,
        offset: Long,
        length: Int,
        onBytes: (Int) -> Unit,
    ): String

    /** Completes one file and returns the size the server stored. */
    fun complete(reportId: String, name: String, token: String, uploadId: String, parts: List<PartRecord>): Long

    fun finish(reportId: String, token: String): FinishResponse
}

data class CreatedResponse(val created: CreatedReport, val uploads: Map<String, String>)

data class FinishResponse(val code: String, val missing: List<String>)

/** Thrown out of an upload when the user stops it. Deliberately not an IOException: the HTTP code treats those as a lost connection. */
class UploadCancelled : RuntimeException("The upload was stopped.")

/** Cuts [conn] if [touch] has not been called for [stallMs]: HttpURLConnection has no write timeout. */
private class StallGuard(private val conn: HttpURLConnection, private val stallMs: Long) : AutoCloseable {
    @Volatile
    private var last = System.nanoTime()

    @Volatile
    private var closed = false

    private val watcher = kotlin.concurrent.thread(isDaemon = true, name = "bugreport-stall-guard") {
        while (!closed) {
            try {
                Thread.sleep(1_000)
            } catch (_: InterruptedException) {
                return@thread
            }
            if ((System.nanoTime() - last) / 1_000_000 > stallMs) {
                runCatching { conn.disconnect() }
                return@thread
            }
        }
    }

    fun touch() {
        last = System.nanoTime()
    }

    override fun close() {
        closed = true
        watcher.interrupt()
    }
}

/** [ReportApi] over HttpURLConnection, with every part streamed from disk at a fixed length. */
class HttpReportApi(private val baseUrl: String = REPORT_SERVER) : ReportApi {
    private companion object {
        const val CONNECT_TIMEOUT_MS = 20_000
        const val READ_TIMEOUT_MS = 60_000
        const val STALL_MS = 60_000L
    }

    override fun create(body: JSONObject): CreatedResponse {
        val json = postJson("/v1/reports", body, token = null)
        return try {
            val uploads = mutableMapOf<String, String>()
            json.optJSONObject("uploads")?.let { u ->
                for (key in u.keys()) uploads[key] = u.getString(key)
            }
            CreatedResponse(
                CreatedReport(
                    id = json.getString("id"),
                    code = json.optString("code").ifEmpty { ReportCode.format(json.getString("id")) },
                    token = json.getString("token"),
                    expires = json.getLong("expires"),
                    partSize = json.getInt("partSize"),
                ),
                uploads,
            )
        } catch (e: org.json.JSONException) {
            throw ApiException(201, "bad_response", "The report server's answer was incomplete.", e)
        }
    }

    override fun putPart(
        reportId: String,
        name: String,
        n: Int,
        token: String,
        uploadId: String,
        file: File,
        offset: Long,
        length: Int,
        onBytes: (Int) -> Unit,
    ): String {
        val conn = open("/v1/reports/$reportId/files/$name/parts/$n", "PUT", token)
        try {
            conn.setRequestProperty("X-Upload-Id", uploadId)
            conn.setRequestProperty("Content-Type", "application/octet-stream")
            // A fixed length makes HttpURLConnection stream the body instead of buffering it, and the
            // server requires Content-Length (no chunked encoding).
            conn.setFixedLengthStreamingMode(length)
            conn.doOutput = true
            // The timeouts above cover connecting and waiting for the answer; a write that stalls
            // has none, so a guard cuts the connection if the body stops moving.
            val guard = StallGuard(conn, STALL_MS)
            try {
                try {
                    RandomAccessFile(file, "r").use { raf ->
                        raf.seek(offset)
                        conn.outputStream.use { out ->
                            val buffer = ByteArray(32 * 1024)
                            var left = length
                            while (left > 0) {
                                val read = raf.read(buffer, 0, minOf(buffer.size, left))
                                if (read < 0) throw IOException("${file.name} is shorter than it was when the report was made")
                                out.write(buffer, 0, read)
                                guard.touch()
                                left -= read
                                // May throw to stop the upload (UploadCancelled is not an IOException, so it gets through).
                                onBytes(read)
                            }
                        }
                    }
                } catch (e: IOException) {
                    // A server that rejects the part early closes the stream; its answer is still readable.
                    throw failure(conn, e)
                }
                guard.touch()
                val json = readJson(conn)
                return json.optString("etag").ifEmpty { throw ApiException(conn.responseCode, "bad_response", "The server did not return an etag for part $n.") }
            } finally {
                guard.close()
            }
        } finally {
            conn.disconnect()
        }
    }

    override fun complete(reportId: String, name: String, token: String, uploadId: String, parts: List<PartRecord>): Long {
        val body = JSONObject().put("uploadId", uploadId).put(
            "parts",
            JSONArray().also { array ->
                for (p in parts.sortedBy { it.n }) array.put(JSONObject().put("partNumber", p.n).put("etag", p.etag))
            },
        )
        return postJson("/v1/reports/$reportId/files/$name/complete", body, token).optLong("size", -1L)
    }

    override fun finish(reportId: String, token: String): FinishResponse {
        val json = postJson("/v1/reports/$reportId/finish", JSONObject(), token)
        val missing = mutableListOf<String>()
        json.optJSONArray("missing")?.let { m -> for (i in 0 until m.length()) missing += m.getString(i) }
        return FinishResponse(json.optString("code"), missing)
    }

    private fun open(path: String, method: String, token: String?): HttpURLConnection {
        val conn = try {
            URL(baseUrl + path).openConnection() as HttpURLConnection
        } catch (e: IOException) {
            throw ApiException(0, "connect", "Could not reach the report server.", e)
        }
        conn.requestMethod = method
        // Set explicitly: the defaults are "wait forever", and a stalled part has to fail to be retried.
        conn.connectTimeout = CONNECT_TIMEOUT_MS
        // A part is 8 MiB; the server answers after it has relayed it to storage.
        conn.readTimeout = READ_TIMEOUT_MS
        conn.instanceFollowRedirects = false
        conn.setRequestProperty("Accept", "application/json")
        if (token != null) conn.setRequestProperty("Authorization", "Bearer $token")
        return conn
    }

    private fun postJson(path: String, body: JSONObject, token: String?): JSONObject {
        val conn = open(path, "POST", token)
        try {
            val bytes = body.toString().toByteArray(Charsets.UTF_8)
            conn.setRequestProperty("Content-Type", "application/json")
            conn.setFixedLengthStreamingMode(bytes.size)
            conn.doOutput = true
            try {
                conn.outputStream.use { it.write(bytes) }
            } catch (e: IOException) {
                throw failure(conn, e)
            }
            return readJson(conn)
        } finally {
            conn.disconnect()
        }
    }

    /** The body of a 2xx answer as JSON; anything else becomes an [ApiException]. */
    private fun readJson(conn: HttpURLConnection): JSONObject {
        val status = try {
            conn.responseCode
        } catch (e: IOException) {
            throw ApiException(0, "connect", "The connection to the report server was lost.", e)
        }
        if (status in 200..299) {
            val text = try {
                conn.inputStream.bufferedReader().use { it.readText() }
            } catch (e: IOException) {
                throw ApiException(0, "connect", "The connection to the report server was lost.", e)
            }
            return try {
                if (text.isBlank()) JSONObject() else JSONObject(text)
            } catch (e: Exception) {
                throw ApiException(status, "bad_response", "The report server sent something unreadable.", e)
            }
        }
        throw errorFor(conn, status)
    }

    /** A write that failed: prefer the server's own explanation if it managed to send one. */
    private fun failure(conn: HttpURLConnection, cause: IOException): ApiException {
        val status = try {
            conn.responseCode
        } catch (_: IOException) {
            -1
        }
        return if (status > 0) errorFor(conn, status) else ApiException(0, "connect", "The connection to the report server was lost.", cause)
    }

    private fun errorFor(conn: HttpURLConnection, status: Int): ApiException {
        val text = try {
            (conn.errorStream ?: return ApiException(status, "", "The report server answered $status.")).bufferedReader().use { it.readText() }
        } catch (_: IOException) {
            ""
        }
        val json = try {
            JSONObject(text)
        } catch (_: Exception) {
            null
        }
        val message = json?.optString("message").orEmpty().ifEmpty { "The report server answered $status." }
        val retryAfter = conn.getHeaderField("Retry-After")?.trim()?.toIntOrNull() ?: 0
        return ApiException(status, json?.optString("error").orEmpty(), message, retryAfterSeconds = retryAfter)
    }
}
