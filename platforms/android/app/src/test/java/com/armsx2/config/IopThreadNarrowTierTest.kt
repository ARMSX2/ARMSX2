package com.armsx2.config

import android.content.ContextWrapper
import com.armsx2.navigation.SettingsCategory
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.settingshub.resetCategory
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The IOP thread starts on where the device's performance tier is two or three cores (the core's rule,
 * IopThread::DefaultOnForHost, reached through DeviceTier.iopThreadDefault), and an existing install is moved once.
 *
 * The JNI call cannot run in a JVM test, so each test says what the device answers: `narrow` for a device the rule
 * picks (RG 55G1: two fast cores), `wide` for one it does not (four or more fast cores, one, or a host that cannot run
 * the thread). The migration runs against the store the app reads (SharedPreferences: `config.global`, the per-game
 * `config.game.<serial>` blobs and the `config.migrated.*` marks), not the `armsx2-settings.json` mirror.
 */
class IopThreadNarrowTierTest {
    private lateinit var prefs: FakeSharedPreferences

    private val narrow: (Boolean) -> Boolean = { true }
    private val wide: (Boolean) -> Boolean = { false }

    /** A save made while the thread was off by default, as Settings.toJson wrote it. */
    private fun oldSave(iopThread: Boolean = false, lag: Int = 4_915_200, mtvu: Boolean = true): String =
        Settings().toJson().put("iopThread", iopThread).put("iopThreadLag", lag).put("mtvu", mtvu).toString()

    @Before
    fun freshStore() {
        prefs = FakeSharedPreferences()
        MainActivityRuntime.prefs = prefs
    }

    // ---- the persisted default is still off

    @Test
    fun theSettingsDefaultStaysOff() {
        // The host-aware default is applied where the device is known (a new install, the move, Reset); a save from an
        // older build, a settings file that names no key and a unit test see the plain default.
        assertFalse(Settings().cpu.iopThread)
        assertFalse(Settings.fromJson(JSONObject()).cpu.iopThread)
    }

    // ---- a new install

    @Test
    fun aNewInstallOnANarrowTierDeviceStartsWithTheThreadOn() {
        assertTrue(ConfigStore.freshInstallSettings(narrow).cpu.iopThread)
    }

    @Test
    fun aNewInstallElsewhereStartsWithTheThreadOff() {
        assertFalse(ConfigStore.freshInstallSettings(wide).cpu.iopThread)
    }

    @Test
    fun aNewInstallChangesNothingButTheThreadSetting() {
        val fresh = ConfigStore.freshInstallSettings(narrow)
        assertEquals(Settings().copy(cpu = Settings().cpu.copy(iopThread = true)), fresh)
    }

    @Test
    fun theRuleIsAskedWithTheMtvuSettingTheInstallStartsWith() {
        var asked: Boolean? = null
        ConfigStore.freshInstallSettings { mtvu -> asked = mtvu; false }
        assertEquals(Settings().cpu.mtvu, asked)
    }

    @Test
    fun seedingAnEmptyInstallWritesTheDeviceDefaultToTheStore() {
        ConfigStore.seedFreshInstallDefaults(ContextWrapper(null), narrow)
        assertTrue(JSONObject(prefs.getString("config.global", null)!!).getBoolean("iopThread"))
    }

    @Test
    fun seedingLeavesAnExistingInstallAlone() {
        val saved = oldSave(iopThread = false)
        prefs.edit().putString("config.global", saved).apply()
        ConfigStore.seedFreshInstallDefaults(ContextWrapper(null), narrow)
        assertEquals(saved, prefs.getString("config.global", null))
    }

    // ---- the move itself

    @Test
    fun aSavedOffBecomesOnOnANarrowTierDevice() {
        val old = Settings.fromJson(JSONObject(oldSave()))
        val moved = ConfigStore.withIopThreadNarrowTierMigrated(old, hostDefaultsOn = true)
        assertTrue(moved.cpu.iopThread)
        assertEquals(old.copy(cpu = old.cpu.copy(iopThread = true)), moved)
    }

    @Test
    fun aSavedOffStaysOffOnAnyOtherDevice() {
        val old = Settings.fromJson(JSONObject(oldSave()))
        assertSame(old, ConfigStore.withIopThreadNarrowTierMigrated(old, hostDefaultsOn = false))
    }

    @Test
    fun aSavedOnComesBackAsTheSameObjectEverywhere() {
        val on = Settings.fromJson(JSONObject(oldSave(iopThread = true)))
        assertSame(on, ConfigStore.withIopThreadNarrowTierMigrated(on, hostDefaultsOn = true))
        assertSame(on, ConfigStore.withIopThreadNarrowTierMigrated(on, hostDefaultsOn = false))
    }

    // ---- against the live store

    @Test
    fun theMigrationMovesTheGlobalSaveInSharedPreferences() {
        prefs.edit().putString("config.global", oldSave()).apply()

        ConfigStore.migrateIopThreadNarrowTier(narrow)

        assertTrue(JSONObject(prefs.getString("config.global", null)!!).getBoolean("iopThread"))
        assertTrue(ConfigStore.loadGlobal().cpu.iopThread)
        assertTrue(prefs.getBoolean("config.migrated.iopThreadNarrowTier", false))
    }

    @Test
    fun aDeviceWithFourOrMoreFastCoresKeepsItsSavedOff() {
        val saved = oldSave()
        prefs.edit().putString("config.global", saved).apply()

        ConfigStore.migrateIopThreadNarrowTier(wide)

        assertEquals(saved, prefs.getString("config.global", null))
        assertFalse(ConfigStore.loadGlobal().cpu.iopThread)
        assertTrue("the move is done for this install", prefs.getBoolean("config.migrated.iopThreadNarrowTier", false))
    }

    @Test
    fun itRunsOnceAndALaterOffSticks() {
        prefs.edit().putString("config.global", oldSave()).apply()
        ConfigStore.migrateIopThreadNarrowTier(narrow)

        // The player turns it off again.
        val global = ConfigStore.loadGlobal()
        ConfigStore.saveGlobal(global.copy(cpu = global.cpu.copy(iopThread = false)))

        ConfigStore.migrateIopThreadNarrowTier(narrow) // the next launch

        assertFalse(ConfigStore.loadGlobal().cpu.iopThread)
    }

    @Test
    fun aSavedOnIsLeftOnWhateverTheDevice() {
        prefs.edit().putString("config.global", oldSave(iopThread = true)).apply()
        ConfigStore.migrateIopThreadNarrowTier(wide)
        assertTrue(ConfigStore.loadGlobal().cpu.iopThread)
    }

    @Test
    fun theRuleIsAskedWithTheStoredMtvuSetting() {
        prefs.edit().putString("config.global", oldSave(mtvu = false)).apply()
        var asked: Boolean? = null
        ConfigStore.migrateIopThreadNarrowTier { mtvu -> asked = mtvu; true }
        assertEquals(false, asked)
    }

    @Test
    fun theGamesWithTheirOwnThreadSettingKeepIt() {
        prefs.edit()
            .putString("config.global", oldSave())
            // Turned off for this game on purpose, and pinned there.
            .putString("config.game.SLUS-20001", """{"iopThread":false}""")
            // Turned on for this game.
            .putString("config.game.SLUS-20002", """{"iopThread":true}""")
            .putString("config.game.SLUS-20003", """{"mtvu":false}""")
            .apply()

        ConfigStore.migrateIopThreadNarrowTier(narrow)

        assertEquals("""{"iopThread":false}""", prefs.getString("config.game.SLUS-20001", null))
        assertEquals("""{"iopThread":true}""", prefs.getString("config.game.SLUS-20002", null))
        assertEquals("""{"mtvu":false}""", prefs.getString("config.game.SLUS-20003", null))
        assertFalse(ConfigStore.resolveForGame("SLUS-20001").cpu.iopThread)
        assertTrue(ConfigStore.resolveForGame("SLUS-20002").cpu.iopThread)
        assertTrue(ConfigStore.resolveForGame("SLUS-20003").cpu.iopThread)
        assertTrue(ConfigStore.resolveForGame(null).cpu.iopThread)
    }

    @Test
    fun anInstallWithNoSaveHasNothingToMoveAndIsMarkedDone() {
        ConfigStore.migrateIopThreadNarrowTier(narrow)
        assertNull(prefs.getString("config.global", null))
        assertTrue(prefs.getBoolean("config.migrated.iopThreadNarrowTier", false))
    }

    @Test
    fun theTwoMovesAreIndependent() {
        // An install that already had the lag moved (an earlier build) and not this one: only this one runs. And the
        // other way round.
        prefs.edit()
            .putString("config.global", oldSave(lag = 24576))
            .putBoolean("config.migrated.iopThreadLag", true)
            .apply()
        ConfigStore.migrateIopThreadNarrowTier(narrow)
        ConfigStore.migrateIopThreadLag()
        var loaded = ConfigStore.loadGlobal()
        assertTrue(loaded.cpu.iopThread)
        assertEquals("the lag mark was already set, so 24576 is a choice", 24576, loaded.cpu.iopThreadLag)

        prefs.edit()
            .putString("config.global", oldSave(lag = 24576))
            .putBoolean("config.migrated.iopThreadLag", false)
            .putBoolean("config.migrated.iopThreadNarrowTier", true)
            .apply()
        ConfigStore.migrateIopThreadNarrowTier(narrow)
        ConfigStore.migrateIopThreadLag()
        loaded = ConfigStore.loadGlobal()
        assertFalse("its mark was set, so the saved off stays", loaded.cpu.iopThread)
        assertEquals(4_915_200, loaded.cpu.iopThreadLag)
    }

    // ---- Reset

    @Test
    fun resettingThePerformanceTabGivesTheDeviceDefault() {
        val off = Settings().let { it.copy(cpu = it.cpu.copy(iopThread = false)) }
        assertTrue(off.resetCategory(SettingsCategory.Performance, narrow).cpu.iopThread)
        assertFalse(off.resetCategory(SettingsCategory.Performance, wide).cpu.iopThread)

        val on = Settings().let { it.copy(cpu = it.cpu.copy(iopThread = true)) }
        assertFalse(on.resetCategory(SettingsCategory.Performance, wide).cpu.iopThread)
    }

    @Test
    fun resettingAnotherTabNeverAsksTheDevice() {
        val settings = Settings().let { it.copy(cpu = it.cpu.copy(iopThread = false)) }
        val reset = settings.resetCategory(SettingsCategory.Graphics) { error("the graphics tab does not own the IOP thread") }
        assertFalse(reset.cpu.iopThread)
    }
}
