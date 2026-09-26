package com.armsx2.input

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HotkeyHoldStateTest {
    @Test fun comboStopsWhenModifierIsReleasedFirst() {
        val hold = HotkeyHoldState()
        hold.start(mainKey = 102, modifierKey = 109)
        assertFalse(hold.release(104))
        assertTrue(hold.release(109))
        assertFalse(hold.release(102))
    }

    @Test fun comboStopsWhenMainKeyIsReleasedFirst() {
        val hold = HotkeyHoldState()
        hold.start(mainKey = 102, modifierKey = 109)
        assertTrue(hold.release(102))
        assertFalse(hold.release(109))
    }

    @Test fun singleKeyHoldAndReset() {
        val hold = HotkeyHoldState()
        hold.start(mainKey = 102, modifierKey = null)
        assertTrue(hold.release(102))
        hold.start(mainKey = 102, modifierKey = 109)
        hold.clear()
        assertFalse(hold.release(109))
    }
}
