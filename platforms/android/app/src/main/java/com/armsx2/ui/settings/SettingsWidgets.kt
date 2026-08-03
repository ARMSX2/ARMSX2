package com.armsx2.ui.settings

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.ui.settings.SettingsControllerNav.move
import com.armsx2.i18n.str
import kotlin.math.abs
import kotlin.math.roundToInt
import android.view.KeyEvent as AndroidKeyEvent
import androidx.core.content.edit

/**
 * Shared widget primitives for the in-game settings tabs. Style matches
 * InGameOverlay's MenuRow — left-anchored alpha aura on the row bg,
 * white text, transparent borders. Each row is 24dp tall to keep the
 * tab content fitting in the same 75% screen height the Playing-Now
 * tab uses.
 */

private val focusBlue = Color(0xFF3DA5FF)

val LocalSettingsScrollState = staticCompositionLocalOf<ScrollState?> { null }

/** The exclusive input layer the surrounding content belongs to — null on a base screen, a
 *  modal's key inside PadModal's host, which is the only thing that ever provides it.
 *
 *  Ambient rather than a parameter on [controllerFocusable] because every shared widget has to
 *  layer correctly the moment it is placed inside a modal, and a per-widget argument is a rule
 *  forty call sites have to remember. Position in the tree is the one spelling of "which layer
 *  am I in" that cannot be got wrong. */
internal val LocalNavLayer = staticCompositionLocalOf<String?> { null }

@Composable
fun settingsScrollState(): ScrollState = LocalSettingsScrollState.current ?: remember { ScrollState(0) }

/** Horizontal background gradient that matches the divider direction. */
@Composable
internal fun rowAura() = Brush.horizontalGradient(
    listOf(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.055f), Color.Transparent),
)

internal object SettingsControllerNav {
    private data class Item(
        val id: String,
        val onConfirm: (() -> Unit)?,
        val onLeft: (() -> Unit)?,
        val onRight: (() -> Unit)?,
        val layer: String? = null,
    )

    /** One entry per modal currently claiming input. [restoreId] is the selection that was
     *  live when it opened, so closing it can put the user back where they were. */
    private class Layer(val key: String, val restoreId: String?)

    // Exclusive input layers. Only items registered with the TOP layer participate in nav, so
    // a modal's selection cannot "escape" into the screen still composed behind its scrim.
    //
    // A stack rather than the single slot this started as, for two reasons. Modals nest (the
    // library's exit confirm is raised from inside the overflow panel), and the save-the-
    // previous-value/restore-on-close idiom that a single slot forces is wrong whenever two
    // sibling subtrees dispose in the order Compose happens to pick: the survivor is left
    // holding a layer that is no longer active, so it looks fine and answers nothing.
    //
    // Written only by PadModal, via pushLayer/popLayer. Everything else reads.
    private val layers = mutableStateListOf<Layer>()

    /** The layer that currently owns input; null when the base screen does. */
    val activeLayer: String? get() = layers.lastOrNull()?.key

    // An UNREGISTERED id is in no layer at all. Spelling that out matters: the map
    // lookup yields null for both "not registered" and "registered at the base
    // layer", so the old one-line form answered TRUE for an unknown id whenever no
    // layer was active — which is exactly the case the callers below have to
    // distinguish.
    private fun inActiveLayer(id: String): Boolean {
        val item = registry[id] ?: return false
        return item.layer == activeLayer
    }

    /** Claim [key] as the exclusive input layer and park the outside selection — that row is
     *  un-navigable now, and a ring left lit on it reads as stuck. */
    fun pushLayer(key: String) {
        layers.add(Layer(key, selectedId.value))
        selectedId.value = null
        selectedIndex.intValue = -1
        scrollVelocity.floatValue = 0f
    }

    /** Release [key]. Removed BY KEY, never "drop the top" — see the note on [layers].
     *  Restores the selection the layer interrupted, so closing a row's info panel returns
     *  you to that row rather than to the top of the pane, which reads as a scroll bug and is
     *  really a nav bug. */
    fun popLayer(key: String) {
        val index = layers.indexOfLast { it.key == key }
        if (index < 0) return
        val gone = layers.removeAt(index)
        // Only the layer that was actually on top hands the selection back; a layer buried
        // under a still-open modal must not disturb what that modal has focused.
        if (index != layers.size) return
        selectedId.value = gone.restoreId?.takeIf { inActiveLayer(it) }
        selectedIndex.intValue = orderedIds().indexOf(selectedId.value)
        scrollVelocity.floatValue = 0f
    }

    private var scopeKey: String = ""
    // Persistent registry keyed by row id. Each row UPSERTS its latest closures
    // on every composition (via SideEffect in controllerFocusable) and removes
    // itself on dispose. This is the fix for adjust skipping / getting stuck:
    // the old begin/register/end list assumed every row re-registers on every
    // recomposition, but when you change ONE value only that row recomposes —
    // RootTabs (which calls begin/end) does not — so the list kept a STALE
    // closure capturing the old index, and the next press either no-op'd
    // ("stuck") or double-stepped ("skip"). A registry keyed by id always holds
    // the freshest closure regardless of which rows recomposed.
    private val registry = LinkedHashMap<String, Item>()
    // On-screen position (y, x) per id, fed by controllerFocusable's
    // onGloballyPositioned. Nav order follows this (top→bottom, left→right) so it
    // matches what the user sees even when a section appears later than others
    // (e.g. the memcard "New Card" form, which registers after the card list but
    // is drawn above it).
    private val positions = HashMap<String, Pair<Float, Float>>()
    private val selectedId = mutableStateOf<String?>(null)
    val selectedIndex = mutableIntStateOf(-1)
    val scrollVelocity = mutableFloatStateOf(0f)

    /** The id of the currently highlighted control (null if none). Screens read
     *  this to react to WHICH control is focused — e.g. the settings hub snaps its
     *  scroll to the very top when a category chip regains focus. */
    fun currentSelectedId(): String? = selectedId.value

    fun setPosition(id: String, x: Float, y: Float) {
        positions[id] = y to x
    }

    private fun orderedIds(): List<String> {
        // Sort by measured y (then x). Items that haven't reported a position YET — an
        // off-screen row before its first layout, or a freshly-shown one — INHERIT the
        // previous registered item's y instead of sinking to Float.MAX_VALUE at the bottom.
        // Registration order (LinkedHashMap) mirrors visual order, so an unmeasured row stays
        // navigable in place. Without this, Down from a measured row skipped every unmeasured
        // neighbour and jumped to the next measured item (e.g. past a long driver list to the
        // next dropdown header). A stable insertion-order tiebreak keeps same-y rows in order.
        var lastY = 0f
        val effY = HashMap<String, Float>(registry.size)
        val idx = HashMap<String, Int>(registry.size)
        var i = 0
        val layerIds = registry.keys.filter { inActiveLayer(it) }
        for (id in layerIds) {
            positions[id]?.first?.let { lastY = it }
            effY[id] = lastY
            idx[id] = i++
        }
        return layerIds.sortedWith(
            compareBy(
                { effY[it] ?: 0f },
                { positions[it]?.second ?: 0f },
                { idx[it] ?: 0 },
            ),
        )
    }

    fun begin(scope: String) {
        if (scopeKey != scope) {
            // Switched tab: drop the old selection. The new tab's rows register
            // during this composition and stale ids are pruned by onDispose.
            scopeKey = scope
            selectedId.value = null
            selectedIndex.intValue = -1
        }
    }

    fun register(
        id: String,
        onConfirm: (() -> Unit)? = null,
        onLeft: (() -> Unit)? = null,
        onRight: (() -> Unit)? = null,
        layer: String? = null,
    ) {
        // Upsert — replacing an existing key keeps its insertion order (stable
        // visual order) while refreshing the closures to the current value.
        registry[id] = Item(id, onConfirm, onLeft, onRight, layer)
        if (selectedId.value == id)
            selectedIndex.intValue = orderedIds().indexOf(id)
    }

    fun unregister(id: String) {
        registry.remove(id)
        positions.remove(id)
        if (selectedId.value == id)
            selectedId.value = orderedIds().firstOrNull()
        selectedIndex.intValue = orderedIds().indexOf(selectedId.value)
    }

    fun end() {
        // Keep the highlighted index in sync with the current order.
        selectedIndex.intValue = orderedIds().indexOf(selectedId.value)
    }

    fun clearSelection() {
        selectedId.value = null
        selectedIndex.intValue = -1
        scrollVelocity.floatValue = 0f
    }

    fun hasItems(): Boolean = registry.keys.any { inActiveLayer(it) }

    /** Highlight + scroll to the row whose id derives from [label] — used by settings search
     *  to jump to a specific control after switching tabs. The shared row widgets register
     *  label-based ids (toggle:/segmented:/segmented-grid:/slider:<label>[:hash]); sliders
     *  append a composition hash, so those are matched by prefix. Returns true once a row
     *  matched (the target tab must already be composed — retry until it is). */
    fun selectByLabel(label: String): Boolean {
        val ids = orderedIds()
        val exact = setOf("toggle:$label", "segmented:$label", "segmented-grid:$label", "slider:$label")
        val id = ids.firstOrNull { it in exact }
            ?: ids.firstOrNull { it.startsWith("slider:$label:") }
            ?: return false
        selectedId.value = id
        selectedIndex.intValue = ids.indexOf(id)
        return true
    }

    /** Focus a specific control by id. Returns false when it hasn't registered yet, so a caller
     *  that composes its controls in the same frame can retry.
     *
     *  Exists for modal layers (see ConfirmOverlay): the home screen decides whether the D-pad
     *  belongs to this registry or to the cover grid by asking [hasSelection], so a modal that
     *  merely claims [activeLayer] without also taking a selection would lose its first press to
     *  the grid behind the scrim. */
    fun selectById(id: String): Boolean {
        val ids = orderedIds()
        val index = ids.indexOf(id)
        if (index < 0) return false
        selectedId.value = id
        selectedIndex.intValue = index
        return true
    }

    /** Highlight the first item of the active layer, silently. The fallback a modal takes when
     *  it names no initial focus, or names one that never registered — so a modal can only be
     *  left with nothing selected if it contains nothing focusable at all. Silent because this
     *  fires as the modal appears, and a nav click there sounds like a press the user didn't
     *  make. */
    fun selectFirstInLayer(): Boolean = selectEdgeOfLayer(first = true, sfx = false)

    /** True when a registered item in the active layer is currently highlighted —
     *  i.e. the registry "lane" owns D-pad focus (used by the home screen to split
     *  input between the cover grid and the toolbar/recents lane). */
    fun hasSelection(): Boolean =
        selectedId.value?.let { registry.containsKey(it) && inActiveLayer(it) } == true

    /** Number of registered focusable items in the active layer. */
    fun count(): Int = registry.keys.count { inActiveLayer(it) }

    fun move(delta: Int): Boolean {
        val ids = orderedIds()
        if (ids.isEmpty() || delta == 0) return false
        val cur = ids.indexOf(selectedId.value)
        val next = if (cur < 0) {
            if (delta < 0) ids.lastIndex else 0
        } else {
            (cur + delta).coerceIn(0, ids.lastIndex)
        }
        selectedId.value = ids[next]
        selectedIndex.intValue = next
        if (next != cur) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        return true
    }

    /** Highlight the first (or last) item of the ACTIVE LAYER in visual order. Used when a
     *  direction arrives with nothing selected yet — travelling down or right lands on the
     *  first item, up or left on the last, which is what makes the very first press after a
     *  surface appears feel like it entered from the edge you pushed from. */
    private fun selectEdgeOfLayer(first: Boolean, sfx: Boolean = true): Boolean {
        val ids = orderedIds()
        if (ids.isEmpty()) return false
        val next = if (first) 0 else ids.lastIndex
        val changed = selectedId.value != ids[next]
        selectedId.value = ids[next]
        selectedIndex.intValue = next
        if (changed && sfx) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        return true
    }

    /** 2D spatial navigation over the captured on-screen positions: move to the nearest
     *  focusable in the (dx, dy) direction. Rows of side-by-side controls need this — a flat
     *  1D step makes a two-button row feel stuck on its first button, because every direction
     *  just walks the list. Vertical steps exactly one row and then lands on the item in that
     *  row nearest the current x; horizontal takes the nearest item in that direction on
     *  (roughly) the same row. Confined to the active layer. positions[id] = (y, x). */
    fun moveSpatial(dx: Int, dy: Int): Boolean {
        if (dx == 0 && dy == 0) return false
        val curId = selectedId.value
        val cur = curId?.let { positions[it] }
        if (cur == null) return selectEdgeOfLayer(first = dx > 0 || dy > 0)
        val cy = cur.first
        val cx = cur.second
        val rowTol = 28f // px; items within this |dy| count as the same visual row

        // Candidates come from orderedIds(), NOT from the raw positions map. Two things
        // ride on that. It confines the move to the ACTIVE LAYER — this function used to
        // scan every measured item on screen, so a modal's selection could step straight
        // out through its own scrim onto a row behind it, defeating the one mechanism
        // built to prevent that. And it makes the scan deterministic: positions is a
        // HashMap, so the old iteration order was arbitrary, and any tie between two
        // equally-good candidates was settled differently from run to run.
        val candidates = orderedIds().mapNotNull { id ->
            if (id == curId) null else positions[id]?.let { id to it }
        }

        val target: String? = if (dy != 0) {
            // Vertical: step exactly ONE row in the travel direction, then land on the
            // item in that row whose x is closest to the current x. Row-based (not a
            // per-item distance score) so Up and Down are symmetric and can never skip
            // a row or pick a far diagonal item — the old score could make Up land
            // nowhere useful on the card grid.
            var rowY: Float? = null
            for ((_, p) in candidates) {
                val py = p.first
                if (dy < 0 && py >= cy - rowTol) continue   // need a row strictly above
                if (dy > 0 && py <= cy + rowTol) continue   // need a row strictly below
                rowY = when {
                    rowY == null -> py
                    dy < 0 -> if (py > rowY) py else rowY  // up: highest row still above
                    else -> if (py < rowY) py else rowY    // down: lowest row still below
                }
            }
            val ry = rowY
            if (ry == null) null else {
                var bestId: String? = null
                var bestDx = Float.MAX_VALUE
                for ((id, p) in candidates) {
                    if (abs(p.first - ry) > rowTol) continue
                    val d = abs(p.second - cx)
                    if (d < bestDx) { bestDx = d; bestId = id }
                }
                bestId
            }
        } else {
            // Horizontal: nearest focusable in the dx direction on (roughly) the same row.
            var bestId: String? = null
            var bestScore = Float.MAX_VALUE
            for ((id, p) in candidates) {
                val ddx = p.second - cx
                val ddy = p.first - cy
                val inDir = if (dx > 0) ddx > 1f && abs(ddy) < rowTol
                            else ddx < -1f && abs(ddy) < rowTol
                if (!inDir) continue
                val score = abs(ddx) + abs(ddy) * 4f
                if (score < bestScore) { bestScore = score; bestId = id }
            }
            bestId
        }

        if (target == null) return false
        selectedId.value = target
        selectedIndex.intValue = orderedIds().indexOf(target)
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        return true
    }

    fun adjust(delta: Int): Boolean {
        if (delta == 0) return false
        val item = selectedItem() ?: return false
        val action = if (delta < 0) item.onLeft else item.onRight
        action ?: return false
        action.invoke()
        return true
    }

    fun confirm(): Boolean {
        val item = selectedItem() ?: return false
        val before = com.armsx2.MenuSfx.lastPlayedAt()
        item.onConfirm?.invoke() ?: return false
        // If the invoked action was SILENT (didn't already toggle/navigate/etc.), add a generic
        // select blip. This covers custom controls — RetroAchievements, memory-card, texture-pack,
        // BIOS-location, patches/cheats — that route through the nav registry but aren't one of the
        // self-sounding standard widgets. Anything that made its own sound is left alone (no double).
        if (com.armsx2.MenuSfx.lastPlayedAt() == before) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
        return true
    }

    fun setScrollVelocity(velocity: Float): Boolean {
        scrollVelocity.floatValue = if (abs(velocity) > 0.08f) velocity.coerceIn(-1f, 1f) else 0f
        return true
    }

    /** Whether this control draws the focus ring. Layer-gated, so a row on a screen sitting
     *  behind a modal goes dark for as long as the modal owns input instead of leaving a
     *  second ring lit on top of the modal's own — two rings on screen at once, with only
     *  one of them answering the pad, is the exact symptom this whole mechanism exists to
     *  prevent. */
    fun isSelected(id: String): Boolean = selectedId.value == id && inActiveLayer(id)

    /** The item a value-adjust or an activation should act on. Falls back to the first item
     *  of the active layer when the selection is missing or belongs to a suspended layer —
     *  the fallback is what stops a stale selection from firing a control the user can no
     *  longer see, which is how Left/Right and A used to reach through a scrim. */
    private fun selectedItem(): Item? {
        val ids = orderedIds()
        if (ids.isEmpty()) return null
        val id = selectedId.value
        if (id == null || !inActiveLayer(id)) {
            val first = ids.first()
            selectedId.value = first
            selectedIndex.intValue = 0
            return registry[first]
        }
        return registry[id]
    }
}

@Composable
internal fun ControllerAutoScroll(scroll: ScrollState) {
    val density = LocalDensity.current
    // Keeping the selected row on-screen is handled per-row by bringIntoView()
    // in controllerFocusable, which uses the ACTUAL measured layout. The old
    // estimate here (selectedIndex * fixed 44dp row height) over/under-scrolled
    // because rows with descriptions are taller, so it "fought" the selection.
    // This loop only drives the optional right-stick free scroll.
    LaunchedEffect(scroll) {
        var lastFrame = withFrameNanos { it }
        while (true) {
            val frame = withFrameNanos { it }
            val dt = ((frame - lastFrame).coerceAtMost(50_000_000L)).toFloat() / 1_000_000_000f
            lastFrame = frame
            val velocity = SettingsControllerNav.scrollVelocity.floatValue
            if (abs(velocity) > 0.08f && scroll.maxValue > 0) {
                val pxPerSecond = with(density) { 1500.dp.toPx() }
                scroll.scrollBy(velocity * pxPerSecond * dt)
            }
        }
    }
}

internal fun Modifier.controllerFocusable(
    controllerId: String? = null,
    shape: RoundedCornerShape = RoundedCornerShape(16.dp),
    onConfirm: (() -> Unit)? = null,
    onLeft: (() -> Unit)? = null,
    onRight: (() -> Unit)? = null,
): Modifier = composed {
    // Which layer this control belongs to is decided by WHERE IT SITS, not by an argument the
    // call site has to remember to pass. That is what lets an unmodified ToggleRow or slider be
    // dropped inside a modal and layer correctly with no plumbing at all.
    val navLayer = LocalNavLayer.current
    var focused by remember { mutableStateOf(false) }
    val bringIntoView = remember { BringIntoViewRequester() }
    if (controllerId != null) {
        // Upsert the latest closures after every (re)composition so adjust /
        // confirm always run against the CURRENT value (SideEffect runs on each
        // successful recomposition, including the partial ones where only this
        // row re-runs). Remove on dispose so other tabs don't inherit the row.
        SideEffect {
            SettingsControllerNav.register(
                id = controllerId,
                onConfirm = onConfirm,
                onLeft = onLeft,
                onRight = onRight,
                layer = navLayer,
            )
        }
        DisposableEffect(controllerId) {
            onDispose { SettingsControllerNav.unregister(controllerId) }
        }
    }
    val selected = controllerId != null && SettingsControllerNav.isSelected(controllerId)
    if (controllerId != null) {
        // Scroll the selected row just into view using its real measured bounds.
        LaunchedEffect(selected) {
            if (selected) runCatching { bringIntoView.bringIntoView() }
        }
    }
    this
        .bringIntoViewRequester(bringIntoView)
        .then(
            if (controllerId != null)
                Modifier.onGloballyPositioned {
                    val p = it.positionInRoot()
                    SettingsControllerNav.setPosition(controllerId, p.x, p.y)
                }
            else Modifier,
        )
        .onFocusChanged { focused = it.isFocused }
        .onPreviewKeyEvent { event ->
            if (event.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
            when (event.key.nativeKeyCode) {
                AndroidKeyEvent.KEYCODE_DPAD_CENTER,
                AndroidKeyEvent.KEYCODE_ENTER,
                AndroidKeyEvent.KEYCODE_NUMPAD_ENTER -> {
                    onConfirm?.invoke()
                    onConfirm != null
                }
                // Left/Right adjust the value in place (stepper −/+, toggle off/on,
                // dropdown prev/next) when this row has an adjust handler. Consumed
                // only when a handler exists, so plain nav rows still let Left/Right
                // move focus. This is what makes sliders/steppers adjustable with a
                // controller when the row is driven by Compose focus (the settings
                // hub) rather than the registry's own adjust() path (the memcard
                // dialog, which consumes these keys upstream in the router).
                AndroidKeyEvent.KEYCODE_DPAD_LEFT -> {
                    onLeft?.invoke()
                    onLeft != null
                }
                AndroidKeyEvent.KEYCODE_DPAD_RIGHT -> {
                    onRight?.invoke()
                    onRight != null
                }
                else -> false
            }
        }
        .then(
            if (focused || selected) {
                // Driver-safe selection ring: solid inner + translucent outer border. We
                // deliberately avoid Modifier.shadow with a custom ambient/spot color here —
                // Adreno / Mali / Turnip drivers frequently ignore the tint and render the
                // elevation shadow as an ugly opaque BLACK box behind the row (visible only
                // under controller nav, which is what sets `selected`; touch never does).
                // Borders render identically on every driver.
                Modifier
                    .border(3.dp, focusBlue.copy(alpha = 0.30f), shape)
                    .border(1.5.dp, focusBlue, shape)
            } else {
                Modifier
            }
        )
        .focusable()
}

/** Thin horizontal divider with left-anchored fade. Mirrors InGameOverlay's
 *  MenuDivider so settings rows tie visually into the existing overlay. */
@Composable
fun SettingsDivider() {
    Box(
        Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(
                Brush.horizontalGradient(
                    listOf(MaterialTheme.colorScheme.outline.copy(alpha = 0.55f), Color.Transparent)
                )
            ),
    )
}

/** Collapsible settings section: a tappable header (▸ collapsed / ▾ expanded) that
 *  shows or hides its content. Controller-focusable so a gamepad can open it. Shared
 *  by the Fixes / Pad / Performance / Renderer tabs to de-bloat long settings lists.
 *  [initiallyExpanded] lets a tab open its most-used section by default.
 *
 *  This is the de-bloating that the settings screens were built around, and it had stopped
 *  happening: the header rendered as plain text and `content()` was called unconditionally, with
 *  `initiallyExpanded` marked UNUSED_PARAMETER. Every tab was therefore one flat list of every
 *  option it owns — the reason the settings became unusable in landscape. Restoring it shortens
 *  Pad / Renderer / Performance / Advanced all at once.
 *
 *  State is [rememberSaveable] so a rotation does not re-collapse what the user just opened —
 *  landscape being exactly where the long lists hurt most. */
@Composable
fun CollapsibleSection(
    title: String,
    initiallyExpanded: Boolean = false,
    content: @Composable () -> Unit,
) {
    var expanded by androidx.compose.runtime.saveable.rememberSaveable(title) {
        mutableStateOf(initiallyExpanded)
    }
    val toggle = {
        expanded = !expanded
        com.armsx2.MenuSfx.play(
            if (expanded) com.armsx2.MenuSfx.Event.TOGGLE_ON else com.armsx2.MenuSfx.Event.TOGGLE_OFF
        )
    }
    Spacer(Modifier.height(12.dp))
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable("section.$title", onConfirm = toggle)
            .clickable(onClick = toggle)
            .padding(horizontal = 8.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            title,
            color = MaterialTheme.colorScheme.primary,
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.weight(1f),
        )
        Text(
            if (expanded) "▾" else "▸",
            color = MaterialTheme.colorScheme.primary,
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
        )
    }
    // Collapsed content is not composed at all, so its rows also drop out of the controller-focus
    // registry — a pad cannot land on a setting the user cannot see.
    if (expanded) content()
}

@Composable
fun HelpText(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontSize = 14.sp,
        lineHeight = 20.sp,
        modifier = modifier.padding(horizontal = 8.dp, vertical = 8.dp),
    )
}

/** Toggle row — label on left, status text on right. Tapping anywhere
 *  on the row flips the value via [onChange]. */
@Composable
fun ToggleRow(
    label: String,
    value: Boolean,
    description: String? = null,
    onChange: (Boolean) -> Unit,
) {
    // Menu SFX: a distinct on/off blip on every flip. Touch, the switch, and the controller's
    // confirm/left/right all route through this, so one wrapper covers every input path; the
    // left/right guards below keep it silent when nothing actually changes.
    val emit: (Boolean) -> Unit = {
        com.armsx2.MenuSfx.play(if (it) com.armsx2.MenuSfx.Event.TOGGLE_ON else com.armsx2.MenuSfx.Event.TOGGLE_OFF)
        onChange(it)
    }
    Surface(
        onClick = { emit(!value) },
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .controllerFocusable(
                controllerId = "toggle:$label",
                onConfirm = { emit(!value) },
                onLeft = { if (value) emit(false) },
                onRight = { if (!value) emit(true) },
            ),
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
        border = androidx.compose.foundation.BorderStroke(
            1.dp,
            MaterialTheme.colorScheme.outline.copy(alpha = 0.46f),
        ),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = if (description == null) 64.dp else 78.dp)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 18.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    if (description != null) InfoHint(label, description)
                }
                if (description != null) {
                    Spacer(Modifier.height(3.dp))
                    Text(
                        description,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        fontSize = 14.sp,
                        lineHeight = 19.sp,
                        maxLines = 3,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Spacer(Modifier.width(12.dp))
            Switch(checked = value, onCheckedChange = emit)
        }
    }
}

/** Integer slider row — label + current value on top line, custom slim
 *  slider below. Replaces Material3's chunky default with a Canvas-drawn
 *  slider that matches the overlay's thin-line aesthetic: 3dp track,
 *  tick dots at each discrete step, 5dp thumb with a soft halo. Drag
 *  and tap-to-position both update the value.
 *
 *  [onReset], when non-null, puts a Reset chip on the row and wires the controller's
 *  confirm button to it. Pass null when the value already IS the default — "is there
 *  something to reset" is the caller's question to answer, and it's the same question as
 *  "should the chip show", so the nullability carries both rather than making callers keep
 *  a separate flag in sync. Confirm is otherwise dead on a slider row (left/right do the
 *  adjusting), so this costs the controller no extra focus stops. */
@Composable
fun IntSliderRow(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    description: String? = null,
    valueFormatter: (Int) -> String = { it.toString() },
    onReset: (() -> Unit)? = null,
    onChange: (Int) -> Unit,
) {
    // Include the call-site composite-key hash so two sliders that happen to share a
    // label (e.g. the OSD-scale and border-scale rows both labelled "UI Size") get
    // DISTINCT registry ids — otherwise one overwrites the other and the controller
    // skips right over it.
    val sliderId = "slider:$label:${androidx.compose.runtime.currentCompositeKeyHash}"
    // Menu SFX: a soft tick as the value changes (throttled in MenuSfx so a touch drag ticks
    // instead of buzzing) and the reset blip on Reset. Both cover touch and controller — the
    // DiscreteSlider drag / reset chip and the controller's left/right/confirm route through here.
    val onChangeSfx: (Int) -> Unit = { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SLIDER); onChange(it) }
    val onResetSfx: (() -> Unit)? = onReset?.let { r -> { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.RESET); r() } }
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .controllerFocusable(
                controllerId = sliderId,
                onConfirm = onResetSfx,
                onLeft = { onChangeSfx((value - 1).coerceAtLeast(min)) },
                onRight = { onChangeSfx((value + 1).coerceAtMost(max)) },
            ),
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
        border = androidx.compose.foundation.BorderStroke(
            1.dp,
            MaterialTheme.colorScheme.outline.copy(alpha = 0.46f),
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = if (description == null) 76.dp else 94.dp)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 18.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                        if (description != null) InfoHint(label, description)
                    }
                    if (description != null) {
                        Spacer(Modifier.height(3.dp))
                        Text(
                            description,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 14.sp,
                            lineHeight = 19.sp,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Text(
                    valueFormatter(value),
                    color = MaterialTheme.colorScheme.primary,
                    fontSize = 17.sp,
                    fontWeight = FontWeight.Bold,
                )
                if (onReset != null) {
                    Spacer(Modifier.width(10.dp))
                    // Its own Surface rather than a registry row: the controller reaches
                    // this through the slider's confirm, so a second focus stop per
                    // modified parameter would only pad the D-pad walk.
                    Surface(
                        onClick = onResetSfx ?: onReset,
                        shape = RoundedCornerShape(12.dp),
                        color = MaterialTheme.colorScheme.primary.copy(alpha = 0.16f),
                    ) {
                        Text(
                            str("action.reset"),
                            color = MaterialTheme.colorScheme.primary,
                            fontSize = 13.sp,
                            fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                        )
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            DiscreteSlider(
                value = value,
                min = min,
                max = max,
                onChange = onChangeSfx,
            )
        }
    }
}

/** Custom slim discrete slider. Canvas-drawn so we don't have to fight
 *  Material's default touch target / thumb sizing. The thumb sits at the
 *  current step, tick dots mark every step in the range, and the active
 *  track fills from min to the current value in PS2 blue. */
@Composable
private fun DiscreteSlider(
    value: Int,
    min: Int,
    max: Int,
    onChange: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val steps = (max - min).coerceAtLeast(1)
    val frac = ((value - min).toFloat() / steps.toFloat()).coerceIn(0f, 1f)
    // pointerInput is keyed only on (min, max), so its gesture coroutine captures
    // the onChange lambda from first composition and never restarts. That stale
    // lambda also captures a stale Settings snapshot, so editing one slider after
    // another reverted the first (the EE Cycle Rate/Skip bug). Route every gesture
    // through the always-current lambda instead.
    val latestOnChange by rememberUpdatedState(onChange)
    val activeColor = MaterialTheme.colorScheme.primary
    val inactiveTrackColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f)
    val inactiveTickColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.35f)
    val thumbHighlight = MaterialTheme.colorScheme.onPrimary

    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(28.dp)
            .pointerInput(min, max) {
                // -- Approach C: axis-arbitrated gesture, built from first principles on
                // awaitPointerEvent so the parent verticalScroll and this slider never
                // both act on the same drag.
                //
                // The slider lives inside a Column with Modifier.verticalScroll. That
                // scroll container installs its OWN pointer handler that, once it detects
                // vertical touch-slop, starts consuming the drag (marking each change as
                // consumed) to move the list. Our handler must therefore:
                //   1. Never mutate the value on a bare touch-down (the OLD bug: down
                //      immediately jogged the slider even when the user meant to scroll).
                //   2. Wait past touch-slop and decide by DOMINANT AXIS:
                //        - vertical wins  -> do nothing, leave the change UNconsumed so
                //          the parent verticalScroll takes over and scrolls the list.
                //        - horizontal wins -> claim the pointer (consume) and drive the
                //          value, following the finger live until lift.
                //   3. Stay ONE awaitEachGesture so tap + drag share the same pointer and
                //      a drag doesn't stall after its first jump (the OTHER old bug).
                //   4. Route every value write through latestOnChange (never the captured
                //      onChange), so editing this slider can't revert a sibling.
                val edgePx = 6.dp.toPx()
                fun frac(x: Float): Float {
                    val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
                    return ((x - edgePx) / usable).coerceIn(0f, 1f)
                }
                fun commit(x: Float) {
                    latestOnChange(min + (frac(x) * steps).roundToInt())
                }
                val slop = viewConfiguration.touchSlop

                awaitEachGesture {
                    // awaitFirstDown(requireUnconsumed = false): suspends until the first
                    // pointer presses inside our bounds, returning that PointerInputChange.
                    // We do NOT consume it and we do NOT touch the value yet — a touch-down
                    // that turns into a vertical scroll must leave the slider untouched.
                    val down = awaitFirstDown(requireUnconsumed = false)
                    val pointerId: PointerId = down.id
                    var totalDx = 0f
                    var totalDy = 0f
                    var mode = 0 // 0 = undecided, 1 = horizontal (ours), 2 = vertical (parent's)

                    // Accumulate motion until we cross touch-slop, then lock the axis.
                    // Pass ordering is the whole reason this works: the slider is a
                    // DESCENDANT of the verticalScroll node, and on PointerEventPass.Main
                    // events dispatch child -> ancestor. So on Main WE (the child) see each
                    // move BEFORE verticalScroll does. If we lock horizontal and consume,
                    // the parent never gets an unconsumed move and cannot scroll. If we lock
                    // vertical and DON'T consume, verticalScroll — running right after us on
                    // the same Main event — receives the unconsumed move and scrolls. The
                    // isConsumed early-out below is a belt-and-suspenders guard for the rare
                    // case some other node consumed first; it is not the primary mechanism.
                    while (mode == 0) {
                        val event = awaitPointerEvent(PointerEventPass.Main)
                        val change = event.changes.firstOrNull { it.id == pointerId } ?: break
                        if (!change.pressed) break // lifted before slop -> treat as tap below

                        // If an ancestor already consumed this change (e.g. the parent's
                        // vertical-scroll handler locked the drag), concede: vertical wins,
                        // stop arbitrating, and never write the value.
                        if (change.isConsumed) { mode = 2; break }

                        val d = change.positionChange() // per-event delta in local px
                        totalDx += d.x
                        totalDy += d.y
                        if (abs(totalDx) >= slop || abs(totalDy) >= slop) {
                            // Dominant-axis decision at the moment slop is first crossed.
                            mode = if (abs(totalDx) >= abs(totalDy)) 1 else 2
                        }
                    }

                    when (mode) {
                        1 -> {
                            // Horizontal drag is ours. Seed the value from the current finger
                            // position, then follow it live. Consume each change so the parent
                            // verticalScroll can't also steal this gesture.
                            var pos = down.position.x + totalDx
                            commit(pos)
                            var active = true
                            while (active) {
                                val event = awaitPointerEvent(PointerEventPass.Main)
                                val change = event.changes.firstOrNull { it.id == pointerId }
                                if (change == null || !change.pressed) { active = false }
                                else {
                                    pos = change.position.x
                                    commit(pos)
                                    change.consume() // claim it; blocks parent scroll
                                }
                            }
                        }
                        2 -> {
                            // Vertical won (or the parent already claimed it). Do nothing:
                            // leave every change UNconsumed so Modifier.verticalScroll drives
                            // the list. The slider value is never modified.
                        }
                        else -> {
                            // mode == 0: the arbitration loop exited via the !change.pressed
                            // break (the pointer lifted) before either axis crossed slop. That
                            // is exactly a clean tap. Because total motion stayed under
                            // touch-slop it cannot have been the start of a scroll, so it is
                            // safe to set the value to the tapped position (spec nice-to-have).
                            if (abs(totalDx) < slop && abs(totalDy) < slop) {
                                commit(down.position.x + totalDx)
                            }
                        }
                    }
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val edgePx = 6.dp.toPx()
            val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
            val centerY = size.height / 2f
            val trackThickness = 4.dp.toPx()
            val trackY = centerY - trackThickness / 2f

            // Inactive track (full width between edge insets).
            drawRoundRect(
                color = inactiveTrackColor,
                topLeft = Offset(edgePx, trackY),
                size = Size(usable, trackThickness),
                cornerRadius = CornerRadius(trackThickness / 2f),
            )
            // Active fill (min → current value).
            drawRoundRect(
                color = activeColor,
                topLeft = Offset(edgePx, trackY),
                size = Size(usable * frac, trackThickness),
                cornerRadius = CornerRadius(trackThickness / 2f),
            )

            // Tick dots — only when the step count is small enough to read.
            // Cycle Rate is -3..3 (7 steps), Cycle Skip is 0..3 (4 steps),
            // both well within range. For wider sliders the ticks would
            // crowd, so we skip them.
            if (steps in 2..12) {
                val tickRadius = 2.dp.toPx()
                for (i in 0..steps) {
                    val tickFrac = i.toFloat() / steps
                    val tickX = edgePx + usable * tickFrac
                    val onActive = tickFrac <= frac
                    drawCircle(
                        color = if (onActive) activeColor else inactiveTickColor,
                        radius = tickRadius,
                        center = Offset(tickX, centerY),
                    )
                }
            }

            // Thumb: outer halo + solid body + inner highlight pip. The
            // halo softens the dot against the dim backdrop without making
            // the thumb feel as heavy as Material's default 20dp circle.
            val thumbX = edgePx + usable * frac
            drawCircle(
                color = activeColor.copy(alpha = 0.25f),
                radius = 11.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = activeColor,
                radius = 8.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = thumbHighlight,
                radius = 2.5.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
        }
    }
}

/** Segmented chooser row — label on left, horizontal chip strip on right.
 *  Each option is a tappable chip; selected chip highlights in PS2 blue.
 *  Suitable for short option lists (3–6) — for longer lists prefer a
 *  dropdown widget (TODO when needed). */
@Composable
fun SegmentedRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    description: String? = null,
    onChange: (Int) -> Unit,
) {
    // Menu SFX: a select blip when the chosen option changes — covers the chip tap and the
    // controller's confirm/left/right, which all route through onChange.
    val emit: (Int) -> Unit = { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT); onChange(it) }
    Box(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f))
            .border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f), RoundedCornerShape(22.dp))
            .controllerFocusable(
                controllerId = "segmented:$label",
                onConfirm = {
                    if (options.isNotEmpty())
                        emit((selectedIndex + 1).floorMod(options.size))
                },
                onLeft = {
                    if (options.isNotEmpty())
                        emit((selectedIndex - 1).coerceAtLeast(0))
                },
                onRight = {
                    if (options.isNotEmpty())
                        emit((selectedIndex + 1).coerceAtMost(options.lastIndex))
                },
            )
            .padding(vertical = 14.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 18.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                if (description != null) InfoHint(label, description)
            }
            if (description != null) {
                Spacer(Modifier.height(3.dp))
                Text(
                    description,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 14.sp,
                    lineHeight = 19.sp,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }
            Spacer(Modifier.height(10.dp))
            Row(
                modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                options.forEachIndexed { idx, option ->
                    val on = idx == selectedIndex
                    Box(
                        Modifier
                            .defaultMinSize(minHeight = 42.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(if (on) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface)
                            .border(
                                1.dp,
                                if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.55f),
                                RoundedCornerShape(12.dp),
                            )
                            .clickable { emit(idx) }
                            .padding(horizontal = 13.dp, vertical = 8.dp),
                    ) {
                        Text(
                            option,
                            color = if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 14.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                }
            }
        }
    }
}

/** Multi-row variant for longer option lists. Keeps deinterlacing / hardware
 *  download choices readable inside the compact in-game overlay instead of
 *  letting a long chip strip run off-screen. */
@Composable
fun SegmentedGridRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    columns: Int = 3,
    description: String? = null,
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f))
            .border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f), RoundedCornerShape(22.dp))
            .controllerFocusable(
                controllerId = "segmented-grid:$label",
                onConfirm = {
                    if (options.isNotEmpty())
                        onChange((selectedIndex + 1).floorMod(options.size))
                },
                onLeft = {
                    if (options.isNotEmpty())
                        onChange((selectedIndex - 1).coerceAtLeast(0))
                },
                onRight = {
                    if (options.isNotEmpty())
                        onChange((selectedIndex + 1).coerceAtMost(options.lastIndex))
                },
            )
            .padding(horizontal = 16.dp, vertical = 14.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 18.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                if (description != null) InfoHint(label, description)
            }
            if (description != null) {
                Spacer(Modifier.height(3.dp))
                Text(
                    description,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 14.sp,
                    lineHeight = 19.sp,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(10.dp))
            val visibleColumns = columns.coerceIn(1, 3)
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                options.chunked(visibleColumns).forEachIndexed { rowIndex, rowOptions ->
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                        rowOptions.forEachIndexed { colIndex, option ->
                            val idx = rowIndex * visibleColumns + colIndex
                            val on = idx == selectedIndex
                            Box(
                                Modifier
                                    .weight(1f)
                                    .defaultMinSize(minHeight = 50.dp)
                                    .clip(RoundedCornerShape(12.dp))
                                    .background(if (on) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface)
                                    .border(
                                        1.dp,
                                        if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.55f),
                                        RoundedCornerShape(12.dp),
                                    )
                                    .clickable { onChange(idx) }
                                    .padding(horizontal = 7.dp, vertical = 8.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(
                                    option,
                                    color = if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                                    fontSize = 14.sp,
                                    lineHeight = 17.sp,
                                    fontWeight = FontWeight.Bold,
                                    maxLines = 2,
                                    textAlign = TextAlign.Center,
                                )
                            }
                        }
                        repeat(visibleColumns - rowOptions.size) {
                            Spacer(Modifier.weight(1f))
                        }
                    }
                }
            }
        }
    }
}

private fun Int.floorMod(modulus: Int): Int =
    if (modulus <= 0) 0 else ((this % modulus) + modulus) % modulus

@Composable
internal fun InfoHint(title: String, message: String) {
    var open by remember { mutableStateOf(false) }
    Box(
        modifier = Modifier
            .padding(start = 8.dp)
            .size(22.dp)
            .clip(androidx.compose.foundation.shape.CircleShape)
            .background(MaterialTheme.colorScheme.primaryContainer)
            .clickable { open = true },
        contentAlignment = Alignment.Center,
    ) {
        Text("i", color = MaterialTheme.colorScheme.primary, fontSize = 13.sp, fontWeight = FontWeight.Bold)
    }
    if (open) {
        androidx.compose.runtime.DisposableEffect(Unit) {
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_OPEN)
            onDispose { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_CLOSE) }
        }
        val layer = "info-hint:$title"
        val bodyScroll = rememberScrollState()
        com.armsx2.ui.common.PadModal(
            key = layer,
            onDismiss = { open = false },
            // Every setting description is a candidate for being longer than the panel, so the
            // pad needs Up/Down to read it — Close is the only thing here to select.
            scrollState = bodyScroll,
        ) {
            Surface(
                modifier = Modifier
                    .padding(24.dp)
                    .widthIn(max = 420.dp),
                shape = RoundedCornerShape(20.dp),
                color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                tonalElevation = 6.dp,
            ) {
                Column(Modifier.padding(20.dp)) {
                    Text(
                        title,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Spacer(Modifier.height(8.dp))
                    // Cap the height and scroll INSIDE it. AlertDialog did not scroll its text
                    // slot at all, so a description longer than the slot was CLIPPED mid-sentence
                    // with no way to reach the rest — which is most of the longer setting
                    // explanations (reported against Low Latency Mode, cut off at "...turning
                    // back off if the frame pacing").
                    Text(
                        message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .heightIn(max = 340.dp)
                            .verticalScroll(bodyScroll),
                    )
                    Spacer(Modifier.height(18.dp))
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        Surface(
                            onClick = { open = false },
                            modifier = Modifier.controllerFocusable(
                                controllerId = "$layer.close",
                                shape = RoundedCornerShape(14.dp),
                                onConfirm = { open = false },
                            ),
                            shape = RoundedCornerShape(14.dp),
                            color = MaterialTheme.colorScheme.primaryContainer,
                        ) {
                            Text(
                                str("action.close"),
                                modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
                                style = MaterialTheme.typography.labelLarge,
                                color = MaterialTheme.colorScheme.onPrimaryContainer,
                            )
                        }
                    }
                }
            }
        }
    }
}

/** A scroll indicator drawn down the right edge of a vertically-scrolled
 *  container, inside a DEDICATED right gutter so the rows never sit under it.
 *
 *  Layout (Approach C): the modifier reserves a fixed [gutter] column on the
 *  right via `.padding(end = gutter)` and draws the thumb INTO that gutter with
 *  a small [edgeInset] off the very edge, so:
 *    - sliders / rows are inset by [gutter] and can no longer be caught while
 *      you drag the scrollbar (the user's "too close" / "I still hit sliders"
 *      complaint), and
 *    - the bar reads as a proper, wider, grabbable indicator rather than a hair
 *      pinned to the pixel edge.
 *
 *  Modifier ORDER matters and is the whole trick:
 *    this.drawWithContent{…}.padding(end = gutter)
 *  drawWithContent is the OUTER node, so its `size.width` is the FULL width and
 *  it can paint the thumb in the gutter. padding is the INNER node, so it insets
 *  the scrollable content (the Column's rows) by [gutter], leaving that column
 *  empty for the bar. Because this whole modifier is applied AFTER
 *  `.verticalScroll(state)`, the gutter/inset belongs to the fixed viewport and
 *  does not scroll away.
 *
 *  Visible only when there IS content to scroll. The thumb's height reflects how
 *  much of the content fits on screen; its position reflects scroll progress.
 *  Non-interactive (indicator only) so it can never fight verticalScroll's own
 *  pointer handling — apply it right after `.verticalScroll(state)`. */
fun Modifier.verticalScrollbar(
    state: ScrollState,
    width: Dp = 4.dp,
    gutter: Dp = 6.dp,
    edgeInset: Dp = 1.dp,
    color: Color = Color(0x99718097),
    minThumb: Dp = 28.dp,
): Modifier = this
    .drawWithContent {
        drawContent()
        val max = state.maxValue
        if (max <= 0) return@drawWithContent // fits on screen -> no bar
        val viewport = size.height
        val content = viewport + max // total scrollable content height
        val w = width.toPx()
        val inset = edgeInset.toPx()
        val thumb = (viewport * viewport / content).coerceAtLeast(minThumb.toPx())
        val track = (viewport - thumb).coerceAtLeast(0f)
        val y = (state.value.toFloat() / max) * track
        // size.width is the FULL width here (drawWithContent is the outer node),
        // so the bar sits in the reserved gutter, inset from the very edge.
        drawRoundRect(
            color = color,
            topLeft = Offset(size.width - w - inset, y),
            size = Size(w, thumb),
            cornerRadius = CornerRadius(w / 2f, w / 2f),
        )
    }
    // Reserve the gutter: this padding is the INNER node, so it insets the
    // scrollable rows (not the bar) and keeps them out from under the indicator.
    .padding(end = gutter)
