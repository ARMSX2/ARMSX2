package com.armsx2.input

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class DualShock3OutputReportTest {
    private fun u(b: Byte) = b.toInt() and 255

    @Test fun motorsAndLedsLandInTheirBytes() {
        val report = DualShock3UsbInput.outputReport(0x04, large = 200, small = 255)
        assertEquals(35, report.size)
        assertEquals(0x01, u(report[0]))
        assertEquals(0xff, u(report[1]))   // small motor duration
        assertEquals(1, u(report[2]))      // small motor on
        assertEquals(0xff, u(report[3]))   // large motor duration
        assertEquals(200, u(report[4]))    // large motor force
        for (i in 5..8) assertEquals(0, u(report[i]))
        assertEquals(0x04, u(report[9]))
        val steady = byteArrayOf(0xff.toByte(), 0x27, 0x10, 0x00, 0x32)
        for (led in 0 until 4) assertArrayEquals(steady, report.copyOfRange(10 + led * 5, 15 + led * 5))
        for (i in 30 until 35) assertEquals(0, u(report[i]))
    }

    @Test fun smallMotorIsOnOrOffAndLargeMotorIsClamped() {
        assertEquals(0, u(DualShock3UsbInput.outputReport(0x02, 0, 0)[2]))
        assertEquals(1, u(DualShock3UsbInput.outputReport(0x02, 0, 1)[2]))
        assertEquals(0, u(DualShock3UsbInput.outputReport(0x02, -5, 0)[4]))
        assertEquals(255, u(DualShock3UsbInput.outputReport(0x02, 300, 0)[4]))
    }

    @Test fun motorsOffMatchesThePlayerLedReportSentBefore() {
        // With both motors off the report is the LED-only report the pad has always been sent.
        val report = DualShock3UsbInput.outputReport(0x02, 0, 0)
        val expected = ByteArray(35)
        expected[0] = 0x01
        expected[1] = 0xff.toByte()
        expected[3] = 0xff.toByte()
        expected[9] = 0x02
        val steady = byteArrayOf(0xff.toByte(), 0x27, 0x10, 0x00, 0x32)
        repeat(4) { steady.copyInto(expected, 10 + it * 5) }
        assertArrayEquals(expected, report)
    }
}
