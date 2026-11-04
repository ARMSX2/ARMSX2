// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File

/**
 * Sends a prepared report folder to the server and can pick up where an earlier attempt stopped.
 *
 * The folder holds the report's files and `state.json` ([UploadState]). Everything the server has
 * acknowledged is in that file: the create response, and for each file the parts it took. After
 * every part the file is rewritten, so a kill at any point costs at most the part in flight.
 *
 * The rules (DESIGN.md, "Client upload rules"):
 *  - a part that fails is retried up to five times, after 2, 4, 8, 16 and 32 seconds; after that the
 *    report waits for the next launch or for Retry ([Outcome.WaitingRetry]);
 *  - a failure that may pass is one the server did not refuse: no connection, a 5xx, 408, 429, a 403
 *    or an error that is not the server's own JSON (the CDN or a captive portal). Only a 401 with
 *    `bad_token` or `token_expired` says the token is dead; the device's clock is never consulted;
 *  - a file is never tried again once the server has refused it for good (a 4xx that is not "try
 *    later", or a stored size that does not match), unless its failure was the kind that can pass
 *    ([FailureKind.TRANSIENT]) and Retry reset it;
 *  - a dead token ends the upload of the report: what arrived stays under its code, and Retry
 *    makes a new report from the rest ([UploadState.prepareRetry]);
 *  - the user can stop it: whatever the server has is kept and `finish` is called if it will take it.
 *
 * Blocking: run it on a background thread. [sleep] and [now] are parameters so tests do not wait.
 */
class ReportUploader(
    private val api: ReportApi,
    private val now: () -> Long = { System.currentTimeMillis() },
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    private val retryDelaysMs: List<Long> = listOf(2_000, 4_000, 8_000, 16_000, 32_000),
) {
    sealed interface Outcome {
        /** `finish` succeeded. [missing] lists files that were declared but did not arrive. */
        data class Finished(val code: String, val missing: List<String>) : Outcome

        /** Stopped on something that may pass; call [run] again later. [detail] is for logs. */
        data class WaitingRetry(val reason: UploadReason, val detail: String = "") : Outcome

        /** Nothing more can be done for this report as it is. [detail] is for logs. */
        data class Failed(val reason: UploadReason, val detail: String = "") : Outcome

        /** The user stopped it. [code] is empty if the server never created the report. */
        data class Stopped(val code: String, val missing: List<String>) : Outcome
    }

    /** Told as the upload moves, from the uploading thread. */
    interface Listener {
        /** The server created the report and gave it a code. Called once per report. */
        fun onCreated(code: String) {}

        /** [done] of [total] bytes are acknowledged or in flight; [file] is the one going up. */
        fun onProgress(done: Long, total: Long, file: String) {}

        /** The state changed and was saved. */
        fun onState(state: UploadState) {}
    }

    /** Runs the upload in [dir] until it finishes, fails, has to wait or is stopped. [isCancelled] is polled between steps and while bytes move. */
    fun run(dir: File, listener: Listener = object : Listener {}, isCancelled: () -> Boolean = { false }): Outcome {
        val stateFile = File(dir, STATE_FILE)
        val state = UploadState.load(stateFile)
            ?: return Outcome.Failed(UploadReason.LOCAL, "The report's state file is missing or unreadable.")
        if (state.phase == ReportPhase.FINISHED) {
            return Outcome.Finished(state.created?.code.orEmpty(), state.missing)
        }
        if (state.tokenDead) return Outcome.Failed(UploadReason.TOKEN, "The upload link ended; retry to send the rest in a new report.")

        fun save(phase: ReportPhase? = null, reason: UploadReason? = null, detail: String? = null) {
            if (phase != null) state.phase = phase
            state.lastReason = reason?.key
            state.lastError = detail
            state.save(stateFile)
            listener.onState(state)
        }

        try {
            // A stop asked for while the report was not running (a waiting report) is honoured before anything is sent.
            if (isCancelled()) throw UploadCancelled()
            // 1. Make sure the server knows about the report.
            if (state.created == null) {
                val created = try {
                    withRetries(isCancelled) { api.create(state.createBody()) }
                } catch (e: ApiException) {
                    return if (e.retryable) {
                        save(ReportPhase.WAITING_RETRY, e.reason, e.message)
                        Outcome.WaitingRetry(e.reason, e.message.orEmpty())
                    } else {
                        save(ReportPhase.FAILED, e.reason, e.message)
                        Outcome.Failed(e.reason, e.message.orEmpty())
                    }
                }
                state.created = created.created
                for (f in state.files) f.uploadId = created.uploads[f.name]
                save(ReportPhase.UPLOADING)
            } else if (state.phase != ReportPhase.UPLOADING) {
                save(ReportPhase.UPLOADING)
            }
            val created = state.created!!
            listener.onCreated(created.code)

            // 2. Send what is left.
            var waiting: ApiException? = null
            val total = state.totalBytes
            for (file in state.files) {
                if (file.settled || state.tokenDead || waiting != null) continue
                if (isCancelled()) throw UploadCancelled()
                val source = File(dir, file.name)
                if (!source.isFile) {
                    // The storage may be unmounted for a while; Retry tries again.
                    file.failure = "${file.name} is not there."
                    file.failureKind = FailureKind.TRANSIENT
                    save()
                    continue
                }
                if (source.length() != file.size) {
                    file.failure = "${file.name} changed on disk."
                    file.failureKind = FailureKind.PERMANENT
                    save()
                    continue
                }
                val uploadId = file.uploadId
                if (uploadId == null) {
                    file.failure = "The server gave no upload for ${file.name}."
                    file.failureKind = FailureKind.PERMANENT
                    save()
                    continue
                }
                try {
                    sendFile(dir, state, file, created, uploadId, total, listener, isCancelled) { save() }
                } catch (e: ApiException) {
                    when {
                        e.tokenRejected -> {
                            state.tokenDead = true
                            save(reason = UploadReason.TOKEN, detail = e.message)
                        }
                        e.retryable -> waiting = e
                        else -> {
                            file.failure = e.message ?: "The server refused ${file.name}."
                            file.failureKind = FailureKind.PERMANENT
                            save()
                        }
                    }
                }
            }
            if (state.tokenDead) return tokenEnded(state, created, ::save)
            waiting?.let { w ->
                save(ReportPhase.WAITING_RETRY, w.reason, w.message)
                return Outcome.WaitingRetry(w.reason, w.message.orEmpty())
            }

            // 3. Close the report, with whatever arrived.
            return try {
                val finish = withRetries(isCancelled) { api.finish(created.id, created.token) }
                state.finishedAt = now()
                state.missing = finish.missing
                save(ReportPhase.FINISHED)
                Outcome.Finished(finish.code.ifEmpty { created.code }, finish.missing)
            } catch (e: ApiException) {
                when {
                    e.tokenRejected -> {
                        state.tokenDead = true
                        save(reason = UploadReason.TOKEN, detail = e.message)
                        tokenEnded(state, created, ::save)
                    }
                    e.retryable -> {
                        save(ReportPhase.WAITING_RETRY, e.reason, e.message)
                        Outcome.WaitingRetry(e.reason, e.message.orEmpty())
                    }
                    else -> {
                        save(ReportPhase.FAILED, e.reason, e.message)
                        Outcome.Failed(e.reason, e.message.orEmpty())
                    }
                }
            }
        } catch (_: UploadCancelled) {
            return stopped(state, ::save)
        }
    }

    /**
     * The server said the upload link is dead. If everything that could be sent has been, the report
     * is as complete as it will get: it is finished here, without the server's confirmation. If not,
     * it fails, and Retry sends the rest as a new report.
     */
    private fun tokenEnded(state: UploadState, created: CreatedReport, save: (ReportPhase?, UploadReason?, String?) -> Unit): Outcome {
        if (state.leftover().isEmpty()) {
            state.finishedAt = now()
            state.missing = state.files.filter { !it.complete }.map { it.name }
            save(ReportPhase.FINISHED, null, "The upload link ended after every file that could be sent had arrived.")
            return Outcome.Finished(created.code, state.missing)
        }
        save(ReportPhase.FAILED, UploadReason.TOKEN, state.lastError)
        return Outcome.Failed(UploadReason.TOKEN, state.lastError.orEmpty())
    }

    /** The user stopped the upload: tell the server the report is closed, if it will listen, and keep the state. */
    private fun stopped(state: UploadState, save: (ReportPhase?, UploadReason?, String?) -> Unit): Outcome {
        val created = state.created
        var code = created?.code.orEmpty()
        var missing = state.files.filter { !it.complete }.map { it.name }
        if (created != null && !state.tokenDead) {
            // One try, no backoff: a stop should not take long.
            try {
                val finish = api.finish(created.id, created.token)
                code = finish.code.ifEmpty { code }
                missing = finish.missing
            } catch (_: ApiException) {
            }
        }
        state.missing = missing
        save(ReportPhase.STOPPED, UploadReason.STOPPED, null)
        return Outcome.Stopped(code, missing)
    }

    private fun sendFile(
        dir: File,
        state: UploadState,
        file: FileState,
        created: CreatedReport,
        uploadId: String,
        total: Long,
        listener: Listener,
        isCancelled: () -> Boolean,
        save: () -> Unit,
    ) {
        val source = File(dir, file.name)
        val partSize = created.partSize
        val allDone = { state.bytesDone() }
        while (true) {
            val n = file.nextPart(partSize) ?: break
            if (isCancelled()) throw UploadCancelled()
            val length = PartMath.length(file.size, n, partSize)
            val base = allDone()
            listener.onProgress(base, total, file.name)
            val etag = withRetries(isCancelled) {
                api.putPart(created.id, file.name, n, created.token, uploadId, source, PartMath.offset(n, partSize), length) { sent ->
                    if (isCancelled()) throw UploadCancelled()
                    listener.onProgress(minOf(total, base + sent), total, file.name)
                }
            }
            file.parts.removeAll { it.n == n }
            file.parts += PartRecord(n, etag)
            save()
            listener.onProgress(allDone(), total, file.name)
        }
        if (isCancelled()) throw UploadCancelled()
        val stored = withRetries(isCancelled) { api.complete(created.id, file.name, created.token, uploadId, file.parts) }
        if (stored >= 0 && stored != file.size) {
            throw ApiException(422, "size_mismatch", "The server stored ${file.name} at the wrong size.")
        }
        file.complete = true
        save()
    }

    /**
     * Runs [call], retrying a failure that may pass after each of [retryDelaysMs]. A failure that
     * cannot pass, or the last of the retries failing, is thrown. A stop request ends the waiting.
     */
    private fun <T> withRetries(isCancelled: () -> Boolean, call: () -> T): T {
        var attempt = 0
        while (true) {
            try {
                return call()
            } catch (e: ApiException) {
                if (!e.worthBackoff || attempt >= retryDelaysMs.size) throw e
                if (isCancelled()) throw UploadCancelled()
                // A server that says how long to wait (the rate limiter: 60 s) is believed over the schedule.
                sleep(maxOf(retryDelaysMs[attempt], e.retryAfterSeconds * 1000L))
                attempt++
            }
        }
    }

    companion object {
        const val STATE_FILE = "state.json"
    }
}
