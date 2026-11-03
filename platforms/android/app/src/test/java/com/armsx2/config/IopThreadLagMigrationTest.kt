package com.armsx2.config

import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The IOP thread is off by default, and a save made while the lag defaulted to 24576 is moved to the one-frame default
 * once.
 *
 * The migration is tested against the store the app reads (SharedPreferences: `config.global` and the per-game
 * `config.game.<serial>` blobs), not the `armsx2-settings.json` mirror in the data folder, which [ConfigStore] only
 * writes through to and reads back when `config.global` is missing.
 */
class IopThreadLagMigrationTest {
    private lateinit var prefs: FakeSharedPreferences

    /** What Settings.toJson wrote while the lag default was 24576. */
    private fun oldSave(lag: Int = 24576, iopThread: Boolean = false): String =
        Settings().toJson().put("iopThread", iopThread).put("iopThreadLag", lag).toString()

    @Before
    fun freshStore() {
        prefs = FakeSharedPreferences()
        MainActivityRuntime.prefs = prefs
    }

    // ---- the defaults

    @Test
    fun theThreadIsOffAndTheLagIsOneFrameByDefault() {
        assertFalse(Settings().cpu.iopThread)
        assertEquals(4_915_200, Settings().cpu.iopThreadLag)
    }

    @Test
    fun aSettingsFileThatNamesNeitherKeyGetsTheDefaults() {
        val loaded = Settings.fromJson(JSONObject())
        assertFalse(loaded.cpu.iopThread)
        assertEquals(4_915_200, loaded.cpu.iopThreadLag)
    }

    // ---- the move itself

    @Test
    fun anOldSaveGetsTheOneFrameLag() {
        val moved = ConfigStore.withIopThreadLagMigrated(Settings.fromJson(JSONObject(oldSave())))
        assertEquals(4_915_200, moved.cpu.iopThreadLag)
    }

    @Test
    fun anyOtherLagIsKept() {
        for (lag in listOf(4096, 6144, 12288, 24575, 24577, 196_608, 4_915_200, 9_830_400)) {
            val moved = ConfigStore.withIopThreadLagMigrated(Settings.fromJson(JSONObject(oldSave(lag))))
            assertEquals("lag $lag", lag, moved.cpu.iopThreadLag)
        }
    }

    @Test
    fun aSaveThatNeedsNoMoveComesBackAsTheSameObject() {
        val current = Settings()
        assertSame(current, ConfigStore.withIopThreadLagMigrated(current))
    }

    @Test
    fun onlyTheLagIsTouchedAndTheThreadSettingStaysWhateverItWas() {
        for (thread in listOf(false, true)) {
            val old = Settings.fromJson(JSONObject(oldSave(iopThread = thread)))
            val moved = ConfigStore.withIopThreadLagMigrated(old)
            assertEquals(thread, moved.cpu.iopThread)
            assertEquals(old.copy(cpu = old.cpu.copy(iopThreadLag = 4_915_200)), moved)
        }
    }

    // ---- against the live store

    @Test
    fun theMigrationMovesTheGlobalSaveInSharedPreferences() {
        prefs.edit().putString("config.global", oldSave()).apply()

        ConfigStore.migrateIopThreadLag()

        val stored = JSONObject(prefs.getString("config.global", null)!!)
        assertEquals(4_915_200, stored.getInt("iopThreadLag"))
        assertFalse("the thread setting is not moved", stored.getBoolean("iopThread"))
        assertEquals(4_915_200, ConfigStore.loadGlobal().cpu.iopThreadLag)
        assertTrue(prefs.getBoolean("config.migrated.iopThreadLag", false))
    }

    @Test
    fun aThreadTheUserTurnedOnIsLeftOn() {
        prefs.edit().putString("config.global", oldSave(iopThread = true)).apply()
        ConfigStore.migrateIopThreadLag()
        val loaded = ConfigStore.loadGlobal()
        assertTrue(loaded.cpu.iopThread)
        assertEquals(4_915_200, loaded.cpu.iopThreadLag)
    }

    @Test
    fun aLagThePlayerPickedInTheStoreIsKept() {
        prefs.edit().putString("config.global", oldSave(lag = 12288)).apply()
        ConfigStore.migrateIopThreadLag()
        assertEquals(12288, ConfigStore.loadGlobal().cpu.iopThreadLag)
    }

    @Test
    fun itRunsOnceAndALagPickedAfterwardsStays() {
        prefs.edit().putString("config.global", oldSave()).apply()
        ConfigStore.migrateIopThreadLag()

        // The player picks the old lag on purpose.
        val global = ConfigStore.loadGlobal()
        ConfigStore.saveGlobal(global.copy(cpu = global.cpu.copy(iopThreadLag = 24576)))

        ConfigStore.migrateIopThreadLag() // the next launch

        assertEquals(24576, ConfigStore.loadGlobal().cpu.iopThreadLag)
    }

    @Test
    fun theGamesWithTheirOwnLagKeepIt() {
        prefs.edit()
            .putString("config.global", oldSave())
            // Pinned at the old default lag for this game.
            .putString("config.game.SLUS-20001", """{"iopThreadLag":24576}""")
            // A game that turned the thread on for itself.
            .putString("config.game.SLUS-20002", """{"iopThread":true}""")
            // A game with a blob that says nothing about the thread.
            .putString("config.game.SLUS-20003", """{"mtvu":false}""")
            .apply()

        ConfigStore.migrateIopThreadLag()

        // The blobs are byte for byte what they were.
        assertEquals("""{"iopThreadLag":24576}""", prefs.getString("config.game.SLUS-20001", null))
        assertEquals("""{"iopThread":true}""", prefs.getString("config.game.SLUS-20002", null))
        assertEquals("""{"mtvu":false}""", prefs.getString("config.game.SLUS-20003", null))

        // What each game runs with: its own choice where it made one, the new global lag where it did not.
        assertEquals(24576, ConfigStore.resolveForGame("SLUS-20001").cpu.iopThreadLag)
        assertEquals(4_915_200, ConfigStore.resolveForGame("SLUS-20002").cpu.iopThreadLag)
        assertTrue(ConfigStore.resolveForGame("SLUS-20002").cpu.iopThread)
        assertEquals(4_915_200, ConfigStore.resolveForGame("SLUS-20003").cpu.iopThreadLag)
        assertFalse(ConfigStore.resolveForGame("SLUS-20003").cpu.iopThread)
        assertEquals(4_915_200, ConfigStore.resolveForGame(null).cpu.iopThreadLag)
    }

    @Test
    fun anInstallWithNoSaveHasNothingToMoveAndIsMarkedDone() {
        ConfigStore.migrateIopThreadLag()

        assertNull(prefs.getString("config.global", null))
        assertTrue(prefs.getBoolean("config.migrated.iopThreadLag", false))
        assertEquals(4_915_200, ConfigStore.loadGlobal().cpu.iopThreadLag)
    }

    @Test
    fun aSaveThatNeedsNoMoveIsNotRewritten() {
        val current = Settings().toJson().toString()
        prefs.edit().putString("config.global", current).apply()
        ConfigStore.migrateIopThreadLag()
        assertEquals(current, prefs.getString("config.global", null))
    }
}
