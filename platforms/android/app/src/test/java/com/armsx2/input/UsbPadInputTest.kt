package com.armsx2.input

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class UsbPadInputTest {
    private fun neutral() = ByteArray(49).apply {
        this[0] = 1
        for (i in 6..9) this[i] = 128.toByte()
    }

    @Test fun ds3PreservesEveryPressureValueOnAllTwelveButtons() {
        // Report bytes 14..25: up/right/down/left, L2/R2/L1/R1, triangle/circle/cross/square.
        val commonBits = intArrayOf(13, 14, 15, 16, 6, 7, 4, 5, 3, 1, 0, 2)
        for (i in commonBits.indices) for (value in 0..255) {
            val bytes = neutral()
            val digitalBit = i + 4
            bytes[2 + digitalBit / 8] = (1 shl (digitalBit % 8)).toByte()
            bytes[14 + i] = value.toByte()
            val report = UsbPadInput.decodeDualShock3(bytes)
            assertEquals(1 shl commonBits[i], report.buttons)
            assertEquals(value, report.pressure!![commonBits[i]])
            assertArrayEquals(intArrayOf(128, 128, 128, 128), report.axes)
        }
    }

    @Test fun ds3DigitalControlsAndPsButtonRemainAvailableToFrontend() {
        val expected = intArrayOf(8, 10, 11, 9)
        for (bit in expected.indices) {
            val bytes = neutral().apply { this[2] = (1 shl bit).toByte() }
            val report = UsbPadInput.decodeDualShock3(bytes)
            assertEquals(1 shl expected[bit], report.buttons)
            assertEquals(255, report.pressure!![expected[bit]])
        }
        assertEquals(1 shl 12, UsbPadInput.decodeDualShock3(neutral().apply { this[4] = 1 }).buttons)
        assertEquals(0, UsbPadInput.decodeDualShock3(neutral()).buttons)
    }

    @Test fun existingJniRangeRoundTripsEveryDs3PressureByte() {
        for (value in 0..255) {
            val range = UsbPadInput.pressureRange(value)
            assertTrue(range in 0..32767)
            assertEquals(value, (range / 32767f * 255f).toInt())
        }
    }

    @Test fun heldButtonWithZeroPressureIsSentAsLightestPress() {
        // applyPadButton reads a pressed button with force 0 as a full press. PadDualshock2::Set
        // sets the digital bit for any value above 0 and truncates value * 255 to the pressure
        // byte, so force 1 arrives as pressed with pressure 0.
        val force = UsbPadInput.pressureRange(0)
        assertTrue(force > 0)
        assertEquals(0, (force / 32767f * 255f).toInt())
    }
}
