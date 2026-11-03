package com.armsx2.config

import com.armsx2.ui.settings.IOP_THREAD_LAG_CHOICES
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The IOP thread's lag bound: its default is one frame, the picker offers it, and a value saved by an
 * earlier version is kept as saved (the app stores every setting, defaults included, in armsx2-settings.json).
 */
class IopThreadLagSettingTest {
    /** EE cycles in one NTSC frame: 294,912,000 a second at 60 Hz. IopThread::kOneFrameLag in IopThreadMode.h. */
    private val oneFrame = 294_912_000 / 60

    /** The range the core clamps the setting to (IopThread::kMinLag .. kMaxLag). */
    private val coreMin = 4096
    private val coreMax = 9_830_400

    @Test
    fun theDefaultIsOneFrame() {
        assertEquals(4_915_200, oneFrame)
        assertEquals(oneFrame, Settings().cpu.iopThreadLag)
    }

    @Test
    fun theDefaultIsInsideTheRangeTheCoreAccepts() {
        val lag = Settings().cpu.iopThreadLag
        assertTrue(lag in coreMin..coreMax)
    }

    @Test
    fun thePickerOffersTheDefaultAndEveryChoiceIsInsideTheCoresRange() {
        assertTrue(IOP_THREAD_LAG_CHOICES.contains(Settings().cpu.iopThreadLag))
        assertTrue(IOP_THREAD_LAG_CHOICES.contains(24576))
        assertTrue(IOP_THREAD_LAG_CHOICES.all { it in coreMin..coreMax })
        assertEquals(IOP_THREAD_LAG_CHOICES.sorted(), IOP_THREAD_LAG_CHOICES)
    }

    @Test
    fun aSettingsFileThatNamesNoLagGetsTheNewDefault() {
        val loaded = Settings.fromJson(JSONObject())
        assertEquals(oneFrame, loaded.cpu.iopThreadLag)
    }

    @Test
    fun aLagSavedExplicitlyIsKeptWhateverTheDefaultIs() {
        val saved = Settings().toJson().put("iopThreadLag", 24576)
        assertEquals(24576, Settings.fromJson(saved).cpu.iopThreadLag)
    }

    @Test
    fun theSettingsFileWritesTheLagOutEvenAtTheDefault() {
        // This is why an install that has saved its settings once keeps the value it had then.
        val json = Settings().toJson()
        assertTrue(json.has("iopThreadLag"))
        assertEquals(oneFrame, json.getInt("iopThreadLag"))
    }
}
