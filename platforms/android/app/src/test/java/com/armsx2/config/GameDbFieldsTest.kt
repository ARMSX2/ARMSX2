package com.armsx2.config

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The table of which setting each game database entry sets, and what a settings screen shows for
 * one. A wrong entry does not fail loudly: a row shows a value the game does not run.
 */
class GameDbFieldsTest {
    @Test
    fun everyEntryNamesARealSettingThatCanBeShown() {
        val json = Settings().toJson()
        for ((entry, how) in GameDbFields.sets) {
            val value = json.opt(how.field)
            assertTrue("$entry names ${how.field}, which is not a setting", value != null)
            assertTrue("$entry names ${how.field}, which is neither a flag nor a whole number", value is Boolean || value is Int)
        }
    }

    @Test
    fun aFlagIsOnForAnyNonZeroDatabaseValue() {
        val how = DbSets.Exact("x")
        assertEquals(true, GameDbFields.effective(how, false, 1))
        assertEquals(true, GameDbFields.effective(how, false, 2))
        assertEquals(false, GameDbFields.effective(how, true, 0))
    }

    @Test
    fun aWholeNumberTakesTheDatabasesValue() {
        assertEquals(3, GameDbFields.effective(DbSets.Exact("x"), 1, 3))
    }

    @Test
    fun aFloorPullsUpAndACapPullsDown() {
        assertEquals(3, GameDbFields.effective(DbSets.AtLeast("x"), 1, 3))
        assertEquals(5, GameDbFields.effective(DbSets.AtLeast("x"), 5, 3))
        assertEquals(2, GameDbFields.effective(DbSets.AtMost("x"), 5, 2))
        assertEquals(1, GameDbFields.effective(DbSets.AtMost("x"), 1, 2))
    }

    @Test
    fun aFloorOrCapOnAFlagIsNotShown() {
        assertNull(GameDbFields.effective(DbSets.AtLeast("x"), true, 1))
    }

    @Test
    fun anythingButAFlagOrAWholeNumberIsNotShown() {
        assertNull(GameDbFields.effective(DbSets.Exact("x"), "text", 1))
        assertNull(GameDbFields.effective(DbSets.Exact("x"), null, 1))
        assertNull(GameDbFields.effective(DbSets.Exact("x"), 1.5, 1))
    }

    @Test
    fun aValueIsTheDatabasesOnlyWhenItIsExactlyThat() {
        val how = DbSets.Exact("x")
        assertTrue(GameDbFields.isDatabaseValue(how, 3, 3))
        assertFalse(GameDbFields.isDatabaseValue(how, 1, 3))
        assertTrue(GameDbFields.isDatabaseValue(how, true, 1))
        assertFalse(GameDbFields.isDatabaseValue(how, false, 1))
        assertFalse(GameDbFields.isDatabaseValue(how, "3", 3))
    }

    @Test
    fun aFloorIsTheDatabasesValueWhenTheRowIsExactlyAtIt() {
        assertTrue(GameDbFields.isDatabaseValue(DbSets.AtLeast("x"), 3, 3))
        assertFalse(GameDbFields.isDatabaseValue(DbSets.AtLeast("x"), 4, 3))
        assertFalse(GameDbFields.isDatabaseValue(DbSets.AtLeast("x"), true, 1))
    }
}
