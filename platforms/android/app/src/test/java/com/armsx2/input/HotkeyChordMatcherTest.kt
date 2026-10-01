package com.armsx2.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HotkeyChordMatcherTest {
    private val select = 109
    private val r1 = 103
    private val bindings = listOf(
        ControllerMappings.RuntimeHotkey(ControllerMappings.SysHotkey.SAVE_STATE, r1, 0),
        ControllerMappings.RuntimeHotkey(ControllerMappings.SysHotkey.FAST_FORWARD, r1, select),
        ControllerMappings.RuntimeHotkey(ControllerMappings.SysHotkey.MENU, select, 0),
    )

    @Test fun comboActivatesWhenEitherButtonCompletesIt() {
        val expected = ControllerMappings.SysHotkey.FAST_FORWARD
        assertEquals(expected, ControllerMappings.matchHotkey(r1, setOf(select, r1), bindings))
        assertEquals(expected, ControllerMappings.matchHotkey(select, setOf(r1, select), bindings))
    }

    @Test fun comboWinsOverSingleOnEitherArrivingButton() {
        assertEquals(
            ControllerMappings.SysHotkey.SAVE_STATE,
            ControllerMappings.matchHotkey(r1, setOf(r1), bindings),
        )
        assertEquals(
            ControllerMappings.SysHotkey.MENU,
            ControllerMappings.matchHotkey(select, setOf(select), bindings),
        )
        assertEquals(
            ControllerMappings.SysHotkey.FAST_FORWARD,
            ControllerMappings.matchHotkey(select, setOf(r1, select), bindings),
        )
    }

    @Test fun unknownAndUnboundKeysDoNotCompleteCombo() {
        assertNull(ControllerMappings.matchHotkey(0, setOf(r1, select), bindings))
        assertNull(ControllerMappings.matchHotkey(104, setOf(r1, select), bindings))
    }

    @Test fun singleBindingStillMatchesOnReleaseWithoutHeldKeys() {
        assertEquals(
            ControllerMappings.SysHotkey.SAVE_STATE,
            ControllerMappings.matchHotkey(r1, emptySet(), bindings),
        )
    }

    @Test fun duplicateReversedBindingsKeepRuntimePriority() {
        val duplicates = listOf(
            ControllerMappings.RuntimeHotkey(ControllerMappings.SysHotkey.FAST_FORWARD, r1, select),
            ControllerMappings.RuntimeHotkey(ControllerMappings.SysHotkey.MENU, select, r1),
        )
        for (arriving in listOf(r1, select)) {
            assertEquals(
                ControllerMappings.SysHotkey.FAST_FORWARD,
                ControllerMappings.matchHotkey(arriving, setOf(r1, select), duplicates),
            )
        }
    }

    @Test fun reverseOrderHoldEndsOnEitherRelease() {
        for (released in listOf(r1, select)) {
            val hold = HotkeyHoldState()
            hold.start(mainKey = r1, modifierKey = select)
            assertFalse(hold.release(104))
            assertTrue(hold.release(released))
            assertFalse(hold.release(if (released == r1) select else r1))
        }
    }
}
