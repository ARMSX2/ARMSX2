package com.armsx2.config

import android.util.Log
import com.armsx2.EmuState
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject
import java.util.concurrent.Executors

/**
 * The app's side of auto-tune (the emulator core's automatic performance tuning).
 *
 * The core keeps what auto-tune changed in the game's own settings file, `gamesettings/<serial>_<CRC>.ini`:
 * the keys it set, and an `[AutoTune]` section recording what they were before. On Android that file is
 * not the source of truth. The app owns per-game settings (`config.game.<key>`, a sparse JSON of the fields
 * the player overrode) and regenerates the file from it whenever it saves or boots a game, wiping every
 * `EmuCore*` section first. A value that only the file holds is therefore gone the next time the app writes
 * it. This object keeps the two in step, in both directions:
 *
 *  - [onCommitted]: the core moved one setting into the file; write it into `config.game.<key>` too.
 *  - [reset]: the core put the file's keys back to what they were; do the same to `config.game.<key>`.
 *
 * The `[AutoTune]` section is outside the sections the app regenerates, so it survives and still carries
 * the record.
 */
object AutoTuneStore {
    enum class Kind { BOOL, INT, FLOAT }

    /** A settings-file key auto-tune can write, and the per-game field (the JSON key of [Settings]) it is here. */
    class Lever(val section: String, val key: String, val field: String, val kind: Kind)

    /**
     * Every key auto-tune can write: one for each lever in pcsx2/AutoTune/AutoTuneStore.cpp (KeyForLever).
     * KEEP IN SYNC with that function; a key missing here is written to the file but dropped from the app's
     * copy, so the next regeneration loses it.
     */
    val levers: List<Lever> = listOf(
        Lever("EmuCore/GS", "upscale_multiplier", "upscaleFloat", Kind.FLOAT),
        Lever("EmuCore/GS", "GSBackThreadMode", "gsBackThreadMode", Kind.INT),
        Lever("EmuCore/GS", "HWDownloadMode", "hardwareDownloadMode", Kind.INT),
        Lever("EmuCore/GS", "accurate_blending_unit", "accurateBlendingUnit", Kind.INT),
        Lever("EmuCore/Speedhacks", "iopThread", "iopThread", Kind.BOOL),
        Lever("EmuCore/Speedhacks", "DynamicEECycleRate", "dynamicEeCycleRate", Kind.BOOL),
    )

    /** A key the core put back in the game's file. [value] is the text it holds again, null when it deleted the key. */
    class Restored(val section: String, val key: String, val value: String?)

    /** What a reset did. [restored] is how many keys the core put back; [leftAlone] how many the player had changed since. */
    class ResetOutcome(val hadRecord: Boolean, val restored: Int, val leftAlone: Int)

    private const val TAG = "ARMSX2"

    // One thread, so a commit that follows another is written after it. The core calls on its own (CPU) thread,
    // which must not wait for the app's storage.
    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "autotune-store").apply { isDaemon = true } }
    private val storeLock = Any()

    private fun leverFor(section: String, key: String): Lever? =
        levers.firstOrNull { it.section == section && it.key == key }

    /** [text] (as the settings file holds it) as the JSON value of [lever]'s field, or null when it does not parse. */
    internal fun jsonValue(lever: Lever, text: String): Any? {
        val t = text.trim()
        return when (lever.kind) {
            Kind.BOOL -> when (t.lowercase()) {
                "true", "1" -> true
                "false", "0" -> false
                else -> null
            }
            Kind.INT -> t.toIntOrNull() ?: t.toDoubleOrNull()?.toInt()
            Kind.FLOAT -> t.toDoubleOrNull()
        }
    }

    /** Writes a committed key into the sparse per-game [overrides]. False when the key is not one of ours or the text does not parse. */
    internal fun applyCommit(overrides: JSONObject, section: String, key: String, value: String): Boolean {
        val lever = leverFor(section, key) ?: return false
        val json = jsonValue(lever, value) ?: return false
        overrides.put(lever.field, json)
        return true
    }

    /**
     * Puts back what a reset restored. A key the core deleted had no value in the game's file, so the game followed
     * global for it: the override goes. A key it restored to a value had one: the override is that value again.
     * Returns how many fields were touched.
     */
    internal fun applyRestore(overrides: JSONObject, restored: List<Restored>): Int {
        var touched = 0
        for (r in restored) {
            val lever = leverFor(r.section, r.key) ?: continue
            if (r.value == null) {
                if (overrides.has(lever.field)) {
                    overrides.remove(lever.field)
                    touched++
                }
            } else {
                val json = jsonValue(lever, r.value) ?: continue
                overrides.put(lever.field, json)
                touched++
            }
        }
        return touched
    }

    /** The key `config.game.<key>` for the running game: its disc serial, else the app's own key for it (a homebrew ELF has none). */
    private fun settingsKeyFor(serial: String): String? =
        serial.takeIf { it.isNotBlank() } ?: MainActivityRuntime.currentGame.value?.settingsKey?.takeIf { it.isNotBlank() }

    /**
     * The core moved [section]/[key] into the game's settings file (CPU thread). Writes it into the app's copy
     * of the game's settings so the file regeneration that follows an in-game save or a relaunch keeps it.
     * Returns at once; the write happens on a worker thread.
     */
    fun onCommitted(serial: String, section: String, key: String, value: String) {
        if (leverFor(section, key) == null) {
            Log.w(TAG, "auto-tune committed $section/$key, which the app has no field for; it will not survive a settings save")
            return
        }
        val settingsKey = settingsKeyFor(serial)
        if (settingsKey == null) {
            Log.w(TAG, "auto-tune committed $section/$key but no game is identified; it will not survive a settings save")
            return
        }
        worker.execute {
            runCatching {
                synchronized(storeLock) {
                    val overrides = ConfigStore.loadOverrides(settingsKey) ?: JSONObject()
                    if (applyCommit(overrides, section, key, value)) {
                        ConfigStore.saveOverrides(settingsKey, overrides)
                        Log.i(TAG, "auto-tune: $section/$key = $value written to config.game.$settingsKey")
                    } else {
                        Log.w(TAG, "auto-tune: could not read '$value' for $section/$key")
                    }
                }
            }.onFailure { Log.e(TAG, "auto-tune write-back failed for $section/$key", it) }
        }
    }

    /** What the core's `serial` argument must be for the game [settingsKey]: blank for the running game when it has no serial of its own. */
    private fun nativeSerialFor(settingsKey: String): String {
        val running = MainActivityRuntime.currentGame.value
        val vmRunning = MainActivityRuntime.nativeReady.value && MainActivityRuntime.eState.value != EmuState.STOPPED
        return if (vmRunning && running != null && running.settingsKey == settingsKey && running.serial.isNullOrBlank()) "" else settingsKey
    }

    /** What auto-tune changed for a game, one readable line each, then what it tried without benefit. Reads a file: call off the main thread. */
    fun describe(settingsKey: String): List<String> =
        runCatching { NativeApp.autoTuneDescribe(nativeSerialFor(settingsKey)).toList() }.getOrDefault(emptyList())

    /**
     * Puts back what auto-tune changed for the game: the core restores the game's file (and applies it, when that
     * game is running), then this does the same to the app's copy, and re-applies the app's settings to a running
     * game so the core's base layer stops holding the old values. Null when the game has no settings file, so
     * there is no record.
     */
    suspend fun reset(settingsKey: String): ResetOutcome? {
        val raw = withContext(Dispatchers.IO) {
            runCatching { NativeApp.autoTuneReset(nativeSerialFor(settingsKey)) }.getOrNull()
        } ?: return null
        val hadRecord = raw.getOrNull(0) == "1"
        val leftAlone = raw.getOrNull(1)?.toIntOrNull() ?: 0
        val restored = ArrayList<Restored>()
        var i = 2
        while (i + 2 < raw.size) {
            restored.add(Restored(raw[i] ?: "", raw[i + 1] ?: "", raw[i + 2]))
            i += 3
        }

        withContext(Dispatchers.IO) {
            synchronized(storeLock) {
                val overrides = ConfigStore.loadOverrides(settingsKey)
                if (overrides != null && applyRestore(overrides, restored) > 0) {
                    if (overrides.length() == 0) ConfigStore.clearOverrides(settingsKey)
                    else ConfigStore.saveOverrides(settingsKey, overrides)
                }
            }
        }

        // The core's base layer was filled from the app's settings, which held the kept values; push the restored
        // ones. Only for the game that is running: otherwise the next boot does it.
        withContext(Dispatchers.Main) {
            val running = MainActivityRuntime.nativeReady.value &&
                MainActivityRuntime.eState.value != EmuState.STOPPED &&
                MainActivityRuntime.currentGame.value?.settingsKey == settingsKey
            if (running && hadRecord) runCatching { ConfigStore.resolveForGame(settingsKey).applyTo() }
        }
        return ResetOutcome(hadRecord, restored.size, leftAlone)
    }
}
