package com.armsx2.config

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
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
    fun probeIgnoresKeysItWasNotAskedAbout() {
        val s = Settings()
        val moved = GameDbOverrides.contendedKeysMovedBy(
            "mtvu", s.toJson(), null, s.emittedKeys(), setOf("EmuCore/GS/UserHacks_AutoFlushLevel"),
        )
        assertTrue(moved.isEmpty())
    }
}
