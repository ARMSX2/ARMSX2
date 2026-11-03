package com.armsx2.bugreport

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile

/**
 * The upload rules from DESIGN.md, run against a fake server: parts in order and resumable, five
 * retries with backoff, files given up on for good, the token running out.
 */
class ReportUploaderTest {
    @get:Rule
    val tmp = TemporaryFolder()

    /** Parts of four bytes, so a few bytes make several. */
    private val partSize = 4

    private class FakeApi(private val partSize: Int) : ReportApi {
        val calls = mutableListOf<String>()
        val received = mutableMapOf<String, MutableMap<Int, ByteArray>>()
        var createError: ApiException? = null
        var finishError: ApiException? = null
        var expires = Long.MAX_VALUE / 2000
        var finishMissing: List<String> = emptyList()
        var storedSizeOverride: Map<String, Long> = emptyMap()

        /** Called before each part attempt with (file, part, attempt number from 1); throw to fail it. */
        var onPart: (String, Int, Int) -> Unit = { _, _, _ -> }
        private val attempts = mutableMapOf<Pair<String, Int>, Int>()

        override fun create(body: JSONObject): CreatedResponse {
            calls += "create"
            createError?.let { throw it }
            val uploads = mutableMapOf<String, String>()
            val files = body.getJSONArray("files")
            for (i in 0 until files.length()) uploads[files.getJSONObject(i).getString("name")] = "up-" + files.getJSONObject(i).getString("name")
            return CreatedResponse(CreatedReport("7KQ4M2PD", "AX-7KQ4-M2PD", "tok", expires, partSize), uploads)
        }

        override fun putPart(
            reportId: String, name: String, n: Int, token: String, uploadId: String,
            file: File, offset: Long, length: Int, onBytes: (Int) -> Unit,
        ): String {
            val attempt = (attempts[name to n] ?: 0) + 1
            attempts[name to n] = attempt
            calls += "part $name $n"
            onPart(name, n, attempt)
            val bytes = ByteArray(length)
            RandomAccessFile(file, "r").use { it.seek(offset); it.readFully(bytes) }
            received.getOrPut(name) { mutableMapOf() }[n] = bytes
            onBytes(length)
            return "etag-$name-$n"
        }

        override fun complete(reportId: String, name: String, token: String, uploadId: String, parts: List<PartRecord>): Long {
            calls += "complete $name ${parts.map { it.n }}"
            return storedSizeOverride[name] ?: received[name].orEmpty().values.sumOf { it.size }.toLong()
        }

        override fun finish(reportId: String, token: String): FinishResponse {
            calls += "finish"
            finishError?.let { throw it }
            return FinishResponse("AX-7KQ4-M2PD", finishMissing)
        }

        fun partCalls(name: String) = calls.filter { it.startsWith("part $name ") }
    }

    private fun bundle(vararg files: Pair<String, ByteArray>): File {
        val dir = tmp.newFolder("report-${System.nanoTime()}")
        val states = files.map { (name, bytes) ->
            File(dir, name).writeBytes(bytes)
            FileState(name, bytes.size.toLong(), Scrubber.sha256(File(dir, name)))
        }.toMutableList()
        val state = UploadState(
            localId = dir.name, kind = "graphics", createdAt = 1L,
            request = ReportJson.createRequest(ReportKind.GRAPHICS, "2.6.9", "abc", "github", JSONObject().put("description", "d")),
            files = states,
        )
        state.save(File(dir, ReportUploader.STATE_FILE))
        return dir
    }

    private fun state(dir: File) = UploadState.load(File(dir, ReportUploader.STATE_FILE))!!

    private val slept = mutableListOf<Long>()

    private fun uploader(api: ReportApi, now: Long = 1_000_000L) =
        ReportUploader(api, now = { now }, sleep = { slept += it })

    @Test
    fun aReportGoesUpPartByPartThenIsFinished() {
        val api = FakeApi(partSize)
        val dir = bundle("emulog.txt.gz" to "0123456789".toByteArray(), "report.json" to "{}".toByteArray())
        val progress = mutableListOf<Long>()
        var createdCode = ""
        val outcome = uploader(api).run(dir, object : ReportUploader.Listener {
            override fun onCreated(code: String) { createdCode = code }
            override fun onProgress(done: Long, total: Long, file: String) { progress += done }
        })

        assertEquals(ReportUploader.Outcome.Finished("AX-7KQ4-M2PD", emptyList()), outcome)
        assertEquals("AX-7KQ4-M2PD", createdCode)
        assertEquals(
            listOf(
                "create",
                "part emulog.txt.gz 1", "part emulog.txt.gz 2", "part emulog.txt.gz 3", "complete emulog.txt.gz [1, 2, 3]",
                "part report.json 1", "complete report.json [1]",
                "finish",
            ),
            api.calls,
        )
        // Every byte arrived, in the right place.
        assertEquals("0123456789", api.received["emulog.txt.gz"]!!.toSortedMap().values.joinToString("") { String(it) })
        val saved = state(dir)
        assertEquals(ReportPhase.FINISHED, saved.phase)
        assertTrue(saved.finishedAt > 0)
        assertTrue(saved.files.all { it.complete })
        assertEquals(12L, progress.max())
    }

    @Test
    fun aFailedPartIsRetriedWithBackoffAndTheReportStillFinishes() {
        val api = FakeApi(partSize)
        api.onPart = { name, n, attempt -> if (name == "a.bin" && n == 1 && attempt <= 3) throw ApiException(0, "connect", "lost") }
        val dir = bundle("a.bin" to "abcd".toByteArray())
        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf(2_000L, 4_000L, 8_000L), slept)
        assertEquals(4, api.partCalls("a.bin").size)
    }

    @Test
    fun aServerErrorIsRetriedButABadRequestIsNot() {
        val api = FakeApi(partSize)
        api.onPart = { name, _, attempt -> if (name == "a.bin" && attempt == 1) throw ApiException(503, "unavailable", "busy") }
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        api.onPart = { name, _, attempt ->
            if (name == "a.bin" && attempt == 1) throw ApiException(503, "unavailable", "busy")
            if (name == "b.bin") throw ApiException(400, "bad_part", "That part is the wrong size.")
        }
        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf(2_000L), slept) // only a.bin's 503
        assertEquals(1, api.partCalls("b.bin").size)
        val saved = state(dir)
        assertTrue(saved.files[0].complete)
        assertEquals("That part is the wrong size.", saved.files[1].failure)
        assertFalse(saved.files[1].complete)
    }

    @Test
    fun aSpentDailyBudgetWaitsAtOnceInsteadOfBackingOff() {
        val api = FakeApi(partSize)
        api.createError = ApiException(503, "budget_exhausted", "Reports are paused for today.", retryAfterSeconds = 3600)
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir)

        assertEquals(ReportUploader.Outcome.WaitingRetry(UploadReason.BUSY, "Reports are paused for today."), outcome)
        assertTrue(slept.isEmpty())
        assertEquals(1, api.calls.size)
        assertEquals(ReportPhase.WAITING_RETRY, state(dir).phase)
    }

    @Test
    fun aRateLimitIsWaitedOutForAsLongAsTheServerSays() {
        val api = FakeApi(partSize)
        var tries = 0
        api.onPart = { _, _, _ -> if (tries++ == 0) throw ApiException(429, "rate_limited", "Slow down.", retryAfterSeconds = 60) }
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf(60_000L), slept)
    }

    @Test
    fun afterFiveRetriesTheReportWaitsAndTheNextRunResumesWithoutResending() {
        val api = FakeApi(partSize)
        api.onPart = { name, n, _ -> if (name == "a.bin" && n == 2) throw ApiException(0, "connect", "no network") }
        val dir = bundle("a.bin" to "abcdefgh".toByteArray())
        val first = uploader(api).run(dir)

        assertEquals(ReportUploader.Outcome.WaitingRetry(UploadReason.NETWORK, "no network"), first)
        assertEquals(listOf(2_000L, 4_000L, 8_000L, 16_000L, 32_000L), slept)
        // Part 1 went once; part 2 was tried once and retried five times.
        assertEquals(listOf("part a.bin 1") + List(6) { "part a.bin 2" }, api.partCalls("a.bin"))
        var saved = state(dir)
        assertEquals(ReportPhase.WAITING_RETRY, saved.phase)
        assertEquals(listOf(PartRecord(1, "etag-a.bin-1")), saved.files[0].parts)
        assertNotNull(saved.created)

        // Next launch: network is back. Create is not called again; part 1 is not sent again.
        api.onPart = { _, _, _ -> }
        api.calls.clear()
        val second = uploader(api).run(dir)
        assertTrue(second is ReportUploader.Outcome.Finished)
        assertEquals(listOf("part a.bin 2", "complete a.bin [1, 2]", "finish"), api.calls)
        saved = state(dir)
        assertEquals(ReportPhase.FINISHED, saved.phase)
    }

    @Test
    fun aKilledUploadResumesFromTheStateFileAlone() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcdefghij".toByteArray())
        // As the state file would look after part 1 was acknowledged and the process died.
        val s = state(dir)
        s.created = CreatedReport("7KQ4M2PD", "AX-7KQ4-M2PD", "tok", Long.MAX_VALUE / 2000, partSize)
        s.phase = ReportPhase.UPLOADING
        s.files[0].uploadId = "up-a.bin"
        s.files[0].parts += PartRecord(1, "etag-a.bin-1")
        s.save(File(dir, ReportUploader.STATE_FILE))

        val outcome = uploader(api).run(dir)
        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf("part a.bin 2", "part a.bin 3", "complete a.bin [1, 2, 3]", "finish"), api.calls)
    }

    @Test
    fun theDevicesClockIsNotConsultedAboutTheToken() {
        // The token "expired" at second 500 by the clock the app has (1000); only the server's word counts.
        val api = FakeApi(partSize)
        api.expires = 500L
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api, now = 1_000_000L).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf("create", "part a.bin 1", "complete a.bin [1]", "finish"), api.calls)
    }

    @Test
    fun aTokenTheServerRefusesEndsTheUploadAndLeavesTheRestForANewReport() {
        val api = FakeApi(partSize)
        api.onPart = { name, _, _ -> if (name == "b.bin") throw ApiException(401, "token_expired", "The upload link has expired.") }
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray(), "c.bin" to "1234".toByteArray())

        val outcome = uploader(api).run(dir)

        assertEquals(ReportUploader.Outcome.Failed(UploadReason.TOKEN, "The upload link has expired."), outcome)
        val saved = state(dir)
        assertEquals(ReportPhase.FAILED, saved.phase)
        assertTrue(saved.tokenDead)
        assertTrue(saved.files[0].complete)
        // Nothing is marked as failed: the files are still wanted, just not under this report.
        assertTrue(saved.files.drop(1).all { it.failure == null && !it.complete })
        // No backoff, no finish with a token that is known to be dead, and c.bin was not tried.
        assertTrue(slept.isEmpty())
        assertFalse(api.calls.contains("finish"))
        assertTrue(api.partCalls("c.bin").isEmpty())
    }

    @Test
    fun retryOfADeadTokenReportMakesANewReportOfWhatIsLeft() {
        val api = FakeApi(partSize)
        api.onPart = { name, _, _ -> if (name == "b.bin") throw ApiException(401, "bad_token", "The report token is not valid.") }
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        uploader(api).run(dir)

        val retried = state(dir)
        assertTrue(retried.prepareRetry())
        retried.save(File(dir, ReportUploader.STATE_FILE))
        assertEquals(listOf("AX-7KQ4-M2PD"), retried.earlierCodes)
        assertEquals(listOf("b.bin"), retried.files.map { it.name })
        assertNull(retried.created)
        assertEquals(ReportPhase.READY, retried.phase)
        assertEquals("AX-7KQ4-M2PD", retried.request.getJSONObject("summary").getString("continues"))

        api.onPart = { _, _, _ -> }
        api.calls.clear()
        val outcome = uploader(api).run(dir)
        assertTrue(outcome is ReportUploader.Outcome.Finished)
        // A fresh create that names only the leftover file.
        assertEquals(listOf("create", "part b.bin 1", "complete b.bin [1]", "finish"), api.calls)
        assertEquals(ReportPhase.FINISHED, state(dir).phase)
    }

    @Test
    fun aTokenThatDiesAtFinishWithEverythingSentCountsAsSent() {
        val api = FakeApi(partSize)
        api.finishError = ApiException(401, "token_expired", "The upload link has expired.")
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir)

        assertEquals(ReportUploader.Outcome.Finished("AX-7KQ4-M2PD", emptyList()), outcome)
        assertEquals(ReportPhase.FINISHED, state(dir).phase)
    }

    @Test
    fun aTokenThatDiesAtFinishWithAFileStillMissingFailsAndRetryResendsThatFile() {
        val api = FakeApi(partSize)
        api.finishError = ApiException(401, "bad_token", "The report token is not valid.")
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        val hidden = File(dir, "b.bin").readBytes()
        File(dir, "b.bin").delete()

        val outcome = uploader(api).run(dir)
        assertEquals(ReportUploader.Outcome.Failed(UploadReason.TOKEN, "The report token is not valid."), outcome)

        // The storage came back. Retry sends b.bin as a new report.
        File(dir, "b.bin").writeBytes(hidden)
        val retried = state(dir)
        assertTrue(retried.prepareRetry())
        assertEquals(listOf("b.bin"), retried.files.map { it.name })
        retried.save(File(dir, ReportUploader.STATE_FILE))
        api.finishError = null
        assertTrue(uploader(api).run(dir) is ReportUploader.Outcome.Finished)
    }

    @Test
    fun aRefusalFromTheEdgeIsRetriedNotTreatedAsABadToken() {
        val api = FakeApi(partSize)
        // A 403 from the CDN, a 401 that is not the server's JSON, and an HTML page with a 400.
        val failures = listOf(
            ApiException(403, "", "The report server answered 403."),
            ApiException(401, "", "The report server answered 401."),
            ApiException(400, "", "The report server answered 400."),
        )
        var i = 0
        api.onPart = { _, _, _ -> if (i < failures.size) throw failures[i++] }
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf(2_000L, 4_000L, 8_000L), slept)
        assertFalse(state(dir).tokenDead)
    }

    @Test
    fun aMissingFileIsRecordedAsTransientAndAChangedOneAsPermanent() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        File(dir, "a.bin").delete()
        File(dir, "b.bin").appendText("extra")
        uploader(api).run(dir)

        val saved = state(dir)
        assertEquals(FailureKind.TRANSIENT, saved.files[0].failureKind)
        assertEquals(FailureKind.PERMANENT, saved.files[1].failureKind)
    }

    @Test
    fun retryGivesTransientFailuresAnotherChanceButNotPermanentOnes() {
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        val saved = state(dir)
        saved.files[0].failure = "a.bin is not there."
        saved.files[0].failureKind = FailureKind.TRANSIENT
        saved.files[1].failure = "The server refused b.bin."
        saved.files[1].failureKind = FailureKind.PERMANENT
        saved.phase = ReportPhase.FAILED

        assertTrue(saved.prepareRetry())

        assertNull(saved.files[0].failure)
        assertNotNull(saved.files[1].failure)
        assertEquals(listOf("a.bin"), saved.leftover().map { it.name })
        assertEquals(ReportPhase.READY, saved.phase)
    }

    @Test
    fun aFileFromBeforeFailureKindsWereRecordedIsTreatedAsRetryable() {
        val dir = bundle("a.bin" to "abcd".toByteArray())
        val old = state(dir)
        old.files[0].failure = "The upload token was refused."
        assertNull(old.files[0].failureKind)
        assertTrue(old.prepareRetry())
        assertNull(old.files[0].failure)
    }

    @Test
    fun stoppingMidUploadKeepsWhatWasSentFinishesTheReportAndIsMarkedStopped() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcdefgh".toByteArray(), "b.bin" to "wxyz".toByteArray())
        var parts = 0
        api.onPart = { _, _, _ -> parts++ }
        api.finishMissing = listOf("a.bin", "b.bin")

        // Part 1 is acknowledged; the stop lands while part 2 is going out.
        val outcome = uploader(api).run(dir, isCancelled = { parts >= 2 })

        assertEquals(ReportUploader.Outcome.Stopped("AX-7KQ4-M2PD", listOf("a.bin", "b.bin")), outcome)
        val saved = state(dir)
        assertEquals(ReportPhase.STOPPED, saved.phase)
        assertEquals(listOf(PartRecord(1, "etag-a.bin-1")), saved.files[0].parts)
        assertEquals("finish", api.calls.last())
        assertTrue(api.partCalls("b.bin").isEmpty())
    }

    @Test
    fun aStopAskedForBeforeAnythingWasSentSendsNothing() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir, isCancelled = { true })

        assertEquals(ReportUploader.Outcome.Stopped("", listOf("a.bin")), outcome)
        assertTrue(api.calls.isEmpty())
        assertEquals(ReportPhase.STOPPED, state(dir).phase)
    }

    @Test
    fun aStopEndsTheWaitBetweenRetries() {
        val api = FakeApi(partSize)
        api.onPart = { _, _, _ -> throw ApiException(0, "connect", "no network") }
        val dir = bundle("a.bin" to "abcd".toByteArray())
        var stop = false
        val up = ReportUploader(api, now = { 1L }, sleep = { slept += it; stop = true })

        val outcome = up.run(dir, isCancelled = { stop })

        assertTrue(outcome is ReportUploader.Outcome.Stopped)
        assertEquals(listOf(2_000L), slept)
    }

    @Test
    fun aStoppedReportCanBeRetried() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcdefgh".toByteArray())
        var parts = 0
        api.onPart = { _, _, _ -> parts++ }
        uploader(api).run(dir, isCancelled = { parts >= 2 })

        val saved = state(dir)
        assertTrue(saved.prepareRetry())
        assertEquals(ReportPhase.WAITING_RETRY, saved.phase)
        saved.save(File(dir, ReportUploader.STATE_FILE))
        api.calls.clear()
        assertTrue(uploader(api).run(dir) is ReportUploader.Outcome.Finished)
        // Part 1 stays sent; only the rest goes.
        assertEquals(listOf("part a.bin 2", "complete a.bin [1, 2]", "finish"), api.calls)
    }

    @Test
    fun aSizeMismatchGivesUpOnThatFileOnly() {
        val api = FakeApi(partSize)
        api.storedSizeOverride = mapOf("a.bin" to 3L)
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        val saved = state(dir)
        assertNotNull(saved.files[0].failure)
        assertFalse(saved.files[0].complete)
        assertTrue(saved.files[1].complete)
    }

    @Test
    fun aFileThatChangedOrVanishedOnDiskIsSkipped() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcd".toByteArray(), "b.bin" to "wxyz".toByteArray())
        File(dir, "a.bin").delete()
        File(dir, "b.bin").appendText("extra")

        val outcome = uploader(api).run(dir)

        assertTrue(outcome is ReportUploader.Outcome.Finished)
        assertEquals(listOf("create", "finish"), api.calls)
        assertTrue(state(dir).files.all { it.failure != null })
    }

    @Test
    fun noNetworkAtCreateWaitsAndALaterRunCreatesIt() {
        val api = FakeApi(partSize)
        api.createError = ApiException(0, "connect", "Could not reach the report server.")
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val first = uploader(api).run(dir)
        assertEquals(ReportUploader.Outcome.WaitingRetry(UploadReason.NETWORK, "Could not reach the report server."), first)
        assertNull(state(dir).created)
        assertEquals(ReportPhase.WAITING_RETRY, state(dir).phase)
        assertEquals(6, api.calls.count { it == "create" })

        api.createError = null
        val second = uploader(api).run(dir)
        assertTrue(second is ReportUploader.Outcome.Finished)
    }

    @Test
    fun aRefusedCreateFailsTheReport() {
        val api = FakeApi(partSize)
        api.createError = ApiException(400, "too_big", "That report is over the size limit.")
        val dir = bundle("a.bin" to "abcd".toByteArray())

        val outcome = uploader(api).run(dir)
        assertEquals(ReportUploader.Outcome.Failed(UploadReason.REFUSED, "That report is over the size limit."), outcome)
        assertEquals(1, api.calls.size)
        assertEquals(ReportPhase.FAILED, state(dir).phase)
    }

    @Test
    fun aFinishedReportIsNotSentAgain() {
        val api = FakeApi(partSize)
        val dir = bundle("a.bin" to "abcd".toByteArray())
        uploader(api).run(dir)
        api.calls.clear()

        val again = uploader(api).run(dir)
        assertTrue(again is ReportUploader.Outcome.Finished)
        assertTrue(api.calls.isEmpty())
    }

    @Test
    fun aMissingStateFileIsAFailureNotACrash() {
        val dir = tmp.newFolder("empty")
        assertTrue(uploader(FakeApi(partSize)).run(dir) is ReportUploader.Outcome.Failed)
    }
}
