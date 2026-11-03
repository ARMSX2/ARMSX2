// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File
import java.io.IOException

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
 *  - a file is never tried again once the server has refused it for good (a 4xx that is not "try
 *    later", or a stored size that does not match);
 *  - once the token has expired the remaining files are given up on and `finish` is called if the
 *    token still works; if it does not, the report is failed.
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

        /** Stopped on something that may pass; call [run] again later. */
        data class WaitingRetry(val reason: String) : Outcome

        /** Nothing more can be done for this report. */
        data class Failed(val reason: String) : Outcome
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

    /** Runs the upload in [dir] until it finishes, fails or has to wait. [isCancelled] is polled between steps. */
    fun run(dir: File, listener: Listener = object : Listener {}, isCancelled: () -> Boolean = { false }): Outcome {
        val stateFile = File(dir, STATE_FILE)
        val state = UploadState.load(stateFile) ?: return Outcome.Failed("The report's state file is missing or unreadable.")
        if (state.phase == ReportPhase.FINISHED) {
            return Outcome.Finished(state.created?.code.orEmpty(), state.missing)
        }

        fun save(phase: ReportPhase? = null, error: String? = null) {
            if (phase != null) state.phase = phase
            state.lastError = error
            state.save(stateFile)
            listener.onState(state)
        }

        // 1. Make sure the server knows about the report.
        if (state.created == null) {
            val created = try {
                withRetries(isCancelled) { api.create(state.createBody()) }
            } catch (e: ApiException) {
                return if (e.retryable) {
                    save(ReportPhase.WAITING_RETRY, e.message)
                    Outcome.WaitingRetry(e.message.orEmpty())
                } else {
                    save(ReportPhase.FAILED, e.message)
                    Outcome.Failed(e.message.orEmpty())
                }
            } catch (_: CancelledException) {
                return Outcome.WaitingRetry("Cancelled")
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
        var tokenGood = now() / 1000 <= created.expires
        if (!tokenGood) {
            for (f in state.files) if (!f.settled) f.failure = "The upload token expired before this file was sent."
        }
        var waiting: String? = null
        val total = state.totalBytes
        for (file in state.files) {
            if (file.settled || !tokenGood || waiting != null) continue
            if (isCancelled()) return Outcome.WaitingRetry("Cancelled")
            val source = File(dir, file.name)
            if (!source.isFile || source.length() != file.size) {
                file.failure = "${file.name} is missing or changed on disk."
                save()
                continue
            }
            val uploadId = file.uploadId
            if (uploadId == null) {
                file.failure = "The server gave no upload for ${file.name}."
                save()
                continue
            }
            try {
                sendFile(dir, state, file, created, uploadId, total, listener, isCancelled) { save() }
            } catch (e: ApiException) {
                when {
                    e.tokenRejected -> {
                        tokenGood = false
                        for (f in state.files) if (!f.settled) f.failure = "The upload token was refused."
                        save()
                    }
                    e.retryable -> waiting = e.message ?: "Could not reach the report server."
                    else -> {
                        file.failure = e.message ?: "The server refused ${file.name}."
                        save()
                    }
                }
            } catch (_: CancelledException) {
                return Outcome.WaitingRetry("Cancelled")
            }
        }
        if (waiting != null) {
            save(ReportPhase.WAITING_RETRY, waiting)
            return Outcome.WaitingRetry(waiting)
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
                e.tokenRejected || !e.retryable -> {
                    save(ReportPhase.FAILED, e.message)
                    Outcome.Failed(e.message.orEmpty())
                }
                else -> {
                    save(ReportPhase.WAITING_RETRY, e.message)
                    Outcome.WaitingRetry(e.message.orEmpty())
                }
            }
        } catch (_: CancelledException) {
            Outcome.WaitingRetry("Cancelled")
        }
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
            if (isCancelled()) throw CancelledException()
            val length = PartMath.length(file.size, n, partSize)
            val base = allDone()
            listener.onProgress(base, total, file.name)
            val etag = withRetries(isCancelled) {
                api.putPart(created.id, file.name, n, created.token, uploadId, source, PartMath.offset(n, partSize), length) { sent ->
                    listener.onProgress(minOf(total, base + sent), total, file.name)
                }
            }
            file.parts.removeAll { it.n == n }
            file.parts += PartRecord(n, etag)
            save()
            listener.onProgress(allDone(), total, file.name)
        }
        val stored = withRetries(isCancelled) { api.complete(created.id, file.name, created.token, uploadId, file.parts) }
        if (stored >= 0 && stored != file.size) {
            throw ApiException(422, "size_mismatch", "The server stored ${file.name} at the wrong size.")
        }
        file.complete = true
        save()
    }

    /**
     * Runs [call], retrying a failure that may pass after each of [retryDelaysMs]. A failure that
     * cannot pass, or the last of the retries failing, is thrown.
     */
    private fun <T> withRetries(isCancelled: () -> Boolean, call: () -> T): T {
        var attempt = 0
        while (true) {
            try {
                return call()
            } catch (e: ApiException) {
                if (!e.worthBackoff || e.tokenRejected || attempt >= retryDelaysMs.size) throw e
                if (isCancelled()) throw CancelledException()
                // A server that says how long to wait (the rate limiter: 60 s) is believed over the schedule.
                sleep(maxOf(retryDelaysMs[attempt], e.retryAfterSeconds * 1000L))
                attempt++
            }
        }
    }

    private class CancelledException : IOException("cancelled")

    companion object {
        const val STATE_FILE = "state.json"
    }
}
