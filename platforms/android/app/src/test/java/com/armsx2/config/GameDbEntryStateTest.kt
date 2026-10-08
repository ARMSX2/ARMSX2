package com.armsx2.config

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.json.JSONObject
import org.junit.Test

/**
 * The rule for when a game database entry is in force, shared by the Fixes tab's list and the
 * tint on the settings rows, and the probe that ties a settings field to the INI keys it drives.
 * A wrong answer here does not crash anything: a row is the wrong colour, or the list says an
 * entry applies when the core is skipping it.
 */
class GameDbEntryStateTest {
    private val key = "EmuCore/Speedhacks/vuThread"

    private fun entry(vararg keys: String, core: Boolean = true, userHack: Boolean = false) =
        GameDbOverrides.Entry(name = "e", value = 1, core = core, userHack = userHack, keys = keys.toList())

    private fun state(
        entry: GameDbOverrides.Entry,
        off: Set<String> = emptySet(),
        claimed: Set<String> = emptySet(),
        settings: Settings = Settings(),
        manualHardwareFixes: Boolean = false,
    ) = GameDbOverrides.stateOf(entry, off, claimed, settings, manualHardwareFixes)

    private fun withGameFixes(on: Boolean) =
        Settings().let { it.copy(emuCore = it.emuCore.copy(enableGameFixes = on)) }

    @Test
    fun anEntryNothingOutranksIsInForce() {
        assertEquals(GameDbOverrides.EntryState.InForce, state(entry(key)))
    }

    @Test
    fun switchingAnEntryOffBeatsEverythingElse() {
        assertEquals(
            GameDbOverrides.EntryState.SwitchedOff,
            state(entry(key), off = setOf("e"), claimed = setOf(key), settings = withGameFixes(false)),
        )
    }

    @Test
    fun aSettingTheGameChoseOutranksTheDatabase() {
        assertEquals(
            GameDbOverrides.EntryState.YourSetting,
            state(entry(key), claimed = setOf(key), settings = withGameFixes(false)),
        )
    }

    @Test
    fun aClaimOnAnotherKeyLeavesTheEntryAlone() {
        assertEquals(
            GameDbOverrides.EntryState.InForce,
            state(entry(key), claimed = setOf("EmuCore/GS/UserHacks_AutoFlushLevel")),
        )
    }

    @Test
    fun turningAutomaticGameFixesOffOnlyStopsTheCoreEntries() {
        val off = withGameFixes(false)
        assertEquals(GameDbOverrides.EntryState.AutoFixesOff, state(entry(key), settings = off))
        assertEquals(
            GameDbOverrides.EntryState.InForce,
            state(entry(key, core = false, userHack = true), settings = off),
        )
    }

    @Test
    fun manualHardwareFixesOnlyStopTheUserHackEntries() {
        assertEquals(
            GameDbOverrides.EntryState.ManualFixes,
            state(entry(key, core = false, userHack = true), manualHardwareFixes = true),
        )
        assertEquals(GameDbOverrides.EntryState.InForce, state(entry(key), manualHardwareFixes = true))
    }

    @Test
    fun probeFindsTheKeyAFieldDrives() {
        val s = Settings()
        val moved = GameDbOverrides.contendedKeysMovedBy("mtvu", s.toJson(), null, s.emittedKeys(), setOf(key))
        assertEquals(setOf(key), moved)
    }

    @Test
    fun probeFindsTheKeyOfAnIntField() {
        val autoFlush = "EmuCore/GS/UserHacks_AutoFlushLevel"
        val s = Settings()
        val moved = GameDbOverrides.contendedKeysMovedBy("autoFlush", s.toJson(), null, s.emittedKeys(), setOf(autoFlush))
        assertEquals(setOf(autoFlush), moved)
    }

    @Test
    fun lookupCollectsTheKeysMovedInEveryDirection() {
        // Stepping a clamp mode up sets the "extra" bit and stepping it down clears the plain one,
        // so the first value tried is not the whole answer.
        val s = Settings()
        val effective = s.emittedKeys()
        val moved = GameDbOverrides.keysMovedByAny("vuClampMode", s.toJson(), null, effective, effective.keys)
        assertTrue("EmuCore/CPU/Recompiler/vu0Overflow" in moved)
        assertTrue("EmuCore/CPU/Recompiler/vu0ExtraOverflow" in moved)
    }

    @Test
    fun aTrialLeavesTheSettingsJsonAsItFoundIt() {
        val s = Settings()
        val json = s.toJson()
        val before = json.toString()
        GameDbOverrides.keysMovedByAny("vuClampMode", json, null, s.emittedKeys(), setOf(key))
        GameDbOverrides.contendedKeysMovedBy("mtvu", json, null, s.emittedKeys(), setOf(key))
        assertEquals(before, json.toString())
    }

    private fun named(name: String, value: Int, core: Boolean = true, userHack: Boolean = false) =
        GameDbOverrides.Entry(name = name, value = value, core = core, userHack = userHack, keys = listOf("k/$name"))

    private fun shown(
        vararg entries: GameDbOverrides.Entry,
        off: Set<String> = emptySet(),
        claimed: Set<String> = emptySet(),
        stored: Settings = Settings(),
    ) = GameDbOverrides.displayValues(entries.toList(), off, claimed, stored)

    @Test
    fun anEntryInForceShowsTheDatabasesValue() {
        assertEquals(mapOf<String, Any>("eeClampMode" to 3), shown(named("eeClampMode", 3)))
    }

    @Test
    fun aFlagEntryShowsOnOrOff() {
        // Multi-threaded VU1 is on by default.
        assertEquals(mapOf<String, Any>("mtvu" to false), shown(named("mtvu", 0)))
    }

    @Test
    fun aFloorRaisesTheRowAndACapLowersIt() {
        // Blending accuracy is at 1 by default.
        assertEquals(mapOf<String, Any>("accurateBlendingUnit" to 3), shown(named("minimumBlendingLevel", 3)))
        assertEquals(mapOf<String, Any>("accurateBlendingUnit" to 0), shown(named("maximumBlendingLevel", 0)))
    }

    @Test
    fun aFloorAndACapOnOneFieldApplyOneAfterTheOther() {
        assertEquals(
            mapOf<String, Any>("accurateBlendingUnit" to 2),
            shown(named("minimumBlendingLevel", 3), named("maximumBlendingLevel", 2)),
        )
    }

    @Test
    fun anEntryThePlayerSwitchedOffShowsNothing() {
        assertTrue(shown(named("eeClampMode", 3), off = setOf("eeClampMode")).isEmpty())
    }

    @Test
    fun anEntryAChosenSettingOutranksShowsNothing() {
        assertTrue(shown(named("eeClampMode", 3), claimed = setOf("k/eeClampMode")).isEmpty())
    }

    @Test
    fun anEntryWithNoSettingInTheAppShowsNothing() {
        assertTrue(shown(named("FpuMul", 1)).isEmpty())
    }

    @Test
    fun aCoreEntryShowsNothingWhileAutomaticFixesAreOff() {
        val stored = Settings().let { it.copy(emuCore = it.emuCore.copy(enableGameFixes = false)) }
        assertTrue(shown(named("eeClampMode", 3), stored = stored).isEmpty())
    }

    @Test
    fun aHardwareFixEntryShowsNothingWhileHardwareFixesAreSetByHand() {
        // Decided on what is stored: the player's own auto flush is what counts as setting them by hand.
        val stored = Settings().let { it.copy(hwFixes = it.hwFixes.copy(autoFlush = 1)) }
        assertTrue(shown(named("halfPixelOffset", 2, core = false, userHack = true), stored = stored).isEmpty())
    }

    @Test
    fun probeIgnoresKeysItWasNotAskedAbout() {
        val s = Settings()
        val moved = GameDbOverrides.contendedKeysMovedBy(
            "mtvu", s.toJson(), null, s.emittedKeys(), setOf("EmuCore/GS/UserHacks_AutoFlushLevel"),
        )
        assertTrue(moved.isEmpty())
    }
}
