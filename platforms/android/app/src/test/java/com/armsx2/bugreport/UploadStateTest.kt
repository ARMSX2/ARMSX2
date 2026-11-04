package com.armsx2.bugreport

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** How files are cut into parts, and what is remembered between launches. */
class UploadStateTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val eight = 8 * 1024 * 1024

    @Test
    fun partCountRoundsUpAndNeverReturnsZero() {
        assertEquals(1, PartMath.count(1, eight))
        assertEquals(1, PartMath.count(eight.toLong(), eight))
        assertEquals(2, PartMath.count(eight + 1L, eight))
        assertEquals(6, PartMath.count(48L * 1024 * 1024, eight))
        assertEquals(1, PartMath.count(0, eight))
    }

    @Test
    fun everyPartIsFullSizeExceptTheLast() {
        val size = eight * 2L + 1000
        assertEquals(eight, PartMath.length(size, 1, eight))
        assertEquals(eight, PartMath.length(size, 2, eight))
        assertEquals(1000, PartMath.length(size, 3, eight))
        assertEquals(0L, PartMath.offset(1, eight))
        assertEquals(eight * 2L, PartMath.offset(3, eight))
    }

    @Test
    fun anExactMultipleHasNoEmptyTrailingPart() {
        val size = eight * 3L
        assertEquals(3, PartMath.count(size, eight))
        assertEquals(eight, PartMath.length(size, 3, eight))
    }

    @Test
    fun partLengthsAddUpToTheFile() {
        for (size in listOf(1L, 7L, eight - 1L, eight.toLong(), eight + 1L, eight * 5L + 123)) {
            val total = (1..PartMath.count(size, eight)).sumOf { PartMath.length(size, it, eight).toLong() }
            assertEquals("size $size", size, total)
        }
    }

    @Test
    fun nextPartIsTheLowestOneNotAcknowledged() {
        val f = FileState("a.bin", eight * 3L + 5, "00")
        assertEquals(1, f.nextPart(eight))
        f.parts += PartRecord(1, "e1")
        f.parts += PartRecord(3, "e3")
        assertEquals(2, f.nextPart(eight))
        f.parts += PartRecord(2, "e2")
        assertEquals(4, f.nextPart(eight))
        f.parts += PartRecord(4, "e4")
        assertNull(f.nextPart(eight))
        assertEquals(eight * 3L + 5, f.bytesDone(eight))
    }

    @Test
    fun bytesDoneCountsTheShortLastPart() {
        val f = FileState("a.bin", eight + 10L, "00")
        f.parts += PartRecord(2, "e2")
        assertEquals(10L, f.bytesDone(eight))
        f.parts += PartRecord(1, "e1")
        assertEquals(eight + 10L, f.bytesDone(eight))
    }

    private fun sample(): UploadState {
        val request = ReportJson.createRequest(
            ReportKind.GRAPHICS, "2.6.9", "05f55b8e19", "github",
            ReportJson.summary("it flickers", ReportGame("SCUS-97265", "ABCD1234", "R&C", "SCUS-97265"), "Pocket 5", "SM8250", "Adreno 650", "turnip axfl3-1", "Android 14 (SDK 34)"),
        )
        return UploadState(
            localId = "report-1", kind = "graphics", createdAt = 1000L, request = request,
            files = mutableListOf(FileState("emulog.txt.gz", 100, "aa"), FileState("gsdump.gs.zip", eight + 7L, "bb")),
        )
    }

    @Test
    fun stateSurvivesAWriteAndReadWithEverythingTheUploaderNeeds() {
        val state = sample()
        state.created = CreatedReport("7KQ4M2PD", "AX-7KQ4-M2PD", "tok", 1_762_300_000L, eight)
        state.phase = ReportPhase.UPLOADING
        state.files[0].uploadId = "u1"
        state.files[0].complete = true
        state.files[1].uploadId = "u2"
        state.files[1].parts += PartRecord(1, "etag-1")
        state.files[1].failure = null
        state.lastError = "boom"
        state.missing = listOf("savestate.p2s")

        val file = tmp.newFile("state.json")
        state.save(file)
        val back = UploadState.load(file)!!

        assertEquals("report-1", back.localId)
        assertEquals(ReportPhase.UPLOADING, back.phase)
        assertEquals(CreatedReport("7KQ4M2PD", "AX-7KQ4-M2PD", "tok", 1_762_300_000L, eight), back.created)
        assertTrue(back.files[0].complete)
        assertEquals("u1", back.files[0].uploadId)
        assertEquals(listOf(PartRecord(1, "etag-1")), back.files[1].parts)
        assertEquals(2, back.files.size)
        assertEquals("boom", back.lastError)
        assertEquals(listOf("savestate.p2s"), back.missing)
        assertEquals(state.request.toString(), back.request.toString())
    }

    @Test
    fun aFileFailedForGoodStaysFailedAfterReload() {
        val state = sample()
        state.files[0].failure = "size_mismatch"
        val file = tmp.newFile("failed.json")
        state.save(file)
        val back = UploadState.load(file)!!
        assertEquals("size_mismatch", back.files[0].failure)
        assertTrue(back.files[0].settled)
        assertFalse(back.files[1].settled)
        assertNull(back.files[1].uploadId)
    }

    @Test
    fun theCreateBodyIsTheRequestPlusTheFileList() {
        val body = sample().createBody()
        assertEquals(1, body.getInt("schema"))
        assertEquals("graphics", body.getString("kind"))
        assertEquals("android", body.getString("platform"))
        assertEquals("github", body.getJSONObject("app").getString("flavor"))
        val files = body.getJSONArray("files")
        assertEquals(2, files.length())
        assertEquals("gsdump.gs.zip", files.getJSONObject(1).getString("name"))
        assertEquals(eight + 7L, files.getJSONObject(1).getLong("size"))
        assertEquals("bb", files.getJSONObject(1).getString("sha256"))
        // The stored request must not have been changed by building the body.
        assertFalse(sample().request.has("files"))
    }

    @Test
    fun anUnreadableStateFileIsNull() {
        val file = tmp.newFile("bad.json")
        file.writeText("{not json")
        assertNull(UploadState.load(file))
        assertNull(UploadState.load(tmp.root.resolve("missing.json")))
    }

    @Test
    fun anUnknownPhaseReadsAsReady() {
        assertEquals(ReportPhase.READY, ReportPhase.fromWire("nonsense"))
        assertEquals(ReportPhase.WAITING_RETRY, ReportPhase.fromWire("waiting-retry"))
        assertEquals(ReportPhase.EVIDENCE, ReportPhase.fromWire(JSONObject("""{"p":"evidence"}""").getString("p")))
    }

    @Test
    fun theDeadTokenFlagFailureKindsAndEarlierCodesSurviveTheStateFile() {
        val state = UploadState(
            localId = "report-1", kind = "crash", createdAt = 5,
            request = JSONObject().put("summary", JSONObject()),
            files = mutableListOf(
                FileState("a.bin", 10, "aa", failure = "gone", failureKind = FailureKind.TRANSIENT),
                FileState("b.bin", 10, "bb", failure = "refused", failureKind = FailureKind.PERMANENT),
                FileState("c.bin", 10, "cc"),
            ),
            phase = ReportPhase.FAILED,
            lastReason = UploadReason.TOKEN.key,
            tokenDead = true,
            earlierCodes = listOf("AX-AAAA-BBBB"),
        )
        val file = tmp.newFile("state.json")
        state.save(file)
        val back = UploadState.load(file)!!

        assertTrue(back.tokenDead)
        assertEquals(listOf("AX-AAAA-BBBB"), back.earlierCodes)
        assertEquals(UploadReason.TOKEN.key, back.lastReason)
        assertEquals(FailureKind.TRANSIENT, back.files[0].failureKind)
        assertEquals(FailureKind.PERMANENT, back.files[1].failureKind)
        assertNull(back.files[2].failureKind)
        assertEquals(ReportPhase.FAILED, back.phase)
    }

    @Test
    fun anOldStateFileWithNoneOfThatLoadsWithSafeDefaults() {
        val old = JSONObject("""{"schema":1,"localId":"r","kind":"other","createdAt":1,"phase":"waiting-retry","request":{},"files":[{"name":"a.bin","size":3,"sha256":"x","failure":"The upload token was refused."}]}""")
        val back = UploadState.fromJson(old)
        assertFalse(back.tokenDead)
        assertEquals(emptyList<String>(), back.earlierCodes)
        assertNull(back.files[0].failureKind)
        assertEquals("The upload token was refused.", back.files[0].failure)
    }

    @Test
    fun bytesLeftCountsWhatIsNotSentAndNotRefusedForGood() {
        val partSize = 4
        val state = UploadState(
            "r", "other", 1, JSONObject(),
            mutableListOf(
                FileState("a.bin", 8, "aa", complete = true),
                FileState("b.bin", 8, "bb", parts = mutableListOf(PartRecord(1, "e"))),
                FileState("c.bin", 100, "cc", failure = "no", failureKind = FailureKind.PERMANENT),
                FileState("d.bin", 6, "dd", failure = "gone", failureKind = FailureKind.TRANSIENT),
            ),
        )
        // Before the report exists nothing has been sent: the whole of b.bin and d.bin.
        assertEquals(8L + 6L, state.bytesLeft())
        state.created = CreatedReport("ID", "AX-1111-2222", "t", 1, partSize)
        // b.bin has sent one 4-byte part of two; d.bin is all still to go.
        assertEquals(4L + 6L, state.bytesLeft())
    }
}
