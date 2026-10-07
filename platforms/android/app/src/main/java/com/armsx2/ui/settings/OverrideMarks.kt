package com.armsx2.ui.settings

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import com.armsx2.config.ConfigStore
import com.armsx2.config.GameDbOverrides
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.ui.InGameOverlay
import org.json.JSONObject

/** Why a settings row is tinted. */
enum class OverrideMark {
    None,

    /** The game database sets this for the game being edited, and nothing the player chose outranks it. */
    GameDb,

    /** The player has this at something other than its default. */
    User,
}

/**
 * Which settings rows get a tint, for the settings on screen right now.
 *
 * A row names its setting by the key [Settings.toJson] uses (the `field` parameter of the row
 * widgets), which is also the key the per-game store and the Reset buttons use. Keying on that
 * rather than on a Kotlin property keeps this independent of how [Settings] is grouped.
 *
 * [GameDb] outranks [User]. The core applies the database on top of the player's settings, so a
 * database entry still in force is what the game runs with whatever the row shows; the player's
 * own value only reaches the game once it outranks the entry, and then the row is [User] if it
 * is not the default.
 *
 * Built lazily from [InGameOverlay]'s settings, scope and serial, which every settings tab already
 * edits, and kept until one of them changes: [current] hands every row on a screen the same
 * instance, so the work below runs once per change rather than once per row.
 */
internal class OverrideMarks private constructor(
    private val settings: Settings,
    private val serial: String?,
    private val scope: SettingsScope,
) {
    private val json: JSONObject by lazy { settings.toJson() }
    private val marks = HashMap<String, OverrideMark>()
    private val gameDb: GameDbView? by lazy { gameDbView() }

    fun markFor(field: String): OverrideMark = marks.getOrPut(field) {
        when {
            gameDb?.drives(field) == true -> OverrideMark.GameDb
            differsFromDefault(field) -> OverrideMark.User
            else -> OverrideMark.None
        }
    }

    private fun differsFromDefault(field: String): Boolean {
        val now = json.opt(field) ?: return false
        val default = DEFAULTS.opt(field) ?: return false
        // Both sides come out of toJson, so equal values have equal types and equal text.
        return now.toString() != default.toString()
    }

    /** What the database does to the game on screen. Null where it does nothing: global scope, no
     *  entry for the game, or every entry already outranked or switched off. */
    private fun gameDbView(): GameDbView? {
        if (scope != SettingsScope.Game) return null
        val key = serial?.takeIf { it.isNotBlank() } ?: return null
        val entries = GameDbOverrides.entriesFor(key)
        if (entries.isEmpty()) return null

        val memo = memoFor(key)
        // Which keys the game's own settings claim depends on WHICH fields it has overrides for,
        // not on their values, so a slider drag does not redo the probing.
        val overridden = ConfigStore.loadOverrides(key)?.keys()?.asSequence()?.toSet().orEmpty()
        if (memo.overridden != overridden) {
            memo.claimed = if (overridden.isEmpty()) emptySet()
            else GameDbOverrides.keysClaimedBySettings(key, settings, ConfigStore.loadGlobal())
            memo.overridden = overridden
        }

        val off = GameDbOverrides.switchedOff(key)
        val manualHardwareFixes = settings.anyUserHackEnabled()
        val inForce = HashSet<String>()
        for (entry in entries) {
            if (GameDbOverrides.stateOf(entry, off, memo.claimed, settings, manualHardwareFixes) == GameDbOverrides.EntryState.InForce) {
                inForce.addAll(entry.keys)
            }
        }
        if (inForce.isEmpty()) return null
        if (memo.inForce != inForce) {
            memo.inForce = inForce
            memo.drives.clear()
        }
        return GameDbView(inForce, memo)
    }

    private inner class GameDbView(
        /** "section/key" of every database entry in force for this game. */
        val inForce: Set<String>,
        val memo: Memo,
    ) {
        // Only needed when a field has not been probed yet.
        private val globalJson: JSONObject by lazy { ConfigStore.loadGlobal().toJson() }
        private val effective: Map<String, String> by lazy { settings.emittedKeys() }

        /** Whether changing [field] would move a key the database is setting. Found by trying it,
         *  as [GameDbOverrides.fieldsDriving] does, so no table of which field writes which key has
         *  to be kept in step with [Settings]. Each try loads and emits a whole [Settings], so the
         *  answer is kept in [memo] until the set of keys in force changes. */
        fun drives(field: String): Boolean = memo.drives.getOrPut(field) {
            json.has(field) && runCatching {
                GameDbOverrides.contendedKeysMovedBy(field, json, globalJson.opt(field), effective, inForce)
            }.getOrDefault(emptySet()).isNotEmpty()
        }
    }

    /** What survives a change of value: which keys the game's overrides claim, and which fields
     *  drive the keys in force. One per game; replaced when another game's settings are shown. */
    private class Memo(val serial: String) {
        var overridden: Set<String>? = null
        var claimed: Set<String> = emptySet()
        var inForce: Set<String>? = null
        val drives = HashMap<String, Boolean>()
    }

    companion object {
        private val DEFAULTS: JSONObject by lazy { Settings().toJson() }

        private var cached: OverrideMarks? = null
        private var memo: Memo? = null

        private fun memoFor(serial: String): Memo =
            memo?.takeIf { it.serial == serial } ?: Memo(serial).also { memo = it }

        /** Reads InGameOverlay's state, so a composable that calls this recomposes when it changes. */
        fun current(): OverrideMarks {
            val settings = InGameOverlay.settingsState.value
            val scope = InGameOverlay.settingsScope.value
            val serial = InGameOverlay.currentSerial.value
            cached?.let { if (it.settings === settings && it.scope == scope && it.serial == serial) return it }
            return OverrideMarks(settings, serial, scope).also { cached = it }
        }
    }
}

/** Soft sea green: reads as "set for you" without competing with the PS2 blue accent. */
private val GameDbTint = Color(0xFF57B98A)

/** Dusty rose rather than a warning red: this is a note that something was changed, not an error. */
private val UserTint = Color(0xFFC77878)

/** How much of the tint is mixed into a row's fill. The border carries the rest. */
private const val FILL_STRENGTH = 0.26f
private const val BORDER_ALPHA = 0.60f

/** The quick menu's rows are drawn fainter than the settings screen's, and a tint mixed into a
 *  faint fill barely shows, so a tinted row is never fainter than this. */
private const val TINTED_MIN_FILL_ALPHA = 0.60f

internal class RowTint(val container: Color, val border: Color)

/**
 * The fill and border for a row that edits [field] (null: not a [Settings] field, so never tinted).
 * [fillAlpha] and [outlineAlpha] are how faint the row is drawn untinted: the settings screen's
 * rows and the in-game quick menu's differ.
 */
@Composable
internal fun rowTint(field: String?, fillAlpha: Float = 0.72f, outlineAlpha: Float = 0.46f): RowTint {
    val base = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = fillAlpha)
    val outline = MaterialTheme.colorScheme.outline.copy(alpha = outlineAlpha)
    val tint = when (field?.let { OverrideMarks.current().markFor(it) }) {
        OverrideMark.GameDb -> GameDbTint
        OverrideMark.User -> UserTint
        else -> return RowTint(base, outline)
    }
    return RowTint(
        container = lerp(base.copy(alpha = 1f), tint, FILL_STRENGTH).copy(alpha = maxOf(fillAlpha, TINTED_MIN_FILL_ALPHA)),
        border = tint.copy(alpha = BORDER_ALPHA),
    )
}
