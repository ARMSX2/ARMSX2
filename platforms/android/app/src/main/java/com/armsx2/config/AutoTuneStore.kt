package com.armsx2.config

import android.util.Log
import androidx.core.content.edit
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
 * The core records what the *file* held before a commit, and the file is not where the player's own choices
 * live: a value picked in the quick menu is in the app's copy only until the file is next regenerated, so the
 * core's record says the key was absent and a reset would hand the game back to the global setting. [onCommitted]
 * therefore also notes the override the app's copy had before it wrote ([notePrior], kept in
 * `config.autotune.prior.<key>`), and [reset] puts that value back when there is one.
 *
 * The `[AutoTune]` section is outside the sections the app regenerates, so it survives and still carries
 * the record.
 *
 * One lever is not carried by that regeneration: the resolution. The app's writer for the game's file never
 * emits `upscale_multiplier`; the app keeps the resolution in the core's base layer (renderUpscalemultiplier,
 * pushed at boot and on a change). A kept resolution that only the game's file holds is therefore gone from it
 * at the next save from the app, and the base layer's older value takes over. So the core also puts a kept
 * resolution in the base layer when it commits it (AndroidOnAutoTuneCommitted in native-lib.cpp), and [reset]
 * pushes the restored resolution back ([upscaleAfterRestore]).
 *
 * The same save also leaves out any other key whose value equals the global one, so after it the core can find
 * any lever's key missing from the file when it resets. The app's store, not the file, decides what that means
 * ([applyReset]).
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

    /**
     * The player changed [previous] into [updated] by hand, in a game's own settings. When that moved a setting
     * auto-tune writes (a [levers] field), auto-tune would go on adjusting what the player just chose, so it is
     * turned off for that game: the result is [updated] with the switch off. A change to anything else, or
     * auto-tune already being off, gives [updated] back as it is. What auto-tune already changed, and its
     * record, stay.
     */
    fun withAutoTuneOffForManualChange(previous: Settings, updated: Settings): Settings {
        if (!updated.emuCore.autoTune) return updated
        val changed = Settings.diff(previous, updated)
        if (levers.none { changed.has(it.field) }) return updated
        return updated.copy(emuCore = updated.emuCore.copy(autoTune = false))
    }

    /** A key the core put back in the game's file. [value] is the text it holds again, null when it deleted the key. */
    class Restored(val section: String, val key: String, val value: String?)

    /**
     * A key the core did not put back. [written] is the text auto-tune wrote for it; [absent] says the key is not in the
     * game's file at all (as opposed to holding another value).
     */
    class LeftAlone(val section: String, val key: String, val written: String, val absent: Boolean)

    /** The core's answer to a reset: whether there was a record, the keys it put back, and the ones it left alone. */
    class CoreReset(val hadRecord: Boolean, val restored: List<Restored>, val leftAlone: List<LeftAlone>)

    /**
     * What a reset did to the app's copy. [restored] is every key put back: the core's, and the ones [applyReset] took
     * back itself. [leftAlone] is how many stayed as they are. [overridesChanged] says the store has to be written.
     */
    class ResetApplied(val restored: List<Restored>, val leftAlone: Int, val overridesChanged: Boolean)

    /** What a reset did. [restored] is how many keys were put back; [leftAlone] how many the player had changed since. */
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

    private const val PRIOR = "prior"
    private const val COMMITTED = "committed"
    private const val PRIOR_PREFS_KEY = "config.autotune.prior."

    /** Whether two JSON values of a lever field are the same setting (2 and 2.0 are). */
    internal fun sameJson(a: Any?, b: Any?): Boolean {
        if (a == null || b == null) return a == b
        if (a is Number && b is Number) return Math.abs(a.toDouble() - b.toDouble()) < 1e-6
        return a == b
    }

    private const val UPSCALE_SECTION = "EmuCore/GS"
    private const val UPSCALE_KEY = "upscale_multiplier"

    /** The resolution a committed [section]/[key] sets, or null when it is another key or the text is not a number. */
    internal fun upscaleCommitted(section: String, key: String, value: String): Float? =
        if (section == UPSCALE_SECTION && key == UPSCALE_KEY) value.trim().toFloatOrNull() else null

    /**
     * The resolution to push to the core's base layer after a reset, or null when the reset did not touch it. The
     * core has put the game's file back by then, so what the emulator runs is whatever the base layer holds, and
     * the base layer holds the resolution auto-tune last kept: [resolved] is what the stored settings say it is now.
     */
    internal fun upscaleAfterRestore(restored: List<Restored>, resolved: Float): Float? =
        if (restored.any { it.section == UPSCALE_SECTION && it.key == UPSCALE_KEY }) resolved.coerceIn(0.25f, 8.0f) else null

    /**
     * Notes, before [committed] is written over [overrides], the value the player's own override of [lever] had,
     * in [priors] (`field -> {prior?, committed}`). Called for every commit; only the first step on a field takes
     * the override as the player's: while the override still holds the value auto-tune committed last, it is
     * auto-tune's own and the record keeps what was there before. If the override is something else now, the
     * player set it since, and that is the value to give back. No override at all records no `prior`.
     */
    internal fun notePrior(priors: JSONObject, overrides: JSONObject, lever: Lever, committed: Any) {
        val entry = priors.optJSONObject(lever.field)
        val current: Any? = if (overrides.has(lever.field)) overrides.get(lever.field) else null
        if (entry != null && current != null && entry.has(COMMITTED) && sameJson(entry.get(COMMITTED), current)) {
            entry.put(COMMITTED, committed)
            return
        }
        val fresh = JSONObject()
        if (current != null) fresh.put(PRIOR, current)
        fresh.put(COMMITTED, committed)
        priors.put(lever.field, fresh)
    }

    /**
     * Writes a committed key into the sparse per-game [overrides]. False when the key is not one of ours or the text
     * does not parse. With [priors], also notes the override that was there before ([notePrior]).
     */
    internal fun applyCommit(overrides: JSONObject, section: String, key: String, value: String, priors: JSONObject? = null): Boolean {
        val lever = leverFor(section, key) ?: return false
        val json = jsonValue(lever, value) ?: return false
        priors?.let { notePrior(it, overrides, lever, json) }
        overrides.put(lever.field, json)
        return true
    }

    /**
     * Puts back what a reset restored. A key the player had an override for before auto-tune ([priors]) gets that
     * value back, whatever the core's file said. Otherwise: a key the core deleted had no value in the game's file,
     * so the game followed global for it and the override goes; a key it restored to a value had one, and the
     * override is that value again. A key the core left alone (the player changed it since) is not in [restored]
     * and is not touched. Returns how many fields were touched.
     */
    internal fun applyRestore(overrides: JSONObject, restored: List<Restored>, priors: JSONObject? = null): Int {
        var touched = 0
        for (r in restored) {
            val lever = leverFor(r.section, r.key) ?: continue
            val prior = priors?.optJSONObject(lever.field)
            if (prior != null && prior.has(PRIOR)) {
                overrides.put(lever.field, prior.get(PRIOR))
                touched++
                continue
            }
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

    private const val KIND_RESTORED = "restored"
    private const val KIND_ABSENT = "absent"
    private const val KIND_CHANGED = "changed"

    /**
     * Reads the core's reply to [NativeApp.autoTuneReset]: `hadRecord "1"/"0"`, then four strings for each key the
     * record named: kind (restored, absent or changed), section, key, value. The value of a restored key is the text
     * the file holds again, null where the core deleted it; for a key left alone it is the text auto-tune wrote.
     */
    internal fun parseReset(raw: List<String?>): CoreReset {
        val restored = ArrayList<Restored>()
        val leftAlone = ArrayList<LeftAlone>()
        var i = 1
        while (i + 3 < raw.size) {
            val section = raw[i + 1] ?: ""
            val key = raw[i + 2] ?: ""
            val value = raw[i + 3]
            when (raw[i]) {
                KIND_RESTORED -> restored.add(Restored(section, key, value))
                KIND_ABSENT -> leftAlone.add(LeftAlone(section, key, value ?: "", absent = true))
                KIND_CHANGED -> leftAlone.add(LeftAlone(section, key, value ?: "", absent = false))
            }
            i += 4
        }
        return CoreReset(raw.getOrNull(0) == "1", restored, leftAlone)
    }

    /**
     * Whether the app's copy of [lever] still holds [written], the text auto-tune put in the game's file: nothing has
     * set the field since the commit. Null when the text does not read as a value of the lever.
     */
    private fun holdsWritten(overrides: JSONObject?, lever: Lever, written: String): Boolean? {
        val value = jsonValue(lever, written) ?: return null
        val current: Any? = if (overrides != null && overrides.has(lever.field)) overrides.get(lever.field) else null
        return sameJson(current, value)
    }

    /**
     * Applies the core's reset to the app's copy: [overrides] (null when the game has none) and [priors], in place.
     *
     * The core puts a key back only while the game's file still holds the value it wrote, and counts a key missing
     * from the file as the player's doing. On Android the file is not where the player's settings live: it is
     * regenerated from the app's store at every save, and the app's writer leaves out the resolution always and any
     * other key whose value equals the global one. So a kept value can be missing from the file with the player
     * having done nothing, and the store is what says so. A key the core left alone because it is absent from the file,
     * while the store still holds the value auto-tune committed, is auto-tune's own and goes back like one the core
     * restored (the player's recorded value, or no override if there was none). If the store holds something else the
     * player set it since, and it stays.
     *
     * A recorded player value is dropped only for a key this dealt with: put back, or left alone because the player
     * changed it. A key that cannot be compared keeps its record.
     */
    internal fun applyReset(overrides: JSONObject?, priors: JSONObject, reply: CoreReset): ResetApplied {
        val takenBack = ArrayList<Restored>()
        val decided = ArrayList<Lever>()
        var leftAlone = 0
        for (l in reply.leftAlone) {
            val lever = leverFor(l.section, l.key)
            if (lever == null) {
                leftAlone++
                continue
            }
            // A key that is in the file with another value was changed there, which is the core's call to leave.
            val ours = if (l.absent) holdsWritten(overrides, lever, l.written) else false
            when (ours) {
                true -> takenBack.add(Restored(l.section, l.key, null))
                false -> {
                    leftAlone++
                    decided.add(lever)
                }
                null -> leftAlone++
            }
        }

        val restored = reply.restored + takenBack
        val touched = if (overrides != null) applyRestore(overrides, restored, priors) else 0
        for (r in restored) leverFor(r.section, r.key)?.let { priors.remove(it.field) }
        for (lever in decided) priors.remove(lever.field)
        return ResetApplied(restored, leftAlone, touched > 0)
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
                    val priors = loadPriors(settingsKey)
                    if (applyCommit(overrides, section, key, value, priors)) {
                        ConfigStore.saveOverrides(settingsKey, overrides)
                        savePriors(settingsKey, priors)
                        // What the app believes the emulator runs at, for the resolution hotkeys.
                        upscaleCommitted(section, key, value)?.let { MainActivityRuntime.upscale.value = it }
                        Log.i(TAG, "auto-tune: $section/$key = $value written to config.game.$settingsKey")
                    } else {
                        Log.w(TAG, "auto-tune: could not read '$value' for $section/$key")
                    }
                }
            }.onFailure { Log.e(TAG, "auto-tune write-back failed for $section/$key", it) }
        }
    }

    private fun loadPriors(settingsKey: String): JSONObject =
        runCatching {
            MainActivityRuntime.prefs.getString(PRIOR_PREFS_KEY + settingsKey, null)?.let { JSONObject(it) }
        }.getOrNull() ?: JSONObject()

    private fun savePriors(settingsKey: String, priors: JSONObject) {
        MainActivityRuntime.prefs.edit {
            if (priors.length() == 0) remove(PRIOR_PREFS_KEY + settingsKey) else putString(PRIOR_PREFS_KEY + settingsKey, priors.toString())
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
     * game is running), then this does the same to the app's copy ([applyReset], which also covers a key the file no
     * longer has), and re-applies the app's settings to a running game so the core's base layer stops holding the
     * old values. Null when the game has no settings file, so there is no record.
     */
    suspend fun reset(settingsKey: String): ResetOutcome? {
        val raw = withContext(Dispatchers.IO) {
            runCatching { NativeApp.autoTuneReset(nativeSerialFor(settingsKey)) }.getOrNull()
        } ?: return null
        val reply = parseReset(raw.toList())

        val applied = withContext(Dispatchers.IO) {
            synchronized(storeLock) {
                val overrides = ConfigStore.loadOverrides(settingsKey)
                val priors = loadPriors(settingsKey)
                val done = applyReset(overrides, priors, reply)
                if (overrides != null && done.overridesChanged) {
                    if (overrides.length() == 0) ConfigStore.clearOverrides(settingsKey)
                    else ConfigStore.saveOverrides(settingsKey, overrides)
                }
                // The core clears its record on a reset; so does this one, for the keys it dealt with.
                if (reply.hadRecord) savePriors(settingsKey, priors)
                done
            }
        }

        // The core's base layer was filled from the app's settings, which held the kept values; push the restored
        // ones. Only for the game that is running: otherwise the next boot does it. The resolution is not part of
        // what applyTo pushes (it goes through renderUpscalemultiplier), and the base layer holds the kept one.
        withContext(Dispatchers.Main) {
            val running = MainActivityRuntime.nativeReady.value &&
                MainActivityRuntime.eState.value != EmuState.STOPPED &&
                MainActivityRuntime.currentGame.value?.settingsKey == settingsKey
            if (running && reply.hadRecord) runCatching {
                val resolved = ConfigStore.resolveForGame(settingsKey)
                resolved.applyTo()
                upscaleAfterRestore(applied.restored, resolved.output.upscaleFloat)?.let {
                    MainActivityRuntime.upscale.value = it
                    NativeApp.renderUpscalemultiplier(it)
                }
            }
        }
        return ResetOutcome(reply.hadRecord, applied.restored.size, applied.leftAlone)
    }
}
