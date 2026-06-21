package com.armsx2.ui.settings

import android.view.KeyEvent as AndroidKeyEvent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.foundation.ScrollState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
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
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.ui.Colors
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Shared widget primitives for the in-game settings tabs. Style matches
 * InGameOverlay's MenuRow — left-anchored alpha aura on the row bg,
 * white text, transparent borders. Each row is 24dp tall to keep the
 * tab content fitting in the same 75% screen height the Playing-Now
 * tab uses.
 */

private val rowAuraOn = Color.White.copy(alpha = 0.06f)
private val rowAuraTransparent = Color.Transparent
private val focusBlue = Color(0xFF3DA5FF)

/** Horizontal background gradient that matches the divider direction. */
internal fun rowAura() = Brush.horizontalGradient(listOf(rowAuraOn, rowAuraTransparent))

internal object SettingsControllerNav {
    private data class Item(
        val id: String,
        val onConfirm: (() -> Unit)?,
        val onLeft: (() -> Unit)?,
        val onRight: (() -> Unit)?,
    )

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
    val selectedIndex = mutableStateOf(-1)
    val scrollVelocity = mutableStateOf(0f)

    fun setPosition(id: String, x: Float, y: Float) {
        positions[id] = y to x
    }

    private fun orderedIds(): List<String> =
        // Stable sort: unpositioned items (briefly, before layout) keep their
        // registration order.
        registry.keys.sortedWith(
            compareBy(
                { positions[it]?.first ?: Float.MAX_VALUE },
                { positions[it]?.second ?: 0f },
            ),
        )

    fun begin(scope: String) {
        if (scopeKey != scope) {
            // Switched tab: drop the old selection. The new tab's rows register
            // during this composition and stale ids are pruned by onDispose.
            scopeKey = scope
            selectedId.value = null
            selectedIndex.value = -1
        }
    }

    fun register(
        id: String,
        onConfirm: (() -> Unit)? = null,
        onLeft: (() -> Unit)? = null,
        onRight: (() -> Unit)? = null,
    ) {
        // Upsert — replacing an existing key keeps its insertion order (stable
        // visual order) while refreshing the closures to the current value.
        registry[id] = Item(id, onConfirm, onLeft, onRight)
        if (selectedId.value == id)
            selectedIndex.value = orderedIds().indexOf(id)
    }

    fun unregister(id: String) {
        registry.remove(id)
        positions.remove(id)
        if (selectedId.value == id)
            selectedId.value = orderedIds().firstOrNull()
        selectedIndex.value = orderedIds().indexOf(selectedId.value)
    }

    fun end() {
        // Keep the highlighted index in sync with the current order.
        selectedIndex.value = orderedIds().indexOf(selectedId.value)
    }

    fun clearSelection() {
        selectedId.value = null
        selectedIndex.value = -1
        scrollVelocity.value = 0f
    }

    /** No-op retained for callers; adjustment no longer uses a time gate. */
    fun resetAdjustmentGate() {}

    fun hasItems(): Boolean = registry.isNotEmpty()

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
        selectedIndex.value = next
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
        item.onConfirm?.invoke() ?: return false
        return true
    }

    fun setScrollVelocity(velocity: Float): Boolean {
        scrollVelocity.value = if (abs(velocity) > 0.08f) velocity.coerceIn(-1f, 1f) else 0f
        return true
    }

    fun isSelected(id: String): Boolean = selectedId.value == id

    private fun selectedItem(): Item? {
        val ids = orderedIds()
        if (ids.isEmpty()) return null
        val id = selectedId.value
        if (id == null || !registry.containsKey(id)) {
            val first = ids.first()
            selectedId.value = first
            selectedIndex.value = 0
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
            val velocity = SettingsControllerNav.scrollVelocity.value
            if (abs(velocity) > 0.08f && scroll.maxValue > 0) {
                val pxPerSecond = with(density) { 1500.dp.toPx() }
                scroll.scrollBy(velocity * pxPerSecond * dt)
            }
        }
    }
}

internal fun Modifier.controllerFocusable(
    controllerId: String? = null,
    shape: RoundedCornerShape = RoundedCornerShape(4.dp),
    onConfirm: (() -> Unit)? = null,
    onLeft: (() -> Unit)? = null,
    onRight: (() -> Unit)? = null,
): Modifier = composed {
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
                AndroidKeyEvent.KEYCODE_DPAD_LEFT,
                AndroidKeyEvent.KEYCODE_DPAD_RIGHT -> false
                else -> false
            }
        }
        .then(
            if (focused || selected) {
                Modifier
                    .shadow(6.dp, shape, ambientColor = focusBlue, spotColor = focusBlue)
                    .border(1.dp, focusBlue, shape)
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
                    listOf(Color.White.copy(alpha = 0.35f), Color.Transparent)
                )
            ),
    )
}

@Composable
fun HelpText(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        color = Color(0xFFB8B8B8),
        fontSize = 10.sp,
        lineHeight = 12.sp,
        modifier = modifier.padding(horizontal = 6.dp, vertical = 4.dp),
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
    Box(
        Modifier
            .fillMaxWidth()
            .height(if (description == null) 24.dp else 42.dp)
            .background(rowAura())
            .clickable { onChange(!value) }
            .controllerFocusable(
                controllerId = "toggle:$label",
                onConfirm = { onChange(!value) },
                onLeft = { if (value) onChange(false) },
                onRight = { if (!value) onChange(true) },
            )
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                if (description != null) {
                    Text(
                        description,
                        color = Color(0xFFB8B8B8),
                        fontSize = 9.sp,
                        lineHeight = 10.sp,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Text(
                if (value) "On" else "Off",
                color = if (value) Colors.pasx2_blue else Color(0xFF888888),
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/** Compact On/Off bubble — same visual language as the Playing-Now grid
 *  in [InGameOverlay]. Label on top (1–2 lines), state line ("On"/"Off")
 *  below. When the toggle is active the surface uses the PS2-blue accent
 *  treatment; inactive matches the neutral bubble surface. Caller is
 *  responsible for laying these out in a [BubbleGridRow]. */
@Composable
fun ToggleBubble(
    label: String,
    value: Boolean,
    modifier: Modifier = Modifier,
    onChange: (Boolean) -> Unit,
) {
    val bg: Color
    val border: Color
    if (value) {
        bg = Color(0xFF222F40)
        border = Colors.pasx2_blue.copy(alpha = 0.50f)
    } else {
        bg = Color(0xFF1F2123)
        border = Color.White.copy(alpha = 0.10f)
    }
    Column(
        modifier = modifier
            .aspectRatio(1.35f)
            .clip(RoundedCornerShape(10.dp))
            .background(bg)
            .border(1.dp, border, RoundedCornerShape(10.dp))
            .clickable { onChange(!value) }
            .controllerFocusable(
                controllerId = "bubble:$label",
                shape = RoundedCornerShape(10.dp),
                onConfirm = { onChange(!value) },
            )
            .padding(horizontal = 4.dp, vertical = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            color = Color.White,
            fontSize = 9.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            if (value) "On" else "Off",
            color = if (value) Colors.pasx2_blue else Color.White.copy(alpha = 0.6f),
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

/** Even-spaced four-cell row for [ToggleBubble] grids. Mirrors the
 *  Playing-Now layout so the two surfaces feel like the same component
 *  family. Use [Modifier.weight] inside `content` on each child. */
@Composable
fun BubbleGridRow(content: @Composable RowScope.() -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        content = content,
    )
}

/** Integer slider row — label + current value on top line, custom slim
 *  slider below. Replaces Material3's chunky default with a Canvas-drawn
 *  slider that matches the overlay's thin-line aesthetic: 3dp track,
 *  tick dots at each discrete step, 5dp thumb with a soft halo. Drag
 *  and tap-to-position both update the value. */
@Composable
fun IntSliderRow(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    description: String? = null,
    valueFormatter: (Int) -> String = { it.toString() },
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(if (description == null) 36.dp else 50.dp)
            .background(rowAura())
            .controllerFocusable(
                controllerId = "slider:$label",
                onLeft = { onChange((value - 1).coerceAtLeast(min)) },
                onRight = { onChange((value + 1).coerceAtMost(max)) },
            )
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                    if (description != null) {
                        Text(
                            description,
                            color = Color(0xFFB8B8B8),
                            fontSize = 9.sp,
                            lineHeight = 10.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Text(
                    valueFormatter(value),
                    color = Colors.pasx2_blue,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            Spacer(Modifier.height(3.dp))
            DiscreteSlider(
                value = value,
                min = min,
                max = max,
                onChange = onChange,
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
    val activeColor = Colors.pasx2_blue
    val inactiveTrackColor = Color.White.copy(alpha = 0.12f)
    val inactiveTickColor = Color.White.copy(alpha = 0.35f)

    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(14.dp)
            .pointerInput(min, max) {
                val edgePx = 6.dp.toPx()
                fun update(x: Float) {
                    val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
                    val f = ((x - edgePx) / usable).coerceIn(0f, 1f)
                    onChange(min + (f * steps).roundToInt())
                }
                detectTapGestures { update(it.x) }
            }
            .pointerInput(min, max) {
                val edgePx = 6.dp.toPx()
                detectHorizontalDragGestures { change, _ ->
                    val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
                    val f = ((change.position.x - edgePx) / usable).coerceIn(0f, 1f)
                    onChange(min + (f * steps).roundToInt())
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val edgePx = 6.dp.toPx()
            val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
            val centerY = size.height / 2f
            val trackThickness = 3.dp.toPx()
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
                val tickRadius = 1.5.dp.toPx()
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
                radius = 7.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = activeColor,
                radius = 5.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = Color.White,
                radius = 1.5.dp.toPx(),
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
    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .controllerFocusable(
                controllerId = "segmented:$label",
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
            .padding(horizontal = 6.dp, vertical = 3.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            if (description != null) {
                Spacer(Modifier.height(1.dp))
                Text(
                    description,
                    color = Color(0xFFB8B8B8),
                    fontSize = 9.sp,
                    lineHeight = 10.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(3.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                options.forEachIndexed { idx, option ->
                    val on = idx == selectedIndex
                    Box(
                        Modifier
                            .clip(RoundedCornerShape(4.dp))
                            .background(if (on) Colors.pasx2_blue else Color(0xFF272525).copy(alpha = 0.5f))
                            .clickable { onChange(idx) }
                            .padding(horizontal = 8.dp, vertical = 3.dp),
                    ) {
                        Text(
                            option,
                            color = if (on) Color.White else Color(0xFFAAAAAA),
                            fontSize = 11.sp,
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
            .background(rowAura())
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
            .padding(horizontal = 6.dp, vertical = 3.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            if (description != null) {
                Spacer(Modifier.height(1.dp))
                Text(
                    description,
                    color = Color(0xFFB8B8B8),
                    fontSize = 9.sp,
                    lineHeight = 10.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(3.dp))
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                options.chunked(columns.coerceAtLeast(1)).forEachIndexed { rowIndex, rowOptions ->
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
                        rowOptions.forEachIndexed { colIndex, option ->
                            val idx = rowIndex * columns.coerceAtLeast(1) + colIndex
                            val on = idx == selectedIndex
                            Box(
                                Modifier
                                    .weight(1f)
                                    .clip(RoundedCornerShape(4.dp))
                                    .background(if (on) Colors.pasx2_blue else Color(0xFF272525).copy(alpha = 0.5f))
                                    .clickable { onChange(idx) }
                                    .padding(horizontal = 5.dp, vertical = 3.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(
                                    option,
                                    color = if (on) Color.White else Color(0xFFAAAAAA),
                                    fontSize = 10.sp,
                                    fontWeight = FontWeight.Bold,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                        repeat(columns.coerceAtLeast(1) - rowOptions.size) {
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
