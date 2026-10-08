package com.armsx2.ui.settings

import com.armsx2.config.valueDiffers
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which mark a settings row gets. Getting this wrong does not crash anything, it tells the player
 * something false: that a value is the game database's when it is not, or that it is a default
 * when it is not.
 */
class OverrideMarkTest {
    private fun mark(
        gameDbSetsIt: Boolean = false,
        overridesDatabase: Boolean = false,
        differsFromDefault: Boolean = false,
    ) = decideMark(gameDbSetsIt, overridesDatabase, differsFromDefault)

    @Test
    fun aValueAtItsDefaultHasNoMark() {
        assertEquals(OverrideMark.None, mark())
    }

    @Test
    fun aValueThatIsNotTheDefaultIsRed() {
        assertEquals(OverrideMark.User, mark(differsFromDefault = true))
    }

    @Test
    fun theDatabasesValueIsPurple() {
        assertEquals(OverrideMark.GameDb, mark(gameDbSetsIt = true))
    }

    @Test
    fun aDatabaseValueThatIsNotTheDefaultIsPurpleNotRed() {
        // The row shows what the game runs, so a database value that is not the default is purple, not red.
        assertEquals(OverrideMark.GameDb, mark(gameDbSetsIt = true, differsFromDefault = true))
    }

    @Test
    fun anOverrideOfTheDatabaseIsRedEvenAtTheFactoryDefault() {
        // For a setting the database sets, the game's default is the database's value.
        assertEquals(OverrideMark.User, mark(overridesDatabase = true, differsFromDefault = false))
    }

    @Test
    fun valuesWithTheSameTextDoNotDiffer() {
        // A float out of toJson and the same number read back from stored text.
        assertFalse(valueDiffers(1.5f, 1.5))
        assertFalse(valueDiffers(true, true))
        assertFalse(valueDiffers(3, 3))
    }

    @Test
    fun valuesWithDifferentTextDiffer() {
        assertTrue(valueDiffers(3, 4))
        assertTrue(valueDiffers(true, false))
        assertTrue(valueDiffers("Sockets", "Auto"))
    }

    @Test
    fun aFieldMissingOnEitherSideIsNoDifference() {
        assertFalse(valueDiffers(null, 3))
        assertFalse(valueDiffers(3, null))
        assertFalse(valueDiffers(null, null))
    }
}
