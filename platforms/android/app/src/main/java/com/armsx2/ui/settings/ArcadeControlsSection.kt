// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.settings

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.arcade.Arcade
import com.armsx2.arcade.ArcadeControls
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.i18n.str
import com.armsx2.ui.Colors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Arcade controls: what each pad button does on an arcade game's cabinet (ArcadeControls). In All Settings >
 * Controls ([pickGame]): the layout for every arcade game (All arcade games), or any arcade game's own in the
 * library, the one being played first. In the pause menu's Controls: the game being played ([gameId]).
 */
@Composable
internal fun ArcadeControlsSection(gameId: String?, pickGame: Boolean) {
    CollapsibleSection(str("arcade.controls.section"), initiallyExpanded = false) {
        val app = LocalContext.current.applicationContext as android.app.Application
        val chosen = remember { mutableStateOf(gameId ?: if (pickGame) ArcadeControls.GLOBAL else null) }
        // The library's arcade games (ID, name); null while they are read.
        val games = remember { mutableStateOf<List<Pair<String, String>>?>(null) }
        if (pickGame) {
            LaunchedEffect(Unit) {
                games.value = withContext(Dispatchers.IO) {
                    runCatching {
                        GameLibraryRepository(app).loadCached().games
                            .filter { it.extension == Arcade.BADGE && !it.serial.isNullOrBlank() }
                            .map { it.serial.orEmpty().uppercase() to it.title }
                            .distinctBy { it.first }
                            .sortedBy { it.second.lowercase() }
                    }.getOrDefault(emptyList())
                }
            }
        }
        val global = chosen.value == ArcadeControls.GLOBAL
        Text(
            str(if (global) "arcade.controls.allIntro" else "arcade.controls.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
        )
        if (pickGame) {
            val choices = listOf(ArcadeControls.GLOBAL to str("arcade.controls.all")) + games.value.orEmpty()
            val title = choices.firstOrNull { it.first == chosen.value }?.second ?: chosen.value.orEmpty()
            ArcadeGamePickerRow(chosen.value, title, choices) { chosen.value = it }
        }
        when (val id = chosen.value) {
            null -> Unit
            ArcadeControls.GLOBAL -> GlobalLayoutRows()
            else -> ArcadeLayoutRows(id)
        }
    }
}

/** The All arcade games layout: the pad button each pad button acts as, in every arcade game. */
@Composable
private fun GlobalLayoutRows() {
    val tick = remember { mutableIntStateOf(0) }
    val picking = remember { mutableStateOf<Int?>(null) }
    @Suppress("UNUSED_EXPRESSION")
    tick.intValue // read again after a change
    val changes = ArcadeControls.globalChanges()
    Column {
        ArcadeControls.buttons.forEach { button ->
            val target = changes[button] ?: button
            ArcadeValueRow(
                label = ArcadeControls.buttonName(button),
                value = if (target == ArcadeControls.NOTHING) str("arcade.controls.nothing") else ArcadeControls.buttonName(target),
                highlight = changes.containsKey(button),
                id = "arcadectl:all:$button",
                enabled = true,
            ) { picking.value = button }
        }
        Spacer(Modifier.height(4.dp))
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(1f))
            PickerButton(str("arcade.controls.reset"), "arcadectl:all:reset") {
                ArcadeControls.reset(ArcadeControls.GLOBAL)
                tick.intValue++
            }
        }
    }
    picking.value?.let { button ->
        val current = changes[button] ?: button
        ArcadePicker(
            title = str("arcade.controls.pick").format(ArcadeControls.buttonName(button)),
            layer = "arcade-all-picker",
            items = ArcadeControls.buttons.map { ArcadeControls.buttonName(it) to it.toString() },
            selected = { v -> if (v == null) current == ArcadeControls.NOTHING else v == current.toString() },
            onPick = { v ->
                ArcadeControls.setGlobal(button, v?.toIntOrNull() ?: ArcadeControls.NOTHING)
                picking.value = null
                tick.intValue++
            },
            onDismiss = { picking.value = null },
            extra = str("arcade.controls.nothing"),
        )
    }
}

/** The game whose controls the section shows, and the list to choose another from. */
@Composable
private fun ArcadeGamePickerRow(currentId: String?, title: String, games: List<Pair<String, String>>, onPick: (String) -> Unit) {
    val open = remember { mutableStateOf(false) }
    ArcadeValueRow(str("arcade.controls.game"), title, highlight = false, id = "arcadectl:game", enabled = true) {
        open.value = true
    }
    if (open.value) {
        ArcadePicker(
            title = str("arcade.controls.pickGame"),
            layer = "arcade-game-picker",
            items = games.map { it.second to it.first },
            selected = { it == currentId },
            onPick = { id -> if (id != null) onPick(id); open.value = false },
            onDismiss = { open.value = false },
            extra = null,
        )
    }
}

/** Every pad button of [gameId], with the job it does on the cabinet; tap one to give it another. */
@Composable
private fun ArcadeLayoutRows(gameId: String) {
    val layout = remember(gameId) { ArcadeControls.layout(gameId) }
    if (layout == null) {
        Text(
            str("arcade.controls.unavailable"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 15.sp,
            modifier = Modifier.padding(6.dp),
        )
        return
    }
    val tick = remember(gameId) { mutableIntStateOf(0) }
    val picking = remember(gameId) { mutableStateOf<Int?>(null) }
    @Suppress("UNUSED_EXPRESSION")
    tick.intValue // read again after a change
    val hint = when (layout.mode) {
        ArcadeControls.MODE_DRIVE -> "arcade.controls.hint.drive"
        ArcadeControls.MODE_LIGHTGUN -> "arcade.controls.hint.gun"
        ArcadeControls.MODE_TWINSTICK -> "arcade.controls.hint.twin"
        ArcadeControls.MODE_DRUM -> null
        else -> "arcade.controls.hint.lever"
    }
    if (hint != null) {
        Text(
            str(hint),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
        )
    }
    val changes = ArcadeControls.changes(gameId)
    Column {
        ArcadeControls.buttons.forEach { button ->
            val fixed = layout.defaultJob(button)?.takeIf { it.fixed }
            val job = fixed ?: ArcadeControls.jobOf(gameId, layout, button)
            val value = when {
                fixed != null -> str("arcade.controls.analog").format(fixed.label)
                job == null -> str("arcade.controls.nothing")
                else -> job.label
            }
            ArcadeValueRow(
                label = ArcadeControls.buttonName(button),
                value = value,
                highlight = changes.containsKey(button),
                id = "arcadectl:$button",
                enabled = fixed == null,
            ) { picking.value = button }
        }
        Spacer(Modifier.height(4.dp))
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(1f))
            PickerButton(str("arcade.controls.reset"), "arcadectl:reset") {
                ArcadeControls.reset(gameId)
                tick.intValue++
            }
        }
    }
    picking.value?.let { button ->
        val current = ArcadeControls.jobOf(gameId, layout, button)
        val jobs = layout.jobs.filter { !it.fixed }
        ArcadePicker(
            title = str("arcade.controls.pick").format(ArcadeControls.buttonName(button)),
            layer = "arcade-job-picker",
            items = jobs.map { it.label to it.key.toString() },
            selected = { key -> if (key == null) current == null else current?.key?.toString() == key },
            onPick = { key ->
                val chosen = key?.toIntOrNull()?.let { k -> jobs.firstOrNull { it.key == k } }
                ArcadeControls.set(gameId, layout, button, chosen)
                picking.value = null
                tick.intValue++
            },
            onDismiss = { picking.value = null },
            extra = str("arcade.controls.nothing"),
        )
    }
}

/** A row: a name, and what it is set to; [highlight] when the player changed it. */
@Composable
private fun ArcadeValueRow(label: String, value: String, highlight: Boolean, id: String, enabled: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp)
            .height(52.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .then(
                if (enabled) Modifier
                    .clickable { onClick() }
                    .controllerFocusable(controllerId = id, onConfirm = onClick)
                else Modifier
            )
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.weight(1f))
        Text(
            value,
            color = when {
                !enabled -> Color(0xFF888888)
                highlight -> Colors.pasx2_blue
                else -> Color(0xFFCCCCCC)
            },
            fontSize = 15.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.widthIn(max = 260.dp),
        )
    }
}

/** A list to choose one item from ([items]: name, value), plus [extra] (value null) when given. */
@Composable
private fun ArcadePicker(
    title: String,
    layer: String,
    items: List<Pair<String, String>>,
    selected: (String?) -> Boolean,
    onPick: (String?) -> Unit,
    onDismiss: () -> Unit,
    extra: String?,
) {
    // A plain scrolling Column, not a LazyColumn: the pad's navigation only knows composed rows.
    com.armsx2.ui.common.PadModal(key = layer, onDismiss = onDismiss) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 420.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier
                    .padding(20.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.82f).dp),
            ) {
                Text(title, color = MaterialTheme.colorScheme.onSurface, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(8.dp))
                Column(
                    Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState()),
                ) {
                    items.forEachIndexed { index, (name, value) ->
                        StickPickItem(name, selected(value), "$layer.$index") { onPick(value) }
                    }
                    if (extra != null) StickPickItem(extra, selected(null), "$layer.none") { onPick(null) }
                }
                Spacer(Modifier.height(14.dp))
                Row(Modifier.fillMaxWidth()) {
                    Spacer(Modifier.weight(1f))
                    PickerButton(str("action.cancel"), "$layer.cancel", onDismiss)
                }
            }
        }
    }
}
