package com.armsx2.bugreport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Which failures mean "the token is dead", which "try again", which "no". */
class ApiExceptionTest {
    private fun e(status: Int, code: String = "", retryAfter: Int = 0) = ApiException(status, code, "m", retryAfterSeconds = retryAfter)

    @Test
    fun onlyA401WithTheServersTokenCodesIsATokenFailure() {
        assertTrue(e(401, "bad_token").tokenRejected)
        assertTrue(e(401, "token_expired").tokenRejected)
        // The same words on another status, or another code on 401, are not the server saying it.
        assertFalse(e(403, "bad_token").tokenRejected)
        assertFalse(e(401, "unauthorized").tokenRejected)
        assertFalse(e(401, "").tokenRejected)
        assertFalse(e(403, "").tokenRejected)
        assertFalse(e(0, "connect").tokenRejected)
    }

    @Test
    fun aDeadTokenIsNeverRetried() {
        assertFalse(e(401, "bad_token").retryable)
        assertFalse(e(401, "token_expired").worthBackoff)
        assertEquals(UploadReason.TOKEN, e(401, "token_expired").reason)
    }

    @Test
    fun whatTheEdgeSaysIsRetryable() {
        assertTrue(e(403).retryable)
        assertTrue(e(403, "forbidden").retryable)
        assertTrue(e(401).retryable) // not the server's JSON
        assertTrue(e(400).retryable)
        assertTrue(e(404).retryable)
        assertTrue(e(200, "bad_response").retryable) // a captive portal's page
        assertTrue(e(201, "bad_response").retryable)
    }

    @Test
    fun theServersOwnRefusalsAreNot() {
        for (code in listOf("bad_request", "bad_part_size", "bad_upload_id", "size_mismatch", "report_too_large", "unsupported_schema")) {
            assertFalse(code, e(400, code).retryable)
        }
        assertFalse(e(404, "report_not_found").retryable)
        assertFalse(e(409, "complete_failed").retryable)
        assertFalse(e(413, "file_too_large").retryable)
        assertFalse(e(422, "size_mismatch").retryable)
        assertEquals(UploadReason.REFUSED, e(422, "size_mismatch").reason)
    }

    @Test
    fun serverTroubleAndLimitsAreRetryable() {
        assertTrue(e(0, "connect").retryable)
        assertTrue(e(500, "internal").retryable)
        assertTrue(e(502, "storage_error").retryable)
        assertTrue(e(408).retryable)
        assertTrue(e(429, "rate_limited", 60).retryable)
        assertEquals(UploadReason.NETWORK, e(0, "connect").reason)
        assertEquals(UploadReason.SERVER, e(502, "storage_error").reason)
        assertEquals(UploadReason.BUSY, e(429, "rate_limited").reason)
    }

    @Test
    fun aSpentBudgetOrALongRetryAfterIsNotSleptThrough() {
        assertTrue(e(503, "budget_exhausted", 3600).retryable)
        assertFalse(e(503, "budget_exhausted", 3600).worthBackoff)
        assertEquals(UploadReason.BUSY, e(503, "budget_exhausted", 3600).reason)
        assertTrue(e(429, "rate_limited", 60).worthBackoff)
        assertFalse(e(429, "rate_limited", 600).worthBackoff)
    }

    @Test
    fun theMeteredRuleLetsSmallReportsAndUnmeteredNetworksThrough() {
        val limit = ReportLimits.METERED_NOTE_BYTES
        assertTrue(MeteredRule.allowed(metered = false, bytesLeft = 500L * 1024 * 1024, userAsked = false))
        assertTrue(MeteredRule.allowed(metered = true, bytesLeft = limit - 1, userAsked = false))
        assertFalse(MeteredRule.allowed(metered = true, bytesLeft = limit, userAsked = false))
        assertFalse(MeteredRule.allowed(metered = true, bytesLeft = 500L * 1024 * 1024, userAsked = false))
        // "Send now" and a report the user just sent are their decision.
        assertTrue(MeteredRule.allowed(metered = true, bytesLeft = 500L * 1024 * 1024, userAsked = true))
    }

    @Test
    fun reasonsRoundTripThroughTheirKeys() {
        for (r in UploadReason.entries) assertEquals(r, UploadReason.fromKey(r.key))
        assertEquals(null, UploadReason.fromKey("nonsense"))
        assertEquals(null, UploadReason.fromKey(null))
    }
}
