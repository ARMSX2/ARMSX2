// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.settingshub

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.EN
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.home.LibraryKeyboard

/**
 * Controller-native settings search. Reuses the library's on-screen keyboard (the system IME
 * can't be D-pad-driven) to type a query, shows live results, and jumps to the SPECIFIC control
 * — highlighting + scrolling to it in its tab, not just opening the tab.
 *
 * Driven from MainActivityRuntime.dispatchKeyEvent / fireNavMove while [visible]: the keyboard
 * owns input while up (A = key, X = backspace, Done/B = browse). Once it's dismissed, D-pad/stick
 * moves the result selection, A jumps, Y re-opens the keyboard, B closes. Touch works throughout.
 */
internal object SettingsSearch {
    /** [sections] holds the resolved CollapsibleSection titles that must open for the result
     *  to be visible — the chain from the search entry, resolved like [label]. */
    data class Result(val label: String, val category: SettingsCategory, val sections: List<String> = emptyList())

    val visible = mutableStateOf(false)
    val query = mutableStateOf("")
    val selected = mutableIntStateOf(0)

    /** Latest filtered results, published by the overlay each recomposition for the dispatcher. */
    @Volatile
    var results: List<Result> = emptyList()
    private var onJump: (SettingsCategory, String, List<String>) -> Unit = { _, _, _ -> }

    fun open(onJump: (SettingsCategory, String, List<String>) -> Unit) {
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
        this.onJump = onJump
        query.value = ""
        selected.intValue = 0
        results = emptyList()
        visible.value = true
        LibraryKeyboard.open("", ::setQuery, I18n.get("settings.search.placeholder"))
    }

    fun setQuery(q: String) {
        query.value = q
        selected.intValue = 0
    }

    fun move(delta: Int) {
        val n = results.size
        if (n > 0) selected.intValue = (selected.intValue + delta).coerceIn(0, n - 1)
    }

    fun activate() {
        val r = results.getOrNull(selected.intValue) ?: return
        close()
        onJump(r.category, r.label, r.sections)
    }

    /** Re-open the keyboard to edit the query (Y from result-browse mode). */
    fun reopenKeyboard() {
        LibraryKeyboard.open(query.value, ::setQuery, I18n.get("settings.search.placeholder"))
    }

    fun close() {
        LibraryKeyboard.close()
        visible.value = false
    }
}

@Composable
internal fun SettingsSearchOverlay(scope: BoxScope, gameSpecific: Boolean) {
    if (!SettingsSearch.visible.value) return
    val q = SettingsSearch.query.value.trim()
    I18n.current // subscribe: results relabel live on language switch
    val results = remember(q, I18n.current, gameSpecific) {
        if (q.isEmpty()) {
            emptyList()
        } else {
            SETTINGS_SEARCH_INDEX.asSequence()
                // The General/App tab isn't shown in per-game scope.
                .filter { !(gameSpecific && it.category == SettingsCategory.General) }
                .mapNotNull { e ->
                    val label = if (e.isI18nKey) I18n.get(e.text) else e.text
                    val en = if (e.isI18nKey) (EN[e.text] ?: "") else e.text
                    // Match the localized label AND the English source, so search works in any language.
                    if (label.contains(q, ignoreCase = true) || en.contains(q, ignoreCase = true)) {
                        SettingsSearch.Result(label, e.category, e.sections.map { I18n.get(it) })
                    } else {
                        null
                    }
                }
                .distinctBy { it.label + "\u0000" + it.category }
                .sortedByDescending { it.label.startsWith(q, ignoreCase = true) }
                .take(40)
                .toList()
        }
    }
    SideEffect { SettingsSearch.results = results }
    val sel = SettingsSearch.selected.intValue.coerceIn(0, (results.size - 1).coerceAtLeast(0))
    val listState = rememberLazyListState()
    LaunchedEffect(sel, results.size) {
        if (results.isNotEmpty()) runCatching { listState.animateScrollToItem(sel) }
    }

    // System back / gesture back closes the search instead of leaving the settings screen behind it.
    androidx.activity.compose.BackHandler(enabled = true) { SettingsSearch.close() }

    with(scope) {
        // Own dim so results stay readable whether or not the keyboard is up. Tapping it CLOSES
        // the search: it used to swallow taps, which with an empty query left no way out at all on
        // a touchscreen — no rows to tap and no keyboard once Done was pressed (only a controller's
        // B still worked). Reported as "click Done with nothing searched and you're stuck".
        Box(
            Modifier
                .matchParentSize()
                .background(Color.Black.copy(alpha = 0.55f))
                .clickable(
                    indication = null,
                    interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                ) { SettingsSearch.close() },
        )
        Surface(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .padding(top = 70.dp, start = 10.dp, end = 10.dp)
                .heightIn(max = 300.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            shadowElevation = 14.dp,
        ) {
            if (results.isEmpty()) {
                // An explicit way out, always visible. With no results there are no rows to tap,
                // so without this the panel is a dead end for touch users.
                Column(Modifier.padding(16.dp)) {
                    Text(
                        if (q.isBlank()) str("settings.search.placeholder") else str("settings.search.noResults"),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Surface(
                        onClick = { SettingsSearch.close() },
                        modifier = Modifier.padding(top = 12.dp),
                        shape = RoundedCornerShape(12.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant,
                    ) {
                        Text(
                            str("action.close"),
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                            color = MaterialTheme.colorScheme.onSurface,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.padding(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    itemsIndexed(results) { i, r ->
                        SettingsSearchRow(r, selected = i == sel) {
                            SettingsSearch.selected.intValue = i
                            SettingsSearch.activate()
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SettingsSearchRow(r: SettingsSearch.Result, selected: Boolean, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        shape = RoundedCornerShape(12.dp),
        color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant,
        contentColor = if (selected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 9.dp)) {
            Text(
                r.label,
                fontSize = 16.sp,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                categoryTitle(r.category),
                fontSize = 12.sp,
                color = if (selected) MaterialTheme.colorScheme.onPrimary.copy(alpha = 0.85f)
                else MaterialTheme.colorScheme.primary,
            )
        }
    }
}
