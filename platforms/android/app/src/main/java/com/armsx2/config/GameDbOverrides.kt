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

    /** Names of the database entries switched off for [serial]. */
    fun switchedOff(serial: String?): Set<String> {
        val key = serial?.takeIf { it.isNotBlank() } ?: return emptySet()
        val arr = ConfigStore.loadOverrides(key)?.optJSONArray(OFF_KEY) ?: return emptySet()
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

    private fun contendedKeysMovedBy(
        field: String,
        json: JSONObject,
        globalValue: Any?,
        effective: Map<String, String>,
        contended: Set<String>,
    ): Set<String> {
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
        for (candidate in candidates) {
            val probe = runCatching {
                Settings.fromJson(JSONObject(json.toString()).put(field, candidate))
            }.getOrNull() ?: continue
            val moved = probe.emittedKeys().filterTo(HashMap()) { (id, value) -> id in contended && effective[id] != value }.keys
            if (moved.isNotEmpty()) return moved
        }
        return emptySet()
    }
}
