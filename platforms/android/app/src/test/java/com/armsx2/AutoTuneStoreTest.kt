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

    private fun upscale(overrides: JSONObject): Double = overrides.getDouble("upscaleFloat")

    @Test
    fun resetGivesBackTheValueThePlayerSetInTheAppThatTheFileNeverHeld() {
        // The player set 8x in the app's quick menu: the app's copy has it, the game's settings file does not yet.
        // Auto-tune walks the resolution down (8 -> 5 -> 4), each step a commit, and the core's record says the
        // original was absent (~), because it only reads the file. Reset must give back 8x, not "follow global".
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "5.000000", priors))
        assertEquals(5.0, upscale(overrides), 0.0)
        assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "4.000000", priors))
        assertEquals(4.0, upscale(overrides), 0.0)

        val touched = AutoTuneStore.applyRestore(
            overrides,
            listOf(AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", null)),
            priors,
        )
        assertEquals(1, touched)
        assertEquals("the player's own value", 8.0, upscale(overrides), 0.0)
        assertEquals(8.0, Settings.merge(global, overrides).output.upscaleFloat.toDouble(), 0.0)
    }

    @Test
    fun resetRemovesTheKeyWhenThePlayerHadNoOverrideOfTheirOwn() {
        val overrides = JSONObject()
        val priors = JSONObject()
        assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/Speedhacks", "iopThread", "true", priors))
        assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "3.000000", priors))
        assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "2.000000", priors))
        assertFalse("a second step of ours is not the player's value", priors.getJSONObject("upscaleFloat").has("prior"))

        val touched = AutoTuneStore.applyRestore(
            overrides,
            listOf(
                AutoTuneStore.Restored("EmuCore/Speedhacks", "iopThread", null),
                AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", null),
            ),
            priors,
        )
        assertEquals(2, touched)
        assertEquals(0, overrides.length())
    }

    @Test
    fun aValueThePlayerSetAfterTheCommitIsTheNewPriorForTheNextCommit() {
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "5.000000", priors)

        // The player picks 6x in the app; auto-tune then commits 4x on top of that.
        overrides.put("upscaleFloat", 6.0)
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "4.000000", priors)

        AutoTuneStore.applyRestore(overrides, listOf(AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", null)), priors)
        assertEquals(6.0, upscale(overrides), 0.0)
    }

    @Test
    fun aKeyThePlayerEditedAfterTheCommitIsNotInTheRestoredListAndStaysAlone() {
        // The core leaves a key the player changed since the commit out of what it restored; the recorded prior
        // must not bring the old value back over it.
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "5.000000", priors)
        overrides.put("upscaleFloat", 7.0) // the player's edit after the commit

        val touched = AutoTuneStore.applyRestore(overrides, emptyList(), priors)
        assertEquals(0, touched)
        assertEquals(7.0, upscale(overrides), 0.0)
    }

    @Test
    fun theRecordedPriorOutranksTheValueTheCoreRestoredFromItsFile() {
        // The file held 3x (an older regeneration); the player has since picked 8x in the app. 8x is the latest.
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "5.000000", priors)
        AutoTuneStore.applyRestore(overrides, listOf(AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", "3")), priors)
        assertEquals(8.0, upscale(overrides), 0.0)
    }

    @Test
    fun withoutARecordResetBehavesAsBefore() {
        val overrides = JSONObject().put("upscaleFloat", 2.0)
        AutoTuneStore.applyRestore(overrides, listOf(AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", "3")))
        assertEquals(3.0, upscale(overrides), 0.0)
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
        val s = Settings().let { it.copy(emuCore = it.emuCore.copy(autoTune = true), cpu = it.cpu.copy(dynamicEeCycleRate = true)) }
        val back = Settings.fromJson(s.toJson())
        assertTrue(back.emuCore.autoTune)
        assertTrue(back.cpu.dynamicEeCycleRate)
        // Off by default, so a stored blob without the key (a save from before the switch) reads as off.
        val old = Settings.fromJson(JSONObject().also { it.put("mtvu", true) })
        assertFalse(old.emuCore.autoTune)
        assertFalse(old.cpu.dynamicEeCycleRate)
    }

    @Test
    fun theResolutionIsTheOnlyLeverTheAppsWriterForTheGamesFileDoesNotCarry() {
        // The app regenerates the game's file from the stored settings at every in-game save, so a kept value
        // survives only if that writer emits it. All of them do but the resolution, which the app keeps in the
        // core's base layer; that is why AutoTuneStore pushes it there on a commit and on a reset.
        val sample = mapOf(
            "upscale_multiplier" to "3", "GSBackThreadMode" to "1", "HWDownloadMode" to "5",
            "accurate_blending_unit" to "0", "iopThread" to "true", "DynamicEECycleRate" to "true",
        )
        val baseline = global.emittedKeys()
        for (lever in AutoTuneStore.levers) {
            val id = lever.section + "/" + lever.key
            val tuned = Settings.merge(global, committed(lever.section, lever.key, sample.getValue(lever.key)))
            val carried = tuned.emittedKeys().let { it.containsKey(id) && it[id] != baseline[id] }
            assertEquals(id, lever.key != "upscale_multiplier", carried)
        }
    }

    @Test
    fun aCommittedResolutionIsTheOneThatIsPushedToTheBaseLayer() {
        assertEquals(3.0f, AutoTuneStore.upscaleCommitted("EmuCore/GS", "upscale_multiplier", "3.000000")!!, 0.0f)
        assertEquals(null, AutoTuneStore.upscaleCommitted("EmuCore/GS", "HWDownloadMode", "5"))
        assertEquals(null, AutoTuneStore.upscaleCommitted("EmuCore/GS", "upscale_multiplier", "banana"))
    }

    @Test
    fun aResetThatRestoredTheResolutionPushesTheStoredOneBack() {
        val restored = listOf(
            AutoTuneStore.Restored("EmuCore/Speedhacks", "iopThread", null),
            AutoTuneStore.Restored("EmuCore/GS", "upscale_multiplier", null),
        )
        // The player's own 8x, back in the store: that is what the base layer has to hold again.
        assertEquals(8.0f, AutoTuneStore.upscaleAfterRestore(restored, 8.0f)!!, 0.0f)
        // Global's native when the player had nothing of their own.
        assertEquals(1.0f, AutoTuneStore.upscaleAfterRestore(restored, 1.0f)!!, 0.0f)
        // A reset that left the resolution alone (or never touched it) pushes nothing.
        assertEquals(null, AutoTuneStore.upscaleAfterRestore(restored.take(1), 8.0f))
        assertEquals(null, AutoTuneStore.upscaleAfterRestore(emptyList(), 8.0f))
    }

    // ---- keep, then an unrelated save from the app, then Reset -------------------------------------------------------

    private fun leftAbsent(section: String, key: String, written: String) =
        AutoTuneStore.CoreReset(true, emptyList(), listOf(AutoTuneStore.LeftAlone(section, key, written, absent = true)))

    @Test
    fun theCoreReplyIsReadAsRestoredAndLeftAloneKeys() {
        val reply = AutoTuneStore.parseReset(
            listOf(
                "1",
                "restored", "EmuCore/Speedhacks", "iopThread", null,
                "restored", "EmuCore/GS", "HWDownloadMode", "2",
                "absent", "EmuCore/GS", "upscale_multiplier", "3",
                "changed", "EmuCore/GS", "accurate_blending_unit", "0",
            ),
        )
        assertTrue(reply.hadRecord)
        assertEquals(listOf("iopThread" to null, "HWDownloadMode" to "2"), reply.restored.map { it.key to it.value })
        assertEquals(
            listOf(Triple("upscale_multiplier", "3", true), Triple("accurate_blending_unit", "0", false)),
            reply.leftAlone.map { Triple(it.key, it.written, it.absent) },
        )
        assertFalse(AutoTuneStore.parseReset(listOf("0")).hadRecord)
    }

    @Test
    fun resetAfterAKeepAndAnUnrelatedSaveGivesBackTheValueThePlayerSetInTheApp() {
        // The player set 8x in the app. Auto-tune walks it down to 3x, each step a commit.
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        for (step in 7 downTo 3)
            assertTrue(AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "$step.000000", priors))
        // An unrelated save from the app: its writer regenerates the game's file without the resolution, so the core
        // finds the key missing and leaves it alone. The store keeps the 3x and gains the other setting.
        overrides.put("audioVolume", 56)

        val applied = AutoTuneStore.applyReset(overrides, priors, leftAbsent("EmuCore/GS", "upscale_multiplier", "3"))

        assertEquals("the player's own value is back", 8.0, upscale(overrides), 0.0)
        assertEquals("the unrelated setting is untouched", 56, overrides.getInt("audioVolume"))
        assertEquals(0, priors.length())
        assertTrue(applied.overridesChanged)
        assertEquals(1, applied.restored.size)
        assertEquals(0, applied.leftAlone)
        // The running emulator is told to go back to it.
        val resolved = Settings.merge(global, overrides).output.upscaleFloat
        assertEquals(8.0f, AutoTuneStore.upscaleAfterRestore(applied.restored, resolved)!!, 0.0f)
    }

    @Test
    fun resetAfterAKeepAndAnUnrelatedSaveRemovesAnOverrideThePlayerNeverHad() {
        val overrides = JSONObject()
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "3.000000", priors)
        overrides.put("audioVolume", 56)

        val applied = AutoTuneStore.applyReset(overrides, priors, leftAbsent("EmuCore/GS", "upscale_multiplier", "3"))

        assertFalse(overrides.has("upscaleFloat"))
        assertEquals(56, overrides.getInt("audioVolume"))
        assertEquals(1, applied.restored.size)
        assertEquals(global.output.upscaleFloat, Settings.merge(global, overrides).output.upscaleFloat, 0.0f)
    }

    @Test
    fun aValueThePlayerSetAfterTheKeepIsLeftAloneWhenTheFileLostTheKey() {
        val overrides = JSONObject().put("upscaleFloat", 8.0)
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "3.000000", priors)
        overrides.put("upscaleFloat", 5.0) // the player's own choice after the keep

        val applied = AutoTuneStore.applyReset(overrides, priors, leftAbsent("EmuCore/GS", "upscale_multiplier", "3"))

        assertEquals(5.0, upscale(overrides), 0.0)
        assertFalse(applied.overridesChanged)
        assertTrue(applied.restored.isEmpty())
        assertEquals(1, applied.leftAlone)
        assertEquals("nothing is pushed over the player's choice", null, AutoTuneStore.upscaleAfterRestore(applied.restored, 5.0f))
        assertFalse("the core's record is gone, so the old value has no use", priors.has("upscaleFloat"))
    }

    @Test
    fun aRecordOfAKeyThatWasNeitherPutBackNorDecidedIsKept() {
        val overrides = JSONObject().put("upscaleFloat", 8.0).put("iopThread", false)
        val priors = JSONObject()
        AutoTuneStore.applyCommit(overrides, "EmuCore/GS", "upscale_multiplier", "3.000000", priors)
        AutoTuneStore.applyCommit(overrides, "EmuCore/Speedhacks", "iopThread", "true", priors)

        // The reply names the resolution with text that does not read as a number, so it cannot be compared with the
        // store, and says nothing about the IOP thread.
        val applied = AutoTuneStore.applyReset(overrides, priors, leftAbsent("EmuCore/GS", "upscale_multiplier", "banana"))

        assertEquals(3.0, upscale(overrides), 0.0)
        assertEquals(1, applied.leftAlone)
        assertEquals(8.0, priors.getJSONObject("upscaleFloat").getDouble("prior"), 0.0)
        assertEquals(false, priors.getJSONObject("iopThread").getBoolean("prior"))
    }

    @Test
    fun aGameWithNoStoredSettingsHasNothingToRestoreInto() {
        val priors = JSONObject()
        priors.put("upscaleFloat", JSONObject().put("prior", 8.0).put("committed", 3.0))

        val applied = AutoTuneStore.applyReset(null, priors, leftAbsent("EmuCore/GS", "upscale_multiplier", "3"))

        assertFalse(applied.overridesChanged)
        assertEquals(1, applied.leftAlone)
        assertEquals(0, priors.length())
    }

    @Test
    fun everyLeverComesBackAfterAKeepAndAnUnrelatedSaveWhateverTheWriterDid() {
        // The player's own override, and the value auto-tune kept, for each key auto-tune can write.
        val player = mapOf<String, Any>(
            "upscale_multiplier" to 8.0, "GSBackThreadMode" to 0, "HWDownloadMode" to 0,
            "accurate_blending_unit" to 3, "iopThread" to false, "DynamicEECycleRate" to false,
        )
        val kept = mapOf(
            "upscale_multiplier" to "3.000000", "GSBackThreadMode" to "1", "HWDownloadMode" to "5",
            "accurate_blending_unit" to "0", "iopThread" to "true", "DynamicEECycleRate" to "true",
        )
        // Two global settings: the stock one, and one that already has the kept value. The app writes into the game's
        // file only the keys that differ from global, so the second drops every key.
        val globals = mapOf("stock global" to false, "global already at the kept value" to true)
        for ((label, globalHasKept) in globals) {
            for (lever in AutoTuneStore.levers) {
                val id = lever.section + "/" + lever.key
                val base = if (globalHasKept) Settings.merge(global, committed(lever.section, lever.key, kept.getValue(lever.key))) else global
                val overrides = JSONObject().put(lever.field, player.getValue(lever.key))
                val priors = JSONObject()
                assertTrue(AutoTuneStore.applyCommit(overrides, lever.section, lever.key, kept.getValue(lever.key), priors))
                overrides.put("audioVolume", 56)

                // What the app's writer leaves in the game's file for the game as it is now.
                val tuned = Settings.merge(base, overrides)
                val inFile = tuned.emittedKeys().let { it.containsKey(id) && it[id] != base.emittedKeys()[id] }
                assertEquals("$label, $id: the writer carries every key but the resolution, and any equal to global", !globalHasKept && lever.key != "upscale_multiplier", inFile)

                val reply = if (inFile) {
                    // Still in the file holding the kept value: the core puts it back (it was not in the file before).
                    AutoTuneStore.CoreReset(true, listOf(AutoTuneStore.Restored(lever.section, lever.key, null)), emptyList())
                } else {
                    leftAbsent(lever.section, lever.key, kept.getValue(lever.key))
                }
                val applied = AutoTuneStore.applyReset(overrides, priors, reply)

                assertTrue("$label, $id: the player's value is back", AutoTuneStore.sameJson(player.getValue(lever.key), overrides.get(lever.field)))
                assertEquals("$label, $id", 56, overrides.getInt("audioVolume"))
                assertEquals("$label, $id", 1, applied.restored.size)
                assertEquals("$label, $id", 0, applied.leftAlone)
                assertEquals("$label, $id: the record is dealt with", 0, priors.length())
            }
        }
    }
}
