package com.armsx2.ui.touch

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEvent
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.changedToDownIgnoreConsumed
import androidx.compose.ui.node.ModifierNodeElement
import androidx.compose.ui.node.PointerInputModifierNode
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.armsx2.runtime.MainActivityRuntime
import kotlin.math.abs

/**
 * Swipe / double-tap gestures on empty screen area (PPSSPP-style, requested by SNAKEATER).
 *
 * Composed BELOW the visual widgets, exactly like [UnifiedTouchLayer], so a finger that starts on a
 * button, stick or d-pad is claimed by that widget first and never reaches here. It never consumes
 * anything itself, and it looks at a finger only after every other layer has: see [GestureInputNode]
 * for why that takes more than not consuming.
 *
 * Deliberately single-finger: a swipe is only tracked for a pointer that went down while no other
 * gesture pointer was active, so a second thumb resting on the screen can't turn a button press into
 * a phantom swipe.
 */
@Composable
fun GestureLayer(widthPx: Float, heightPx: Float) {
    if (!TouchControls.gestureEnabled.value) return
    if (widthPx <= 0f || heightPx <= 0f) return

    val up = TouchControls.gestureSwipeUp.intValue
    val down = TouchControls.gestureSwipeDown.intValue
    val left = TouchControls.gestureSwipeLeft.intValue
    val right = TouchControls.gestureSwipeRight.intValue
    val dtap = TouchControls.gestureDoubleTap.intValue
    val holdMode = TouchControls.gestureDoubleTapHold.value
    val sens = TouchControls.gestureSwipeSensitivity.floatValue

    // Nothing bound: don't install a pointer handler at all.
    if (up == 0 && down == 0 && left == 0 && right == 0 && dtap == 0) return

    val density = LocalDensity.current
    val params = GestureParams(
        up = up, down = down, left = left, right = right, doubleTap = dtap, holdMode = holdMode,
        // Threshold from the SHORTER edge so a swipe feels the same in portrait and landscape.
        threshold = minOf(widthPx, heightPx) * sens,
        // A tap must stay within this radius to count as a tap rather than a short swipe.
        tapSlop = with(density) { 18.dp.toPx() },
    )
    Box(Modifier.fillMaxSize().then(GestureInputElement(params)))
}

/** Everything [GestureInputNode] decides with. A data class, so the node only starts over when one
 *  of these really changes, the way a pointerInput key did. */
private data class GestureParams(
    val up: Int,
    val down: Int,
    val left: Int,
    val right: Int,
    val doubleTap: Int,
    val holdMode: Boolean,
    val threshold: Float,
    val tapSlop: Float,
)

/**
 * The gesture layer's touch handling: a node rather than a `pointerInput`, so it can share.
 *
 * This layer and [UnifiedTouchLayer] (every face, shoulder and Start/Select button while multi-touch
 * is on) both cover the whole screen, and Compose gives a touch to only ONE of two overlapping
 * siblings unless the upper one says otherwise. This one is the upper one, so with gestures on and
 * anything bound, it took every touch and those buttons were dead. Not consuming did not help: the
 * layers under it were never asked.
 *
 * So it shares ([sharePointerInputWithSiblings]) and reads the Final pass, which runs after every
 * layer's Main pass. By then the button layer has consumed a finger on a button, a Half-Screen
 * Sticks thumb is consumed, and a lightgun shot is too. A DOWN still unconsumed at that point is on
 * empty screen, which is the only place a gesture belongs.
 */
private class GestureInputElement(val params: GestureParams) : ModifierNodeElement<GestureInputNode>() {
    override fun create() = GestureInputNode(params)
    override fun update(node: GestureInputNode) = node.update(params)
    override fun equals(other: Any?) = other is GestureInputElement && other.params == params
    override fun hashCode() = params.hashCode()
}

private class GestureInputNode(private var params: GestureParams) : Modifier.Node(), PointerInputModifierNode {
    private var tracking: PointerId? = null
    private var startPos = Offset.Zero
    private var startTime = 0L
    private var fired = false
    private var lastTapTime = 0L
    private var lastTapPos = Offset.Zero
    // Latch state for HOLD mode, so a second double-tap releases.
    private var latchedCode = 0

    fun update(next: GestureParams) {
        if (next == params) return
        // What a pointerInput key change did: start over.
        reset()
        params = next
    }

    override fun sharePointerInputWithSiblings() = true

    override fun onPointerEvent(pointerEvent: PointerEvent, pass: PointerEventPass, bounds: IntSize) {
        if (pass != PointerEventPass.Final) return
        val p = params
        for (ch in pointerEvent.changes) {
            if (ch.changedToDownIgnoreConsumed()) {
                // Reject a DOWN a control already claimed, and ignore extra fingers.
                if (ch.isConsumed || tracking != null) continue
                tracking = ch.id
                startPos = ch.position
                startTime = ch.uptimeMillis
                fired = false
                continue
            }
            if (ch.id != tracking) continue
            // A control took this finger after all: it slid onto a button. Then it is a press on
            // that button, not a gesture.
            if (ch.isConsumed) {
                tracking = null
                continue
            }

            if (ch.pressed) {
                if (fired) continue
                val d = ch.position - startPos
                if (abs(d.x) >= p.threshold || abs(d.y) >= p.threshold) {
                    // Dominant axis wins, so a diagonal drag fires one direction rather than two.
                    val code = if (abs(d.x) > abs(d.y)) {
                        if (d.x > 0) p.right else p.left
                    } else {
                        if (d.y > 0) p.down else p.up
                    }
                    pulse(code)
                    fired = true
                }
            } else {
                // Lift. A short, near-stationary press is a tap; two in 300ms is a double-tap.
                val now = ch.uptimeMillis
                val moved = (ch.position - startPos).getDistance()
                if (!fired && moved <= p.tapSlop && now - startTime <= 250) {
                    val nearLast = (ch.position - lastTapPos).getDistance() <= p.tapSlop * 3f
                    if (now - lastTapTime <= 300 && nearLast) {
                        if (p.holdMode) toggleLatch(p.doubleTap) else pulse(p.doubleTap)
                        lastTapTime = 0L // consumed; don't chain a third tap
                    } else {
                        lastTapTime = now
                        lastTapPos = ch.position
                    }
                }
                tracking = null
            }
        }
    }

    // A cancelled touch is not a tap or a swipe.
    override fun onCancelPointerInput() {
        tracking = null
    }

    override fun onDetach() = reset()

    /** Forget any gesture in progress, and let go of a latched double-tap button: once this layer
     *  is gone (the controls hiding, the pause menu), nothing is left that could release it. */
    private fun reset() {
        tracking = null
        fired = false
        lastTapTime = 0L
        if (latchedCode != 0) {
            runCatching { kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, latchedCode, 0, false) }
            latchedCode = 0
        }
    }

    private fun pulse(code: Int) {
        if (code == 0) return
        // The emulated pad drops an instant down+up, so a gesture has to HOLD briefly — see the
        // input-timing note in MainActivityRuntime. 40ms comfortably clears one 60Hz sample
        // without feeling sticky.
        MainActivityRuntime.instance?.let {
            kotlin.concurrent.thread(name = "armsx2-gesture-pulse") {
                runCatching {
                    kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, code, 0, true)
                    Thread.sleep(40)
                    kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, code, 0, false)
                }
            }
        }
        TouchControls.noteTouchInteraction()
    }

    private fun toggleLatch(code: Int) {
        if (code == 0) return
        runCatching {
            if (latchedCode == code) {
                kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, code, 0, false)
                latchedCode = 0
            } else {
                if (latchedCode != 0)
                    kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, latchedCode, 0, false)
                kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(TouchControls.playerPort, code, 0, true)
                latchedCode = code
            }
        }
        TouchControls.noteTouchInteraction()
    }
}
