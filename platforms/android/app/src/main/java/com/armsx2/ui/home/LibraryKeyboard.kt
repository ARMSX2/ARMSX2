package com.armsx2.ui.home

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Controller-navigable on-screen keyboard for library search.
 *
 * The system IME can't be driven by a D-pad — Compose consumes D-pad key events for
 * focus traversal before the keyboard ever sees them — so on a handheld the only
 * reliable controller text entry is our own key grid. Touch still uses the normal
 * system keyboard when you tap the search field; this is the CONTROLLER path, opened by
 * A on the Search zone and driven from dispatchKeyEvent while [visible]. The keys are
 * also tappable (52dp tall, weight-filled), so touch works here too.
 */
object LibraryKeyboard {
    val visible = mutableStateOf(false)
    val row = mutableIntStateOf(0)
    val col = mutableIntStateOf(0)

    /** Caps-lock toggle: letter keys emit + render uppercase while true. Sticky until toggled off. */
    val shifted = mutableStateOf(false)

    /** Live buffer, seeded from the current query on open; every edit pushes to onChange. */
    val text = mutableStateOf("")
    private var onChange: (String) -> Unit = {}
    /** Empty-buffer hint; caller-set so the same keyboard serves library + settings search. */
    private val placeholder = mutableStateOf("Search games…")

    // Special keys carry multi-char labels; letter keys are single chars.
    const val SPACE = "space"
    const val BACKSPACE = "⌫"
    const val CLEAR = "clear"
    const val DONE = "done"
    const val SHIFT = "shift"

    val rows: List<List<String>> = listOf(
        listOf("1", "2", "3", "4", "5", "6", "7", "8", "9", "0"),
        listOf("q", "w", "e", "r", "t", "y", "u", "i", "o", "p"),
        listOf("a", "s", "d", "f", "g", "h", "j", "k", "l"),
        listOf(SHIFT, "z", "x", "c", "v", "b", "n", "m"),
        listOf(SPACE, BACKSPACE, CLEAR, DONE),
    )

    /** Use the Android system IME instead of these on-screen keys. Opt-in: the built-in keyboard
     *  stays the default because it is fully gamepad-navigable, whereas the system IME is not (and
     *  Android IMEs never deliver key events to the emulated PS2 pad at all). Switching here rather
     *  than at each call site means the library search and the rename field both follow the
     *  setting with no changes of their own. */
    private const val SystemImeKey = "ui.useSystemKeyboard"
    val useSystemIme = mutableStateOf(false)

    fun setUseSystemIme(on: Boolean) {
        useSystemIme.value = on
        com.armsx2.runtime.MainActivityRuntime.prefs.edit().putBoolean(SystemImeKey, on).apply()
    }

    /** Load the stored preference into [useSystemIme]. Called from open() and from the Settings
     *  row, which can be composed before the keyboard has ever been opened. */
    fun refreshUseSystemIme(): Boolean {
        useSystemIme.value = runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean(SystemImeKey, false)
        }.getOrDefault(false)
        return useSystemIme.value
    }

    fun open(initial: String, onChange: (String) -> Unit, placeholder: String = "Search games…") {
        text.value = initial
        this.onChange = onChange
        this.placeholder.value = placeholder
        // Re-read on every open so the preference needs no wiring into app startup, and so a
        // change made in Settings takes effect the next time the keyboard is summoned.
        refreshUseSystemIme()
        row.intValue = 0
        col.intValue = 0
        shifted.value = false
        visible.value = true
    }

    fun close() { visible.value = false }

    fun move(dx: Int, dy: Int) {
        val br = row.intValue; val bc = col.intValue
        if (dy != 0) {
            row.intValue = (row.intValue + dy).coerceIn(0, rows.size - 1)
        }
        // Clamp the column into the (possibly shorter) target row after any move.
        col.intValue = (col.intValue + dx).coerceIn(0, rows[row.intValue].size - 1)
        if (row.intValue != br || col.intValue != bc)
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
    }

    /** Press the currently-highlighted key. */
    fun press() {
        val key = rows[row.intValue].getOrNull(col.intValue) ?: return
        pressKey(key)
    }

    fun pressKey(key: String) {
        // A tick per key (touch or controller); Done gets the confirm blip.
        com.armsx2.MenuSfx.play(if (key == DONE) com.armsx2.MenuSfx.Event.SELECT else com.armsx2.MenuSfx.Event.NAV)
        if (key == SHIFT) { shifted.value = !shifted.value; return }
        val next = when (key) {
            SPACE -> text.value + " "
            BACKSPACE -> text.value.dropLast(1)
            CLEAR -> ""
            DONE -> { close(); return }
            else -> text.value + (if (shifted.value) key.uppercase() else key)
        }
        text.value = next
        onChange(next)
    }

    /** Convenience hardware shortcut (e.g. X = backspace) from dispatchKeyEvent. */
    fun backspace() = pressKey(BACKSPACE)

    private fun weightOf(key: String): Float = when (key) {
        SPACE -> 4f
        DONE -> 2f
        BACKSPACE, CLEAR, SHIFT -> 1.6f
        else -> 1f
    }

    private fun glyphOf(key: String): String = when (key) {
        SPACE -> "Space"
        BACKSPACE -> "⌫"
        CLEAR -> "Clear"
        DONE -> "Done"
        SHIFT -> "⇧"
        else -> if (shifted.value) key.uppercase() else key
    }

    @Composable
    fun Overlay(scope: BoxScope) {
        if (!visible.value) return
        with(scope) {
        // Dim the library behind, and swallow stray taps so they don't hit covers.
        Box(Modifier.matchParentSize().background(Color.Black.copy(alpha = 0.5f)).clickable(enabled = false) {})
        Surface(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(10.dp),
            shape = RoundedCornerShape(22.dp),
            color = MaterialTheme.colorScheme.surface,
            shadowElevation = 18.dp,
        ) {
            Column(
                Modifier.padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                if (useSystemIme.value) {
                    SystemImeField()
                    return@Column
                }
                Text(
                    text = text.value.ifEmpty { placeholder.value },
                    color = if (text.value.isEmpty()) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.onSurface,
                    fontSize = 18.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 2.dp),
                )
                rows.forEachIndexed { r, keys ->
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(5.dp),
                    ) {
                        keys.forEachIndexed { c, key ->
                            KeyCap(
                                label = glyphOf(key),
                                selected = (row.intValue == r && col.intValue == c) ||
                                    (key == SHIFT && shifted.value),
                                weight = weightOf(key),
                                onClick = {
                                    row.intValue = r
                                    col.intValue = c
                                    pressKey(key)
                                },
                            )
                        }
                    }
                }
            }
        }
        }
    }

    /**
     * The system-IME alternative to the key grid. Rendered INLINE in the library's Box, never in a
     * Dialog: a modal Dialog takes its own focused window and swallows gamepad keys, so it would
     * strand anyone who toggles this on and then reaches for a controller.
     */
    @Composable
    private fun SystemImeField() {
        val focus = remember { FocusRequester() }
        val ime = LocalSoftwareKeyboardController.current
        OutlinedTextField(
            value = text.value,
            onValueChange = {
                // Strip newlines so a multiline IME can't inject them into a search query.
                val next = it.replace("\n", "")
                text.value = next
                onChange(next)
            },
            placeholder = { Text(placeholder.value) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { ime?.hide(); close() }),
            modifier = Modifier.fillMaxWidth().focusRequester(focus),
        )
        // Summon the IME with the field, and make sure it goes away with the overlay — otherwise a
        // dismissed keyboard leaves the system IME covering the library.
        LaunchedEffect(Unit) {
            focus.requestFocus()
            ime?.show()
        }
        DisposableEffect(Unit) { onDispose { ime?.hide() } }
    }

    @Composable
    private fun RowScope.KeyCap(label: String, selected: Boolean, weight: Float, onClick: () -> Unit) {
        Box(
            modifier = Modifier
                .weight(weight)
                .height(52.dp)
                .clip(RoundedCornerShape(11.dp))
                .background(
                    if (selected) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.surfaceVariant,
                )
                .clickable(onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = label,
                color = if (selected) MaterialTheme.colorScheme.onPrimary
                else MaterialTheme.colorScheme.onSurface,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                fontSize = if (label.length > 1) 14.sp else 18.sp,
                maxLines = 1,
            )
        }
    }
}
