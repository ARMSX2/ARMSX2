package com.armsx2

import com.armsx2.config.AutoTuneStore
import com.armsx2.config.Settings
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for the app's side of automatic performance tuning ([AutoTuneStore]): a setting the core
 * commits into a game's settings file has to land in the app's per-game store (config.game.<key>) under the
 * field [Settings] reads it from, and a reset has to take it out again. A lever whose field name is wrong
 * here would pass every core test and still be lost on the next settings save.
 */
class AutoTuneStoreTest {
    private val global = Settings()

    private fun committed(section: String, key: String, value: String): JSONObject {
        val overrides = JSONObject()
        assertTrue("$section/$key must be a lever", AutoTuneStore.applyCommit(overrides, section, key, value))
        return overrides
    }

    @Test
    fun everyLeverReachesItsSettingsField() {
        val resolution = Settings.merge(global, committed("EmuCore/GS", "upscale_multiplier", "2.000000"))
        assertEquals(2.0f, resolution.output.upscaleFloat, 0.0f)

        val backThread = Settings.merge(global, committed("EmuCore/GS", "GSBackThreadMode", "1"))
        assertEquals(1, backThread.display.gsBackThreadMode)

        val download = Settings.merge(global, committed("EmuCore/GS", "HWDownloadMode", "5"))
        assertEquals(5, download.graphics.hardwareDownloadMode)

        val blending = Settings.merge(global, committed("EmuCore/GS", "accurate_blending_unit", "0"))
        assertEquals(0, blending.graphics.accurateBlendingUnit)

        val iop = Settings.merge(global, committed("EmuCore/Speedhacks", "iopThread", "true"))
        assertTrue(iop.cpu.iopThread)

        val governor = Settings.merge(global, committed("EmuCore/Speedhacks", "DynamicEECycleRate", "true"))
        assertTrue(governor.cpu.dynamicEeCycleRate)
    }

    @Test
    fun aCommittedValueSurvivesTheSparseDiff() {
        // What the app writes back to the store after an in-game save: only the fields that differ from global.
        val tuned = Settings.merge(global, committed("EmuCore/Speedhacks", "iopThread", "true"))
        val diff = Settings.diff(global, tuned)
        assertTrue(diff.has("iopThread"))
        assertEquals(1, diff.length())
    }

    @Test
    fun theLeverTableCoversTheCoreKeys() {
        // pcsx2/AutoTune/AutoTuneStore.cpp KeyForLever names six keys; each must have a field here.
        val keys = AutoTuneStore.levers.map { it.section + "/" + it.key }.toSet()
        assertEquals(
            setOf(
                "EmuCore/GS/upscale_multiplier", "EmuCore/GS/GSBackThreadMode", "EmuCore/GS/HWDownloadMode",
                "EmuCore/GS/accurate_blending_unit", "EmuCore/Speedhacks/iopThread", "EmuCore/Speedhacks/DynamicEECycleRate",
            ),
            keys,
        )
    }

    @Test
    fun textTheCoreWritesParses() {
        // The core writes bools as true/false, ints as plain numbers and the resolution as a float with decimals.
        assertEquals(true, AutoTuneStore.jsonValue(AutoTuneStore.levers.first { it.key == "iopThread" }, "true"))
        assertEquals(false, AutoTuneStore.jsonValue(AutoTuneStore.levers.first { it.key == "iopThread" }, "false"))
        assertEquals(3, AutoTuneStore.jsonValue(AutoTuneStore.levers.first { it.key == "HWDownloadMode" }, "3"))
        assertEquals(1.5, AutoTuneStore.jsonValue(AutoTuneStore.levers.first { it.key == "upscale_multiplier" }, "1.500000"))
        assertEquals(null, AutoTuneStore.jsonValue(AutoTuneStore.levers.first { it.key == "iopThread" }, "maybe"))
    }

    @Test
    fun anUnknownKeyIsNotWritten() {
        val overrides = JSONObject()
        assertFalse(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "TVShader", "1"))
        assertFalse(AutoTuneStore.applyCommit(overrides, "EmuCore/Speedhacks", "iopThread", "banana"))
        assertEquals(0, overrides.length())
    }

    @Test
    fun resetRestoresAnOriginalValueAndDropsAnOverrideTheGameNeverHad() {
        val overrides = JSONObject()
        overrides.put("upscaleFloat", 2.0)
        overrides.put("iopThread", true)
        overrides.put("hostFs", true) // not auto-tune's: must not be touched
        val touched = AutoTuneStore.applyRestore(
            overrides,
            listOf(
                // The game's file had the resolution (3x) before auto-tune lowered it: back to 3x.
                AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", "3"),
                // It had no iopThread key (it followed global): the override goes.
                AutoTuneStore.Restored("EmuCore/Speedhacks", "iopThread", null),
            ),
        )
        assertEquals(2, touched)
        assertEquals(3.0, overrides.getDouble("upscaleFloat"), 0.0)
        assertFalse(overrides.has("iopThread"))
        assertTrue(overrides.getBoolean("hostFs"))

        // After the reset the game resolves to global for the dropped key and the restored value for the other.
        val resolved = Settings.merge(global, overrides)
        assertFalse(resolved.cpu.iopThread)
        assertEquals(3.0f, resolved.output.upscaleFloat, 0.0f)
    }

    @Test
    fun resetOfAnAbsentOverrideChangesNothing() {
        val overrides = JSONObject()
        val touched = AutoTuneStore.applyRestore(overrides, listOf(AutoTuneStore.Restored("EmuCore/Speedhacks", "iopThread", null)))
        assertEquals(0, touched)
        assertEquals(0, overrides.length())
    }

    @Test
    fun theSwitchAndTheGovernorSurviveTheSettingsJson() {
        val s = Settings().let { it.copy(emuCore = it.emuCore.copy(autoTune = false), cpu = it.cpu.copy(dynamicEeCycleRate = true)) }
        val back = Settings.fromJson(s.toJson())
        assertFalse(back.emuCore.autoTune)
        assertTrue(back.cpu.dynamicEeCycleRate)
        // On by default, so a stored blob without the key (a save from before the switch) reads as on.
        val old = Settings.fromJson(JSONObject().also { it.put("mtvu", true) })
        assertTrue(old.emuCore.autoTune)
        assertFalse(old.cpu.dynamicEeCycleRate)
    }
}
