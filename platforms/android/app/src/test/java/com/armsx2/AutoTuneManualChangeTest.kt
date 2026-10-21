package com.armsx2

import com.armsx2.config.AutoTuneStore
import com.armsx2.config.Settings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A player who sets one of the settings automatic performance tuning moves, in a game's own settings, has
 * chosen that value: auto-tune would go on adjusting it, so it is turned off for that game
 * ([AutoTuneStore.withAutoTuneOffForManualChange]). Any other setting leaves it alone.
 */
class AutoTuneManualChangeTest {
    // Auto-tune is off by default; the rule under test only applies to a game it is on for.
    private val before = Settings().let { it.copy(emuCore = it.emuCore.copy(autoTune = true)) }

    private fun changedBy(change: (Settings) -> Settings): Settings =
        AutoTuneStore.withAutoTuneOffForManualChange(before, change(before))

    @Test
    fun changingAnyLeverTurnsAutoTuneOffAndKeepsThePlayersValue() {
        val resolution = changedBy { it.copy(output = it.output.copy(upscaleFloat = 3.0f)) }
        assertFalse("resolution", resolution.emuCore.autoTune)
        assertEquals(3.0f, resolution.output.upscaleFloat, 0.0f)

        val backThread = changedBy { it.copy(display = it.display.copy(gsBackThreadMode = 1)) }
        assertFalse("GS back thread", backThread.emuCore.autoTune)
        assertEquals(1, backThread.display.gsBackThreadMode)

        val download = changedBy { it.copy(graphics = it.graphics.copy(hardwareDownloadMode = 5)) }
        assertFalse("download mode", download.emuCore.autoTune)
        assertEquals(5, download.graphics.hardwareDownloadMode)

        val blending = changedBy { it.copy(graphics = it.graphics.copy(accurateBlendingUnit = 0)) }
        assertFalse("blending", blending.emuCore.autoTune)
        assertEquals(0, blending.graphics.accurateBlendingUnit)

        val iop = changedBy { it.copy(cpu = it.cpu.copy(iopThread = true)) }
        assertFalse("IOP thread", iop.emuCore.autoTune)
        assertTrue(iop.cpu.iopThread)

        val governor = changedBy { it.copy(cpu = it.cpu.copy(dynamicEeCycleRate = true)) }
        assertFalse("EE governor", governor.emuCore.autoTune)
        assertTrue(governor.cpu.dynamicEeCycleRate)
    }

    @Test
    fun everyLeverTheStoreCarriesIsCovered() {
        // The rule reads the same list the store carries, so a lever added to it is covered. This fails for such a
        // lever until it has a case here: a lever nothing here changes would never be shown to turn auto-tune off.
        for (lever in AutoTuneStore.levers) {
            val updated = when (lever.field) {
                "upscaleFloat" -> before.copy(output = before.output.copy(upscaleFloat = 3.0f))
                "gsBackThreadMode" -> before.copy(display = before.display.copy(gsBackThreadMode = 1))
                "hardwareDownloadMode" -> before.copy(graphics = before.graphics.copy(hardwareDownloadMode = 5))
                "accurateBlendingUnit" -> before.copy(graphics = before.graphics.copy(accurateBlendingUnit = 0))
                "iopThread" -> before.copy(cpu = before.cpu.copy(iopThread = true))
                "dynamicEeCycleRate" -> before.copy(cpu = before.cpu.copy(dynamicEeCycleRate = true))
                else -> error("no case for ${lever.field}: add one")
            }
            assertFalse(lever.field, AutoTuneStore.withAutoTuneOffForManualChange(before, updated).emuCore.autoTune)
        }
    }

    @Test
    fun anyOtherSettingLeavesAutoTuneOn() {
        val louder = changedBy { it.copy(audio = it.audio.copy(audioVolume = 50)) }
        assertTrue(louder.emuCore.autoTune)
        assertEquals(50, louder.audio.audioVolume)
    }

    @Test
    fun nothingChangedLeavesAutoTuneOn() {
        assertTrue(changedBy { it }.emuCore.autoTune)
    }

    @Test
    fun whenItIsAlreadyOffTheSettingsComeBackAsTheyWere() {
        val off = before.copy(emuCore = before.emuCore.copy(autoTune = false))
        val updated = off.copy(output = off.output.copy(upscaleFloat = 3.0f))
        assertEquals(updated, AutoTuneStore.withAutoTuneOffForManualChange(off, updated))
    }

    @Test
    fun turningItOnByHandIsNotUndone() {
        val off = before.copy(emuCore = before.emuCore.copy(autoTune = false))
        val on = off.copy(emuCore = off.emuCore.copy(autoTune = true))
        assertTrue(AutoTuneStore.withAutoTuneOffForManualChange(off, on).emuCore.autoTune)
    }
}
