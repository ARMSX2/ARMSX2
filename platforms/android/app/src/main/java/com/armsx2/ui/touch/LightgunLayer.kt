package com.armsx2.ui.touch

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.input.pointer.PointerEvent
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.changedToDownIgnoreConsumed
import androidx.compose.ui.node.ModifierNodeElement
import androidx.compose.ui.node.PointerInputModifierNode
import androidx.compose.ui.unit.IntSize
import com.armsx2.input.Lightgun
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Touchscreen aiming for the GunCon 2.
 *
 * With a gun attached, a touch on empty screen IS the gun: it aims where the finger is and holds
 * the trigger until the finger lifts. Aim tracks the finger continuously rather than only on tap:
 * pointing and then firing is how these games are played, and it lets you lead a target.
 *
 * A finger that lands on a control is left to that control. Widgets with a handler of their own
 * (the gun's buttons, the D-pad, the sticks, pause) sit above this layer and take their fingers
 * first. The multi-touch face, shoulder and Start/Select buttons have none, so their areas come in
 * as [blockers]; see [LightgunInputNode] for why that is needed.
 */
@Composable
fun LightgunLayer(widthPx: Float, heightPx: Float, blockers: GunBlockers) {
    if (!Lightgun.enabled.value) return
    if (widthPx <= 0f || heightPx <= 0f) return
    Box(Modifier.fillMaxSize().then(LightgunInputElement(LightgunParams(widthPx, heightPx, blockers))))
}

/** Where the multi-touch buttons answer: each widget's square, and the hit circle that reaches
 *  past it. A finger that starts in one of these is a button press, not a shot. */
data class GunBlockers(val rects: List<Rect>, val circles: List<Pair<Offset, Float>>) {
    fun contains(p: Offset): Boolean =
        rects.any { it.contains(p) } ||
            circles.any { (center, radius) -> (p - center).getDistanceSquared() <= radius * radius }
}

/** The gun button a [TouchButtonId.Kind.GUN] widget presses. */
internal fun gunBind(id: TouchButtonId): Int = when (id) {
    TouchButtonId.GUN_A -> NativeApp.GUNCON_A
    TouchButtonId.GUN_B -> NativeApp.GUNCON_B
    TouchButtonId.GUN_C -> NativeApp.GUNCON_C
    TouchButtonId.GUN_START -> NativeApp.GUNCON_START
    TouchButtonId.GUN_SELECT -> NativeApp.GUNCON_SELECT
    else -> NativeApp.GUNCON_RECALIBRATE
}

/** Everything [LightgunInputNode] decides with. A data class, so the node only starts over (and
 *  lets go of a held trigger) when one of these really changes, the way a pointerInput key did. */
private data class LightgunParams(val widthPx: Float, val heightPx: Float, val blockers: GunBlockers)

/**
 * The aim layer's touch handling: a node rather than a `pointerInput`, so it can share.
 *
 * This layer and [UnifiedTouchLayer] (every face, shoulder and Start/Select button while
 * multi-touch is on) both cover the whole screen, and Compose gives a touch to only ONE of two
 * overlapping siblings unless the upper one says otherwise. This one is the upper one, so with a
 * gun attached it took every touch, and a press on Circle fired the gun instead.
 *
 * The fix is the Half-Screen Sticks one: share ([sharePointerInputWithSiblings]) and decide who owns
 * a finger in the Initial pass, before the button layer looks. A finger on empty screen is claimed
 * and consumed, which the button layer reads at DOWN as someone else's, so an aim dragged across
 * Circle does not press it. A finger on a button is neither claimed nor consumed.
 */
private class LightgunInputElement(val params: LightgunParams) : ModifierNodeElement<LightgunInputNode>() {
    override fun create() = LightgunInputNode(params)
    override fun update(node: LightgunInputNode) = node.update(params)
    override fun equals(other: Any?) = other is LightgunInputElement && other.params == params
    override fun hashCode() = params.hashCode()
}

private class LightgunInputNode(private var params: LightgunParams) : Modifier.Node(), PointerInputModifierNode {
    private var aiming: PointerId? = null
    private var last = Offset.Zero

    fun update(next: LightgunParams) {
        if (next == params) return
        release()
        params = next
    }

    override fun sharePointerInputWithSiblings() = true

    override fun onPointerEvent(pointerEvent: PointerEvent, pass: PointerEventPass, bounds: IntSize) {
        if (pass != PointerEventPass.Initial) return
        val p = params
        for (ch in pointerEvent.changes) {
            if (ch.changedToDownIgnoreConsumed()) {
                // One gun, one trigger: a second finger is not a second shot. A finger on a button
                // belongs to the button.
                if (ch.isConsumed || aiming != null || p.blockers.contains(ch.position)) continue
                aiming = ch.id
                last = ch.position
                // A held trigger is the controls in use, so a timed auto-hide cannot take this
                // layer away mid-shot (see TouchControls.activeHolds).
                TouchControls.beginTouchHold()
                if (Lightgun.calibrateNext.value) {
                    // Cal was tapped first: this touch is the calibration shot, on the target.
                    Lightgun.calibrationShot(ch.position.x, ch.position.y, p.widthPx, p.heightPx)
                } else {
                    // Aim BEFORE the trigger, in that order: the core samples the pointer when the
                    // trigger goes down, so firing first would shoot where the previous shot landed.
                    Lightgun.aim(ch.position.x, ch.position.y, p.widthPx, p.heightPx)
                    Lightgun.trigger(true, ch.position.x, ch.position.y, p.widthPx, p.heightPx)
                }
                ch.consume()
                continue
            }
            if (ch.id != aiming) continue
            ch.consume()
            last = ch.position
            if (ch.pressed) {
                Lightgun.aim(ch.position.x, ch.position.y, p.widthPx, p.heightPx)
            } else {
                Lightgun.trigger(false, ch.position.x, ch.position.y, p.widthPx, p.heightPx)
                aiming = null
                TouchControls.endTouchHold()
            }
        }
    }

    override fun onCancelPointerInput() = release()

    override fun onDetach() = release()

    /** Let go of a held trigger: once this layer is gone (the controls hiding, the pause menu),
     *  nothing is left that could release it. */
    private fun release() {
        if (aiming == null) return
        aiming = null
        Lightgun.trigger(false, last.x, last.y, params.widthPx, params.heightPx)
        TouchControls.endTouchHold()
    }
}
