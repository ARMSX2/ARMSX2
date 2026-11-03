package com.armsx2.bugreport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The code people paste into a chat: how it is written, and every way it is typed back. */
class ReportCodeTest {
    @Test
    fun formatsAnIdWithThePrefixAndDashes() {
        assertEquals("AX-7KQ4-M2PD", ReportCode.format("7KQ4M2PD"))
    }

    @Test
    fun anIdOfTheWrongLengthIsLeftAlone() {
        assertEquals("7KQ4", ReportCode.format("7KQ4"))
    }

    @Test
    fun normalizesEveryWayOfWritingTheSameCode() {
        for (typed in listOf("AX-7KQ4-M2PD", "ax-7kq4-m2pd", "7KQ4M2PD", "7kq4-m2pd", " AX 7KQ4 M2PD ", "AX7KQ4M2PD")) {
            assertEquals(typed, "7KQ4M2PD", ReportCode.normalize(typed))
        }
    }

    @Test
    fun readsLookAlikeLettersTheWayTheServerDoes() {
        // O is 0, I and L are 1. The alphabet has no I, L, O or U.
        assertEquals("01234567", ReportCode.normalize("OI234567"))
        assertEquals("01234567", ReportCode.normalize("AX-0L23-4567"))
    }

    @Test
    fun anIdThatHappensToStartWithAxIsNotMistakenForThePrefix() {
        assertEquals("AX7KQ4M2", ReportCode.normalize("AX7KQ4M2"))
        assertEquals("AX7KQ4M2", ReportCode.normalize("AX-AX7K-Q4M2"))
    }

    @Test
    fun rejectsWhatIsNotACode() {
        assertNull(ReportCode.normalize(""))
        assertNull(ReportCode.normalize("AX-7KQ4"))
        assertNull(ReportCode.normalize("AX-7KQ4-M2PDX"))
        // U is not in the alphabet.
        assertNull(ReportCode.normalize("AX-7KQU-M2PD"))
    }

    @Test
    fun normalizedCodesFormatBackToTheSameCode() {
        val id = ReportCode.normalize("ax 7kq4 m2pd")!!
        assertEquals("AX-7KQ4-M2PD", ReportCode.format(id))
    }
}
