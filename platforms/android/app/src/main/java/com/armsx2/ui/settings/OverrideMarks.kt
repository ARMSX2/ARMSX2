package com.armsx2.ui.settings

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.isSpecified
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.ConfigStore
import com.armsx2.config.GameDbOverrides
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.config.valueDiffers
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay
import org.json.JSONObject

/** Why a settings row is tinted. */
enum class OverrideMark {
    None,

    /** The value is what the game database sets for the game being edited. */
    GameDb,

    /** The value is not a default: not what it was when the app was installed, or the player's own
     *  where the game database would have set another. */
    User,
}

/**
 * Which mark a row gets. A row always shows what the game really runs.
 *
 * Purple when that value comes from the game database. Red when it is not a default: it differs
 * from the factory default, whether it was changed globally or for this game, or the player put
 * their own value where the database would have set another ([overridesDatabase]; for a setting
 * the database sets, the default is the database's value, so that is red even if the player's
 * value happens to be the factory default). Otherwise no mark.
 */
internal fun decideMark(
    gameDbSetsIt: Boolean,
    overridesDatabase: Boolean,
    differsFromDefault: Boolean,
): OverrideMark = when {
    gameDbSetsIt -> OverrideMark.GameDb
    overridesDatabase || differsFromDefault -> OverrideMark.User
    else -> OverrideMark.None
}

/**
 * Which settings rows get a mark, for the settings on screen right now.
 *
 * A row names its setting by the key [Settings.toJson] uses (the `field` parameter of the row
 * widgets), which is also the key the per-game store and the Reset buttons use. Keying on that
 * rather than on a Kotlin property keeps this independent of how [Settings] is grouped.
 *
 * Built lazily from [InGameOverlay]'s settings, scope and serial, which every settings tab already
 * edits, and kept until one of them changes: [current] hands every row on a screen the same
 * instance, so the work below runs once per change rather than once per row. The settings it is
 * given are what the screen shows, which on a game's screen already has the database's values in
 * it ([ConfigStore.resolveForDisplay]); which entries apply is decided on what is stored.
 */
internal class OverrideMarks private constructor(
    private val settings: Settings,
    private val serial: String?,
    private val scope: SettingsScope,
) {
    private val json: JSONObject by lazy { settings.toJson() }
    private val marks = HashMap<String, OverrideMark>()

    /** The game on a per-game screen, null on a global one. */
    private val gameKey: String? = serial?.takeIf { it.isNotBlank() && scope == SettingsScope.Game }

    /** The game's own values, read once per snapshot. */
    private val overrides: JSONObject? by lazy { gameKey?.let(ConfigStore::loadOverrides) }

    private val gameDb: GameDbView? by lazy { gameDbView() }

    /** Whether the game database has anything to say about the game on screen. */
    val hasGameDb: Boolean get() = gameDb != null

    fun markFor(field: String): OverrideMark = marks.getOrPut(field) {
        decideMark(
            gameDbSetsIt = gameDb?.drives(field) == true,
            overridesDatabase = gameDb?.outranks(field) == true,
            differsFromDefault = valueDiffers(json.opt(field), DEFAULTS.opt(field)),
        )
    }

    /** What the database does to the game on screen. Null where it does nothing: global scope, no
     *  entry for the game, or every entry switched off or not applying. */
    private fun gameDbView(): GameDbView? {
        val key = gameKey ?: return null
        val entries = GameDbOverrides.entriesFor(key)
        if (entries.isEmpty()) return null

        val stored = ConfigStore.resolveForGame(key)
        // Which keys the game's own settings claim depends on WHICH fields it has overrides for,
        // not on their values, so a slider drag does not redo the probing.
        val memo = memoFor(key)
        val overridden = overrides?.keys()?.asSequence()?.toSet().orEmpty()
        if (memo.overridden != overridden) {
            memo.claimed = if (overridden.isEmpty()) emptySet()
            else GameDbOverrides.keysClaimedBySettings(key, stored, ConfigStore.loadGlobal(), overrides)
            memo.overridden = overridden
        }

        val off = GameDbOverrides.switchedOff(overrides)
        val manualHardwareFixes = stored.anyUserHackEnabled()
        val inForce = HashSet<String>()
        val outranked = ArrayList<GameDbOverrides.Entry>()
        for (entry in entries) {
            when (GameDbOverrides.stateOf(entry, off, memo.claimed, stored, manualHardwareFixes)) {
                GameDbOverrides.EntryState.InForce -> inForce.addAll(entry.keys)
                GameDbOverrides.EntryState.YourSetting -> outranked.add(entry)
                else -> Unit
            }
        }
        return if (inForce.isEmpty() && outranked.isEmpty()) null else GameDbView(inForce, outranked)
    }

    private inner class GameDbView(
        /** "section/key" of every database entry in force for this game. */
        val inForce: Set<String>,
        /** Entries a setting the player chose for this game outranks. */
        val outranked: List<GameDbOverrides.Entry>,
    ) {
        // Only needed when a field has not been looked up yet.
        private val globalJson: JSONObject by lazy { ConfigStore.loadGlobal().toJson() }
        private val effective: Map<String, String> by lazy { settings.emittedKeys() }

        /** The database-contended keys [field] moves, or null if that could not be worked out. */
        private fun keysOf(field: String): Set<String>? =
            GameDbOverrides.keysDrivenBy(field, json, globalJson.opt(field), { effective }, GameDbOverrides.claimingKeys())

        /** Whether changing [field] would move a key the database is setting. */
        fun drives(field: String): Boolean =
            inForce.isNotEmpty() && keysOf(field)?.any { it in inForce } == true

        /** Whether the player's own value for [field] outranks a database entry that [field] drives. */
        fun outranks(field: String): Boolean {
            if (outranked.isEmpty() || overrides?.has(field) != true) return false
            val keys = keysOf(field) ?: return false
            return outranked.any { entry -> entry.keys.any { it in keys } }
        }
    }

    /** What survives a change of value for one game: which keys its overrides claim. */
    private class Memo(val serial: String) {
        var overridden: Set<String>? = null
        var claimed: Set<String> = emptySet()
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

/** Soft purple. Next to the red it stays apart under red-green colour blindness (purple reads as
 *  blue, the red as tan), where the green it replaced did not. */
private val GameDbTint = Color(0xFFA384DB)

/** Dusty rose rather than a warning red: this is a note that something was changed, not an error. */
private val UserTint = Color(0xFFC77878)

/** How much of the tint is mixed into a row's fill. The border carries the rest. */
private const val FILL_STRENGTH = 0.26f
private const val BORDER_ALPHA = 0.60f

/** The quick menu's rows are drawn fainter than the settings screen's, and a tint mixed into a
 *  faint fill barely shows, so a tinted row is never fainter than this. */
private const val TINTED_MIN_FILL_ALPHA = 0.60f

private val MARKER_SIZE = 8.dp

/** Space between the marker and the label it sits beside. */
private val MARKER_GAP = 3.dp

internal class RowTint(val container: Color, val border: Color, val mark: OverrideMark = OverrideMark.None)

private fun OverrideMark.accent(): Color? = when (this) {
    OverrideMark.GameDb -> GameDbTint
    OverrideMark.User -> UserTint
    OverrideMark.None -> null
}

/**
 * The fill and border for a row that edits [field] (null: not a [Settings] field, so never marked).
 * [fillAlpha] and [outlineAlpha] are how faint the row is drawn unmarked: the settings screen's
 * rows and the in-game quick menu's differ.
 */
@Composable
internal fun rowTint(field: String?, fillAlpha: Float = 0.72f, outlineAlpha: Float = 0.46f): RowTint {
    val base = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = fillAlpha)
    val outline = MaterialTheme.colorScheme.outline.copy(alpha = outlineAlpha)
    val mark = field?.let { OverrideMarks.current().markFor(it) } ?: OverrideMark.None
    val tint = mark.accent() ?: return RowTint(base, outline)
    return RowTint(
        container = lerp(base.copy(alpha = 1f), tint, FILL_STRENGTH).copy(alpha = maxOf(fillAlpha, TINTED_MIN_FILL_ALPHA)),
        border = tint.copy(alpha = BORDER_ALPHA),
        mark = mark,
    )
}

/**
 * A small shape just before a row's label, level with its first line, so the two marks differ by
 * shape as well as by colour: a diamond for the game database, a circle for the player. Put it on
 * the label's own text, not the row: it is drawn in the row's padding beside the label, so it
 * follows the label wherever the widget lays it out and moves nothing. [lineHeight] is the label's.
 */
internal fun Modifier.overrideMarker(tint: RowTint, lineHeight: TextUnit): Modifier {
    val color = tint.mark.accent() ?: return this
    return drawWithContent {
        drawContent()
        val radius = MARKER_SIZE.toPx() / 2f
        val reach = MARKER_GAP.toPx() + radius
        val x = if (layoutDirection == LayoutDirection.Rtl) size.width + reach else -reach
        val line = if (lineHeight.isSpecified) lineHeight else 20.sp
        drawMarker(tint.mark, color, Offset(x, line.toPx() / 2f), radius)
    }
}

private fun DrawScope.drawMarker(mark: OverrideMark, color: Color, center: Offset, radius: Float) {
    when (mark) {
        OverrideMark.GameDb -> {
            // A little larger than the circle's radius so the two read as the same weight.
            val r = radius * 1.2f
            val diamond = Path().apply {
                moveTo(center.x, center.y - r)
                lineTo(center.x + r, center.y)
                lineTo(center.x, center.y + r)
                lineTo(center.x - r, center.y)
                close()
            }
            drawPath(diamond, color)
        }
        OverrideMark.User -> drawCircle(color, radius, center)
        OverrideMark.None -> Unit
    }
}

/** One line saying what the marks mean. The database mark is only listed where there is a database
 *  entry in force to explain. */
@Composable
internal fun OverrideLegend(modifier: Modifier = Modifier, inset: Dp = 8.dp) {
    val marks = OverrideMarks.current()
    Row(
        modifier.fillMaxWidth().padding(horizontal = inset, vertical = 2.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (marks.hasGameDb) LegendItem(OverrideMark.GameDb, str("legend.gameDb"), Modifier.weight(1f, fill = false))
        LegendItem(OverrideMark.User, str("legend.changed"), Modifier.weight(1f, fill = false))
    }
}

@Composable
private fun LegendItem(mark: OverrideMark, text: String, modifier: Modifier) {
    val color = mark.accent() ?: return
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Canvas(Modifier.size(MARKER_SIZE + 2.dp)) {
            drawMarker(mark, color, Offset(size.width / 2f, size.height / 2f), MARKER_SIZE.toPx() / 2f)
        }
        Spacer(Modifier.width(6.dp))
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 12.sp, lineHeight = 15.sp, maxLines = 2)
    }
}
