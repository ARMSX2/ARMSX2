package com.armsx2.ui.common

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import kotlin.math.roundToInt
import com.armsx2.ui.settings.LocalNavLayer
import com.armsx2.ui.settings.SettingsControllerNav

/**
 * The app's one modal mechanism, and the only one a controller can drive.
 *
 * **Never use `Dialog`, `AlertDialog`, `DropdownMenu` or `ModalBottomSheet` in this app.** Each
 * of those is its own *focused Android window*, so it consumes gamepad KeyEvents before they
 * reach the Activity's `dispatchKeyEvent` — which is where every D-pad route in this app lives.
 * A window-based modal is therefore unreachable by pad no matter what is inside it, and it fails
 * silently: perfect on touch, completely dead on a handheld. That is the 2.6.0 "can't remap
 * buttons" bug, and it had been reintroduced in two dozen places by the time this was written.
 *
 * ### How it works
 *
 * A modal is a **portal**. [PadModal] is composed at the call site — beside the state that opens
 * it, with the call site's own values in scope — but it renders nothing there. It publishes its
 * content into a global stack, and [PadModalHost], mounted once above every surface, draws it.
 *
 * That split is not decoration. Most of this app's prompts are raised from inside something that
 * scrolls: a row in a settings tab, a card in the library grid. A scrim drawn there clips to its
 * container and scrolls away with it, so the prompt cannot be authored where it is drawn.
 *
 * Claiming the pad is a *structural consequence of being composed*, never something a call site
 * opts into: [PadModal] pushes its nav layer on enter and pops it on dispose, and the host
 * publishes that layer to its content as [LocalNavLayer]. Every shared widget inside — toggles,
 * sliders, segmented rows — therefore layers automatically, with no per-widget plumbing and
 * nothing for a call site to forget.
 *
 * ### Two rules the compiler cannot check
 *
 * - **No lazy lists inside modal content.** The nav registry only knows about rows that are
 *   actually composed, so a `LazyColumn` hides everything past the viewport from the pad. Use a
 *   plain `Column` with `verticalScroll`; every list this app puts in a modal is bounded.
 * - **Never wrap modal content in `AnimatedVisibility`.** It defers composition to a later
 *   frame, which breaks the same-frame initial-focus guarantee documented on [PadModalHost] and
 *   leaves the modal open with nothing selected.
 */
object PadModals {
    class Entry internal constructor(
        /** Doubles as the nav layer. Distinct per modal. */
        val key: String,
        internal val content: State<@Composable () -> Unit>,
        internal val alignment: State<Alignment>,
        internal val scrimAlpha: State<Float>,
        internal val onDismiss: State<(() -> Unit)?>,
        internal val initialFocusId: State<String?>,
        internal val scrollState: State<ScrollState?>,
        internal val anchor: State<Offset?>,
    ) {
        // Deliberately a plain var and not state: it must survive every recomposition of the
        // content without causing one. Focus is claimed once per open, then the pad owns it.
        internal var focusClaimed = false
    }

    // Bottom to top; the last entry owns input. A stack because modals nest — the library's
    // exit confirm is raised from inside the overflow panel.
    private val entries = mutableStateListOf<Entry>()

    val stack: List<Entry> get() = entries

    /** Whether any modal is up. Read from OUTSIDE composition — the key and motion routers ask
     *  this on every event, which is why it lives in an object rather than in the tree. */
    val visible: Boolean get() = entries.isNotEmpty()

    /** The layer that currently owns the pad, or null. */
    val topKey: String? get() = entries.lastOrNull()?.key

    internal fun push(entry: Entry) {
        entries.add(entry)
    }

    internal fun remove(entry: Entry) {
        // By identity, never "drop the last one". Sibling subtrees dispose in an order Compose
        // does not promise, so popping blindly strands the survivor: the modal still on screen
        // loses the layer it was drawn in and goes dead to the pad while looking fine.
        entries.remove(entry)
    }

    /** B, BACK, or a tap on the scrim. Returns false only when no modal is up — a modal that
     *  declines to be dismissed still swallows the press, or it would reach the screen behind. */
    fun dismissTop(): Boolean {
        val top = entries.lastOrNull() ?: return false
        top.onDismiss.value?.invoke()
        return true
    }

    /** Scroll the topmost modal's body. The router calls this when a direction has nowhere to
     *  move — a modal whose only focusable is its Close button would otherwise leave long text
     *  unreachable on a pad, which is precisely the defect the window dialogs had. Returns false
     *  when the top modal declared no scrollable body, so the press can be swallowed either way. */
    fun scrollTop(dy: Int): Boolean {
        val scroll = entries.lastOrNull()?.scrollState?.value ?: return false
        scroll.dispatchRawDelta(dy * SCROLL_STEP_PX)
        return true
    }

    // Roughly two lines of body text per press: small enough to land on the line you wanted,
    // large enough that a long description doesn't take twenty presses to read.
    private const val SCROLL_STEP_PX = 120f
}

/**
 * Publishes [content] as a modal for as long as this call site is composed.
 *
 * @param key identifies the modal and IS its nav layer, so it must be distinct from any other
 *   modal that can be open at the same time. Registry ids inside should share it as a prefix.
 * @param onDismiss B / BACK / a tap outside. Null makes the modal insistent — it still swallows
 *   the press, it just does not close.
 * @param initialFocusId the row to focus on open. Falls back to the first row in the layer, so
 *   a modal is never left with nothing selected.
 * @param scrollState the body's scroll state, when the content can outgrow the panel. Up/Down
 *   scroll it once the selection has nowhere left to move, which is the only way a pad can read
 *   a panel that has just one focusable in it.
 * @param anchor root-space position to pin the panel's top-left to, clamped to stay on screen.
 *   For a menu belonging to a specific button; [alignment] is ignored when it is set.
 */
@Composable
fun PadModal(
    key: String,
    onDismiss: (() -> Unit)? = null,
    alignment: Alignment = Alignment.Center,
    scrimAlpha: Float = 0.62f,
    initialFocusId: String? = null,
    scrollState: ScrollState? = null,
    anchor: Offset? = null,
    content: @Composable () -> Unit,
) {
    // Re-published on EVERY recomposition, so a closure can never go stale. The nav registry
    // learned this the hard way: a closure captured once is wrong the moment one row recomposes
    // without its parent, and the next press either no-ops or acts on a value already changed.
    val contentState = rememberUpdatedState(content)
    val alignmentState = rememberUpdatedState(alignment)
    val scrimState = rememberUpdatedState(scrimAlpha)
    val dismissState = rememberUpdatedState(onDismiss)
    val focusState = rememberUpdatedState(initialFocusId)
    val scrollStateHolder = rememberUpdatedState(scrollState)
    val anchorState = rememberUpdatedState(anchor)
    val entry = remember(key) {
        PadModals.Entry(
            key, contentState, alignmentState, scrimState, dismissState, focusState,
            scrollStateHolder, anchorState,
        )
    }
    DisposableEffect(entry) {
        PadModals.push(entry)
        SettingsControllerNav.pushLayer(key)
        onDispose {
            PadModals.remove(entry)
            SettingsControllerNav.popLayer(key)
        }
    }
}

/**
 * Renders every open modal. Mounted exactly once, in WindowImpl, above every surface.
 *
 * ### The initial-focus guarantee
 *
 * `controllerFocusable` registers a row from a `SideEffect`, and side effects run at the end of
 * a composition pass in *recording* order. The focus claim below is recorded **after** the
 * content, so it is guaranteed to see every row the modal just composed, in the same frame.
 * That is what replaces the bounded retry loop the old confirmation overlay used, which could
 * quietly give up with nothing selected and hand the first press to the screen behind.
 *
 * The guarantee is why modal content must never be wrapped in `AnimatedVisibility`.
 */
@Composable
fun PadModalHost() {
    val entries = PadModals.stack
    if (entries.isEmpty()) return
    Box(Modifier.fillMaxSize()) {
        // ONE scrim for the whole stack, not one per modal. Stacked scrims multiply: two of
        // them read as near-black and make the lower modal look disabled rather than behind.
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = entries.last().scrimAlpha.value))
                // Swallow taps so they cannot reach the screen behind, and treat a tap outside
                // the panel as dismiss. indication = null: a ripple across the whole screen
                // reads as a rendering bug.
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = { PadModals.dismissTop() },
                ),
        )
        for (entry in entries) {
            key(entry.key) {
                val anchor = entry.anchor.value
                // Absorb taps on the panel itself, or the scrim's dismiss fires through it and
                // the modal closes as you press its own buttons.
                // Swipe-to-dismiss, for bottom-aligned panels only.
                //
                // PadModal replaced ModalBottomSheet because that is its own focused Android
                // window and every row inside it was unreachable by pad. The one thing lost in the
                // trade was the swipe — and a panel that rises from the bottom edge with a rounded
                // top and a drag handle is *promising* a swipe, so its absence reads as broken
                // rather than as a deliberate omission. Restored here without giving up focus.
                //
                // Only for BottomCenter: on a centred or anchored menu a downward drag means
                // nothing, and hijacking it would break scrolling inside those panels.
                val bottomAligned = entry.alignment.value == Alignment.BottomCenter
                val absorbTaps = @Composable { inner: @Composable () -> Unit ->
                    var dragOffset by remember { mutableFloatStateOf(0f) }
                    val density = LocalDensity.current
                    val dismissThresholdPx = with(density) { 110.dp.toPx() }
                    // Only a drag STARTING in this top strip owns the gesture. The strip is where
                    // the drag handle sits, which is where people grab a sheet anyway.
                    val handleStripPx = with(density) { 64.dp.toPx() }
                    val slopPx = with(density) { 8.dp.toPx() }
                    Box(
                        Modifier
                            .then(
                                if (!bottomAligned) Modifier
                                else Modifier
                                    .offset { IntOffset(0, dragOffset.roundToInt()) }
                                    .pointerInput(entry.key) {
                                        // ★ INITIAL pass, and a top-strip guard.
                                        //
                                        // Compose delivers pointer events to CHILDREN first, so a
                                        // detector on this Box never saw the drag — every row in
                                        // the sheet is clickable and consumed it in the Main pass.
                                        // That is why the first attempt at swipe-to-dismiss did
                                        // nothing at all. Watching the Initial pass is the only
                                        // way a parent can win.
                                        //
                                        // Winning it everywhere would be worse than not having it:
                                        // it would eat scrolling inside any modal that scrolls. So
                                        // the gesture is claimed only when it STARTS in the top
                                        // strip, and only once it has clearly gone downward.
                                        awaitEachGesture {
                                            val down = awaitFirstDown(requireUnconsumed = false,
                                                pass = PointerEventPass.Initial)
                                            if (down.position.y > handleStripPx) return@awaitEachGesture
                                            var claimed = false
                                            var total = 0f
                                            while (true) {
                                                val ev = awaitPointerEvent(PointerEventPass.Initial)
                                                val ch = ev.changes.firstOrNull { it.id == down.id } ?: break
                                                if (!ch.pressed) break
                                                total += ch.positionChange().y
                                                if (!claimed && total > slopPx) claimed = true
                                                if (claimed) {
                                                    ch.consume()
                                                    dragOffset = total.coerceAtLeast(0f)
                                                }
                                            }
                                            if (claimed && dragOffset > dismissThresholdPx) PadModals.dismissTop()
                                            dragOffset = 0f
                                        }
                                    }
                            )
                            // Absorb taps LAST so the drag detector above sees the gesture first.
                            .clickable(
                                interactionSource = remember { MutableInteractionSource() },
                                indication = null,
                                onClick = {},
                            ),
                    ) {
                        CompositionLocalProvider(LocalNavLayer provides entry.key) { inner() }
                    }
                }
                if (anchor == null) {
                    Box(Modifier.fillMaxSize(), contentAlignment = entry.alignment.value) {
                        absorbTaps { entry.content.value() }
                    }
                } else {
                    // Anchored to its trigger — the ⋮ menus, which have to keep reading as
                    // "this button's menu" rather than a prompt about the whole screen.
                    BoxWithConstraints(Modifier.fillMaxSize()) {
                        var size by remember { mutableStateOf(IntSize.Zero) }
                        // Clamp so a panel opened from a trigger near the right or bottom edge
                        // stays fully on screen instead of running off it. Before it has been
                        // measured the clamp is a no-op, which is harmless: the anchor is a
                        // point on screen by construction.
                        val x = anchor.x.roundToInt()
                            .coerceIn(0, (constraints.maxWidth - size.width).coerceAtLeast(0))
                        val y = anchor.y.roundToInt()
                            .coerceIn(0, (constraints.maxHeight - size.height).coerceAtLeast(0))
                        Box(
                            Modifier
                                .offset { IntOffset(x, y) }
                                .onSizeChanged { size = it },
                        ) {
                            absorbTaps { entry.content.value() }
                        }
                    }
                }
                SideEffect {
                    if (!entry.focusClaimed) {
                        entry.focusClaimed = true
                        val wanted = entry.initialFocusId.value
                        if (wanted == null || !SettingsControllerNav.selectById(wanted))
                            SettingsControllerNav.selectFirstInLayer()
                    }
                }
            }
        }
    }
}
