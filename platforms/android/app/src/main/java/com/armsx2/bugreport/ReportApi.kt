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
 * @param code the server's machine-readable error, or "" if it sent none.
 */
class ApiException(
    val status: Int,
    val code: String,
    message: String,
    cause: Throwable? = null,
    /** The server's `Retry-After`, in seconds, or 0 if it sent none. */
    val retryAfterSeconds: Int = 0,
) : IOException(message, cause) {
    /** A failure that may pass on its own: no connection, a server error, or being rate limited. */
    val retryable: Boolean get() = status == 0 || status >= 500 || status == 408 || status == 429

    /** The token was refused (`bad_token`, `token_expired`), which a retry with the same token cannot fix. */
    val tokenRejected: Boolean get() = status == 401 || status == 403

    /**
     * True if trying again within the next minute or two is worth it. The daily byte budget is spent
     * for hours (`budget_exhausted`, Retry-After 3600), so waiting out a few seconds of backoff
     * only delays the answer; that failure goes straight to "waiting to retry".
     */
    val worthBackoff: Boolean get() = retryable && code != "budget_exhausted" && retryAfterSeconds <= MAX_BACKOFF_WAIT_SECONDS

    companion object {
        /** A Retry-After longer than this is not slept through. The rate limiter asks for 60. */
        const val MAX_BACKOFF_WAIT_SECONDS = 90
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

/** [ReportApi] over HttpURLConnection, with every part streamed from disk at a fixed length. */
class HttpReportApi(private val baseUrl: String = REPORT_SERVER) : ReportApi {

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
                            left -= read
                            onBytes(read)
                        }
                    }
                }
            } catch (e: IOException) {
                // A server that rejects the part early closes the stream; its answer is still readable.
                throw failure(conn, e)
            }
            val json = readJson(conn)
            return json.optString("etag").ifEmpty { throw ApiException(conn.responseCode, "bad_response", "The server did not return an etag for part $n.") }
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
        conn.connectTimeout = 15_000
        // A part is 8 MiB; the server answers after it has relayed it to storage.
        conn.readTimeout = 90_000
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
