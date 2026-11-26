package com.armsx2

import com.armsx2.ui.emulation.DiscSwap
import com.armsx2.ui.emulation.SessionOrder
import com.armsx2.ui.settings.firstNumberIn
import com.armsx2.ui.settings.sliderStepForTyped
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The pure parts of the typed slider values, the first tab's saved order and Swap Disc's set
 *  matching. */
class MenuSuggestionsLogicTest {
    private val percent: (Int) -> String = { "$it%" }
    private val fpsCap: (Int) -> String = { if (it == 0) "Off" else "$it fps" }
    private val speed: (Int) -> String = { if (it >= 11) "Unlimited" else "${it}×" }

    @Test fun typedNumberPicksItsStep() {
        assertEquals(150, sliderStepForTyped("150", 10, 300, percent))
        assertEquals(150, sliderStepForTyped(" 150% ", 10, 300, percent))
    }

    @Test fun typedNumberOutsideTheRangeTakesTheNearestEnd() {
        assertEquals(300, sliderStepForTyped("1000", 10, 300, percent))
        assertEquals(10, sliderStepForTyped("2", 10, 300, percent))
    }

    @Test fun aStepWithoutANumberIsReachedByItsLabelOrPastItsEnd() {
        assertEquals(0, sliderStepForTyped("off", 0, 240, fpsCap))
        assertEquals(0, sliderStepForTyped("0", 0, 240, fpsCap))
        assertEquals(60, sliderStepForTyped("60", 0, 240, fpsCap))
        assertEquals(11, sliderStepForTyped("50", 2, 11, speed))
        assertEquals(4, sliderStepForTyped("4", 2, 11, speed))
    }

    @Test fun nothingUsefulTypedChangesNothing() {
        assertNull(sliderStepForTyped("", 10, 300, percent))
        assertNull(sliderStepForTyped("abc", 10, 300, percent))
    }

    @Test fun labelsInTheUsersLocaleReadTheirDecimalComma() {
        assertEquals(59.94, firstNumberIn("59,94 Hz")!!, 1e-9)
        assertEquals(59.94, firstNumberIn("59.94 Hz")!!, 1e-9)
        assertNull(firstNumberIn("Off"))
    }

    @Test fun savedOrderWinsAndNewRowsFollowTheirDefaultNeighbour() {
        // "c" is new: it goes right after "b", the row it follows by default, wherever "b" is now.
        assertEquals(listOf("b", "c", "a"), SessionOrder.merge(listOf("b", "a"), listOf("a", "b", "c")))
        assertEquals(listOf("c", "a", "x"), SessionOrder.merge(listOf("c", "a"), listOf("a", "x", "c")))
        assertEquals(listOf("first", "b", "a"), SessionOrder.merge(listOf("b", "a"), listOf("first", "a", "b")))
        // A saved key that is not shown right now (an arcade button) is left out, not invented.
        assertEquals(listOf("a", "b"), SessionOrder.merge(listOf("arcadeCoin1", "a", "b"), listOf("a", "b")))
    }

    @Test fun discsOfOneSetMatchAndOthersDoNot() {
        assertTrue(DiscSwap.sameDiscSet("Parasite Eve II (Disc 1).chd", "Parasite Eve II (Disc 2).chd"))
        assertTrue(DiscSwap.sameDiscSet("Xenosaga II [Disc1].iso", "Xenosaga II [Disc2].iso"))
        assertTrue(DiscSwap.sameDiscSet("Game (USA) (Disc 1).iso", "Game (USA) (Disc 2).iso"))
        assertFalse(DiscSwap.sameDiscSet("Game (Disc 1).chd", "Game (Disc 2).iso"))
        assertFalse(DiscSwap.sameDiscSet("Game (Disc 1).iso", "Other Game (Disc 2).iso"))
        assertFalse(DiscSwap.sameDiscSet("Discworld (USA).iso", "Discworld (USA).iso"))
        assertEquals(2, DiscSwap.number("Parasite Eve II (Disc 2).chd"))
        assertNull(DiscSwap.number("Discworld (USA).iso"))
    }
}
