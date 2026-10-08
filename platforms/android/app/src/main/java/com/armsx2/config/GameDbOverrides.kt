package com.armsx2.config

import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject

/**
 * What PCSX2's game database sets for one game, and which of those settings the player has taken
 * back for that game.
 *
 * The core applies the database ON TOP of the player's settings (VMManager::ApplyGameFixes). About
 * a third of the database's games set something, so for those a settings screen can show one value
 * while the game runs with another. The core already leaves a setting alone when the game's
 * per-game file carries its key -- PerGameOverrides, bmdhacks #593 -- but on Android that file was
 * the weak link. This app keeps per-game settings in its own store and wrote the file only from an
 * in-game save, and only for values that differ from global. So a choice made from the library, or
 * one that happened to equal the global value, never reached the core, and the database won.
 *
 * Two kinds of key now go into the file on top of the ones that differ from global (see
 * [claimsFor], used by Settings.writeGameSettingsIni and the boot-time stage):
 *  - every database-contended key the player set for this game, even to the global value;
 *  - the keys of every database entry switched off in the Fixes tab ([OFF_KEY]).
 */
/** Whether two values out of [Settings.toJson] differ. Both sides come out of toJson, so equal
 *  values have equal types and equal text. A side with no such field is no difference. */
internal fun valueDiffers(a: Any?, b: Any?): Boolean =
    a != null && b != null && a.toString() != b.toString()

object GameDbOverrides {
    /**
     * Where switched-off entries live: in the game's own override blob. They reset with that
     * game's settings, survive every other save (ConfigStore.save keeps keys it does not own), and
     * ride along in the settings backup. Settings.merge and Settings.diff never look at it.
     */
    const val OFF_KEY = "gameDbOff"

    class Entry(
        /** The database's own name for it, which is what the log prints ("GameDB: Skipping eeClampMode"). */
        val name: String,
        val value: Int,
        /** Only applied while automatic game fixes are on. */
        val core: Boolean,
        /** Not applied while manual hardware fixes are on. */
        val userHack: Boolean,
        /** "section/key": the per-game keys whose presence stops the database applying it. */
        val keys: List<String>,
    )

    private val entryCache = HashMap<String, List<Entry>>()

    /** What the database sets for [serial], in the order the core applies it. */
    fun entriesFor(serial: String?): List<Entry> {
        val key = serial?.trim()?.takeIf { it.isNotEmpty() } ?: return emptyList()
        synchronized(entryCache) { entryCache[key]?.let { return it } }
        // Not cached on failure: an empty answer from a call that threw would otherwise stick for
        // the whole process.
        val raw = runCatching { NativeApp.getGameDbEntries(key) }.getOrNull() ?: return emptyList()
        val parsed = raw.lineSequence().mapNotNull(::parseEntry).toList()
        synchronized(entryCache) { entryCache[key] = parsed }
        return parsed
    }

    private fun parseEntry(line: String): Entry? {
        val parts = line.split('\t')
        if (parts.size < 4) return null
        val keys = parts[3].split('|').filter { it.isNotEmpty() }
        if (keys.isEmpty()) return null
        return Entry(
            name = parts[0],
            value = parts[1].toIntOrNull() ?: return null,
            core = parts[2] == "c",
            userHack = parts[2] == "u",
            keys = keys,
        )
    }

    @Volatile
    private var claimingKeyCache: Set<String>? = null

    /** Every "section/key" whose presence in a per-game file claims some database setting. */
    fun claimingKeys(): Set<String> {
        claimingKeyCache?.let { return it }
        val raw = runCatching { NativeApp.gameDbClaimingKeys() }.getOrNull() ?: return emptySet()
        return raw.lineSequence().filter { it.isNotEmpty() }.toSet().also { claimingKeyCache = it }
    }

    /** Why a database entry is, or is not, in force for a game. Only [InForce] reaches the running game. */
    enum class EntryState { InForce, SwitchedOff, YourSetting, AutoFixesOff, ManualFixes }

    /**
     * [entry]'s state for a game whose effective settings are [resolved]. [off] is
     * [switchedOff], [claimedBySetting] is [keysClaimedBySettings], and [manualHardwareFixes] is
     * `resolved.anyUserHackEnabled()`; they are arguments so a caller listing many entries
     * computes each once. One rule for the Fixes tab's list and for the tint on the settings rows.
     */
    fun stateOf(
        entry: Entry,
        off: Set<String>,
        claimedBySetting: Set<String>,
        resolved: Settings,
        manualHardwareFixes: Boolean,
    ): EntryState = when {
        entry.name in off -> EntryState.SwitchedOff
        entry.keys.any { it in claimedBySetting } -> EntryState.YourSetting
        entry.core && !resolved.emuCore.enableGameFixes -> EntryState.AutoFixesOff
        entry.userHack && manualHardwareFixes -> EntryState.ManualFixes
        else -> EntryState.InForce
    }

    /** The database-contended keys driven by settings [serial] has its own value for. [overrides] is
     *  the game's stored blob, for a caller that has already read it. */
    fun keysClaimedBySettings(
        serial: String,
        resolved: Settings,
        global: Settings,
        overrides: JSONObject? = ConfigStore.loadOverrides(serial),
    ): Set<String> {
        if (overrides == null) return emptySet()
        return runCatching {
            fieldsDriving(overrides, resolved, global).values.flatMapTo(HashSet()) { it }
        }.getOrDefault(emptySet())
    }

    /** Names of the database entries switched off for [serial]. */
    fun switchedOff(serial: String?): Set<String> {
        val key = serial?.takeIf { it.isNotBlank() } ?: return emptySet()
        return switchedOff(ConfigStore.loadOverrides(key))
    }

    /** [switchedOff] from a game's stored blob, for a caller that has already read it. */
    fun switchedOff(overrides: JSONObject?): Set<String> {
        val arr = overrides?.optJSONArray(OFF_KEY) ?: return emptySet()
        return buildSet { for (i in 0 until arr.length()) arr.optString(i).takeIf { it.isNotEmpty() }?.let(::add) }
    }

    fun setSwitchedOff(serial: String, name: String, off: Boolean) {
        val overrides = ConfigStore.loadOverrides(serial) ?: JSONObject()
        val names = switchedOff(serial).toMutableSet()
        if (off) names.add(name) else names.remove(name)
        if (names.isEmpty()) overrides.remove(OFF_KEY) else overrides.put(OFF_KEY, JSONArray(names.sorted()))
        if (overrides.length() == 0) ConfigStore.clearOverrides(serial) else ConfigStore.saveOverrides(serial, overrides)
    }

    /**
     * Stop letting this game's own setting for [entry] win, so the database applies it again: drop
     * the per-game value of every field that drives one of its keys. That is the setting going back
     * to "same as global" for this game, which is exactly what used to leave the database in charge.
     */
    fun releaseSetting(serial: String, entry: Entry, global: Settings) {
        val overrides = ConfigStore.loadOverrides(serial) ?: return
        val resolved = Settings.merge(global, overrides)
        val drivers = fieldsDriving(overrides, resolved, global).filterValues { keys -> keys.any { it in entry.keys } }.keys
        if (drivers.isEmpty()) return
        drivers.forEach(overrides::remove)
        if (overrides.length() == 0) ConfigStore.clearOverrides(serial) else ConfigStore.saveOverrides(serial, overrides)
    }

    /** The keys a game's per-game file has to carry beyond the ones that differ from global. */
    class Claims(
        /** Keys to write with the game's own value even where it equals global's. */
        val keys: Set<String>,
        /** Keys of switched-off entries. Some have no setting behind them in this app, so the
         *  native side gives those a value (NativeApp.gameIniClaim). */
        val switchedOffKeys: Set<String>,
    ) {
        companion object {
            val NONE = Claims(emptySet(), emptySet())
        }
    }

    /**
     * What [serial]'s per-game file has to claim. [resolved] is the game's effective settings,
     * [effective] its emitted keys ("section/key" to value).
     */
    fun claimsFor(serial: String?, resolved: Settings, global: Settings, effective: Map<String, String>): Claims {
        val key = serial?.takeIf { it.isNotBlank() } ?: return Claims.NONE
        val overrides = ConfigStore.loadOverrides(key) ?: return Claims.NONE

        val keys = HashSet<String>()
        fieldsDriving(overrides, resolved, global, effective).values.forEach(keys::addAll)

        val off = switchedOff(key)
        val offKeys = if (off.isEmpty()) emptySet()
        else entriesFor(key).filter { it.name in off }.flatMapTo(HashSet()) { it.keys }
        keys.addAll(offKeys)

        return Claims(keys, offKeys)
    }

    /**
     * For each field this game has its own value for, the database-contended keys it drives.
     *
     * Found by changing the one field and seeing which emitted keys move, so it stays right as
     * settings are added -- a hand-kept table of which field writes which key would drift, and a
     * miss there is a per-game choice the database silently overwrites. A field that moves no
     * contended key simply maps to nothing, which leaves the database where it was.
     */
    fun fieldsDriving(
        overrides: JSONObject,
        resolved: Settings,
        global: Settings,
        effective: Map<String, String> = resolved.emittedKeys(),
    ): Map<String, Set<String>> {
        val contended = claimingKeys()
        if (contended.isEmpty()) return emptyMap()

        val json = resolved.toJson()
        val globalJson = global.toJson()
        val out = HashMap<String, Set<String>>()
        val names = overrides.keys()
        while (names.hasNext()) {
            val field = names.next()
            if (field == OFF_KEY || !json.has(field)) continue
            val moved = contendedKeysMovedBy(field, json, globalJson.opt(field), effective, contended)
            if (moved.isNotEmpty()) out[field] = moved
        }
        return out
    }

    /**
     * The keys in [contended] that changing [field] moves. [json] is the game's effective settings
     * ([Settings.toJson]), [globalValue] the global value of the same field, [effective] the keys
     * those settings emit. Stops at the first value that moves any of them.
     */
    internal fun contendedKeysMovedBy(
        field: String,
        json: JSONObject,
        globalValue: Any?,
        effective: Map<String, String>,
        contended: Set<String>,
    ): Set<String> {
        for (candidate in candidatesFor(field, json, globalValue)) {
            val moved = movedBy(field, candidate, json, effective, contended)
            if (moved.isNotEmpty()) return moved
        }
        return emptySet()
    }

    /**
     * Every key in [contended] that any change of [field] moves, not just the first value's. A
     * field that fans out to several keys can move different ones in each direction: stepping a
     * clamp mode up sets the "extra" bit, and stepping it down clears the plain one.
     */
    internal fun keysMovedByAny(
        field: String,
        json: JSONObject,
        globalValue: Any?,
        effective: Map<String, String>,
        contended: Set<String>,
    ): Set<String> {
        val out = HashSet<String>()
        for (candidate in candidatesFor(field, json, globalValue)) {
            out.addAll(movedBy(field, candidate, json, effective, contended))
        }
        return out
    }

    /**
     * Which database-contended keys each field moves. Which field drives which key is a property of
     * the code, not of a game or of a value, so this is kept for the process: a lookup builds and
     * emits a whole [Settings] per value tried, and repeating it for every row each time a setting
     * changed would cost a visible stall.
     */
    private val fieldKeys = HashMap<String, Set<String>>()

    /**
     * The database-contended keys [field] moves, or null if that could not be worked out. [json] is
     * the game's effective settings, [globalValue] the global value of the same field, [effective]
     * the keys those settings emit (only built if the field has not been looked up yet).
     */
    internal fun keysDrivenBy(
        field: String,
        json: JSONObject,
        globalValue: Any?,
        effective: () -> Map<String, String>,
        contended: Set<String>,
    ): Set<String>? {
        synchronized(fieldKeys) { fieldKeys[field] }?.let { return it }
        // Nothing to look up against (the native side did not answer): do not remember that.
        if (contended.isEmpty() || !json.has(field)) return null
        val keys = runCatching { keysMovedByAny(field, json, globalValue, effective(), contended) }.getOrNull() ?: return null
        synchronized(fieldKeys) { fieldKeys[field] = keys }
        return keys
    }

    /**
     * What a settings screen should show for [serial], as setting to value: what the game really
     * runs where a database entry is in force. Only the fields [GameDbFields] knows; the player's
     * stored value stands for every other one. [stored] is the game's settings as stored,
     * [overrides] its stored blob. Decisions about which entries apply are made on the stored
     * settings and never on what the screen shows, since several of the fields shown here (auto
     * flush among them) are what the core reads to tell whether hardware fixes are set by hand.
     */
    fun displayValues(serial: String, stored: Settings, global: Settings, overrides: JSONObject?): Map<String, Any> {
        val entries = entriesFor(serial)
        if (entries.isEmpty()) return emptyMap()
        return displayValues(entries, switchedOff(overrides), keysClaimedBySettings(serial, stored, global, overrides), stored)
    }

    internal fun displayValues(
        entries: List<Entry>,
        off: Set<String>,
        claimed: Set<String>,
        stored: Settings,
    ): Map<String, Any> {
        val manualHardwareFixes = stored.anyUserHackEnabled()
        val json = stored.toJson()
        val out = LinkedHashMap<String, Any>()
        for (entry in entries) {
            if (stateOf(entry, off, claimed, stored, manualHardwareFixes) != EntryState.InForce) continue
            val how = GameDbFields.sets[entry.name] ?: continue
            // A floor and a cap on one field apply one after the other.
            val player = out[how.field] ?: json.opt(how.field) ?: continue
            out[how.field] = GameDbFields.effective(how, player, entry.value) ?: continue
        }
        return out
    }

    /**
     * Square what is about to be stored for [serial] with the fact that its settings screens show
     * database values. [overrides] is the blob being written, [existing] the one it replaces,
     * [changed] the fields the player just changed, [updated] what the screen now holds.
     *
     * A value the screen showed only because the database sets it is not the player's, and would
     * otherwise be stored as the game's own the first time anything else is saved, so it is dropped
     * unless the player changed it. And a setting the player puts at the database's own value goes
     * back to the database: nothing is stored, and the row is the database's again. That is only
     * done where the entry would then really apply; otherwise the value would quietly turn into
     * the global one. Never throws: a failure leaves the blob as it was.
     */
    fun settleDatabaseValues(
        serial: String,
        changed: Set<String>,
        updated: Settings,
        global: Settings,
        existing: JSONObject?,
        overrides: JSONObject,
    ) {
        runCatching {
            val entries = entriesFor(serial)
            if (entries.isEmpty()) return
            val before = Settings.merge(global, existing ?: JSONObject())
            displayValues(serial, before, global, existing).keys.filter { it !in changed }.forEach(overrides::remove)

            val json = updated.toJson()
            for (field in changed.filter(overrides::has)) {
                val atDatabase = entries.filter { entry ->
                    val how = GameDbFields.sets[entry.name]
                    how != null && how.field == field && GameDbFields.isDatabaseValue(how, json.opt(field), entry.value)
                }
                if (atDatabase.isEmpty()) continue
                val kept = overrides.get(field)
                overrides.remove(field)
                val after = Settings.merge(global, overrides)
                val off = switchedOff(overrides)
                val claimed = keysClaimedBySettings(serial, after, global, overrides)
                val manual = after.anyUserHackEnabled()
                if (atDatabase.none { stateOf(it, off, claimed, after, manual) == EntryState.InForce }) overrides.put(field, kept)
            }
        }
    }

    private fun candidatesFor(field: String, json: JSONObject, globalValue: Any?): List<Any> {
        val current = json.get(field)
        // The global value first: for a field that differs it is the one change guaranteed to move
        // every key the field drives. A field already at the global value needs a made-up one.
        val candidates = ArrayList<Any>(3)
        if (globalValue != null && globalValue != JSONObject.NULL && globalValue != current) candidates.add(globalValue)
        when (current) {
            is Boolean -> candidates.add(!current)
            is Int -> { candidates.add(current + 1); candidates.add(current - 1) }
            is Long -> { candidates.add(current + 1); candidates.add(current - 1) }
            is Float -> { candidates.add(current + 1f); candidates.add(current - 1f) }
            is Double -> { candidates.add(current + 1.0); candidates.add(current - 1.0) }
            is String -> { candidates.add("$current~"); candidates.add("") }
        }
        return candidates
    }

    private fun movedBy(
        field: String,
        candidate: Any,
        json: JSONObject,
        effective: Map<String, String>,
        contended: Set<String>,
    ): Set<String> {
        val probe = trial(json, field, candidate) ?: return emptySet()
        return probe.emittedKeys().filterTo(HashMap()) { (id, value) -> id in contended && effective[id] != value }.keys
    }

    /** [json] with [field] set to [candidate], as a [Settings]. [json] is put back as it was, so one
     *  object serves every trial instead of being copied through a string and parsed again. */
    private fun trial(json: JSONObject, field: String, candidate: Any): Settings? {
        val original = json.opt(field)
        try {
            return runCatching { Settings.fromJson(json.put(field, candidate)) }.getOrNull()
        } finally {
            if (original != null) json.put(field, original) else json.remove(field)
        }
    }
}
