// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * The lobby service: one six-character code instead of two long pasted ones. The host leaves its
 * offer under a code and polls for the answer; the friend types the code, reads the offer and
 * leaves an answer. Only those two messages pass through it, for at most ten minutes, and the
 * game stream never does.
 *
 * The service is a Cloudflare Worker shared with ARMSX3. This app's lobbies live under
 * /v1/armsx2, so a code from one app can never be joined from the other.
 *
 * Every call blocks: use them off the main thread.
 */
internal object RemotePlayLobby {
    private const val BASE = "https://remote-play.armsx2mail.workers.dev/v1/armsx2/lobbies"

    // As the service draws them: no 0/O, 1/I/L, since a code is read aloud and typed on a pad.
    private const val ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
    private const val CODE_LENGTH = 6

    /** How long the service keeps a lobby; the host stops waiting at the same time. */
    const val LIFETIME_MS = 10 * 60_000L

    class Lobby(val code: String, val token: String)

    sealed interface Result {
        data class Ok(val value: String) : Result
        /** The answer is not there yet. */
        data object Waiting : Result
        /** No such code, or it expired. */
        data object NotFound : Result
        /** Someone already joined. */
        data object Busy : Result
        data object Unreachable : Result
    }

    /** "k7m-2qx" -> "K7M2QX"; null when [text] is not a lobby code. */
    fun normalize(text: String): String? {
        val code = text.uppercase().filterNot { it.isWhitespace() || it == '-' }
        return code.takeIf { it.length == CODE_LENGTH && it.all { char -> char in ALPHABET } }
    }

    /** "K7M2QX" -> "K7M-2QX", easier to read out. */
    fun display(code: String): String = code.substring(0, 3) + "-" + code.substring(3)

    /** The host opens a lobby with its offer; null when the service cannot be reached. */
    fun open(offer: String): Lobby? {
        val (status, body) = call("POST", "", JSONObject().put("offer", offer))
        if (status != 201 || body == null) return null
        val code = body.optString("code")
        val token = body.optString("token")
        return if (normalize(code) != null && token.isNotEmpty()) Lobby(code, token) else null
    }

    fun offer(code: String): Result = call("GET", "/$code").toResult("offer")

    fun answer(code: String, answer: String): Result =
        when (call("POST", "/$code/answer", JSONObject().put("answer", answer)).first) {
            204 -> Result.Ok("")
            404 -> Result.NotFound
            409 -> Result.Busy
            else -> Result.Unreachable
        }

    /** The friend's answer once it is there; the service deletes the lobby as it hands it over. */
    fun collectAnswer(lobby: Lobby): Result =
        call("GET", "/${lobby.code}/answer", token = lobby.token).toResult("answer")

    fun close(lobby: Lobby) {
        call("DELETE", "/${lobby.code}", token = lobby.token)
    }

    private fun Pair<Int, JSONObject?>.toResult(field: String): Result = when (first) {
        200 -> second?.optString(field)?.takeIf { it.isNotEmpty() }?.let { Result.Ok(it) } ?: Result.Unreachable
        204 -> Result.Waiting
        404 -> Result.NotFound
        409 -> Result.Busy
        else -> Result.Unreachable
    }

    /** The HTTP status (0 when the service could not be reached) and the JSON body, if any. */
    private fun call(method: String, path: String, body: JSONObject? = null, token: String? = null): Pair<Int, JSONObject?> {
        val connection = try {
            URL(BASE + path).openConnection() as HttpURLConnection
        } catch (_: IOException) {
            return 0 to null
        }
        return try {
            connection.requestMethod = method
            connection.connectTimeout = 8_000
            connection.readTimeout = 10_000
            connection.setRequestProperty("Accept", "application/json")
            if (token != null) connection.setRequestProperty("Authorization", "Bearer $token")
            if (body != null) {
                connection.doOutput = true
                connection.setRequestProperty("Content-Type", "application/json")
                connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            }
            val status = connection.responseCode
            val stream = if (status >= 400) connection.errorStream else connection.inputStream
            val text = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            status to text.takeIf { it.isNotBlank() }?.let { runCatching { JSONObject(it) }.getOrNull() }
        } catch (_: IOException) {
            0 to null
        } finally {
            connection.disconnect()
        }
    }
}
