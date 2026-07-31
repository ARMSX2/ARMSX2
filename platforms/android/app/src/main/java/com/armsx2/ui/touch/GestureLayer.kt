package com.armsx2.ui.touch

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.changedToDown
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import com.armsx2.runtime.MainActivityRuntime
import kotlin.math.abs

/**
 * Swipe / double-tap gestures on empty screen area (PPSSPP-style, requested by SNAKEATER).
 *
 * Composed BELOW the visual widgets, exactly like [UnifiedTouchLayer], so a finger that starts on a
 * button, stick or d-pad is claimed by that widget first and never reaches here. On top of that this
 * layer independently rejects any DOWN that arrives already consumed, and never consumes anything
 * itself — so it cannot starve a control even if the ordering above it ever changes. A gesture layer
 * that ate presses would be far worse than one that occasionally misses a swipe.
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
    // Threshold from the SHORTER edge so a swipe feels the same in portrait and landscape.
    val threshold = minOf(widthPx, heightPx) * sens
    // A tap must stay within this radius to count as a tap rather than a short swipe.
    val tapSlop = with(density) { 18.dp.toPx() }

    Box(
        Modifier
            .fillMaxSize()
            .pointerInput(up, down, left, right, dtap, holdMode, sens, widthPx, heightPx) {
                var tracking: PointerId? = null
                var startPos = Offset.Zero
                var startTime = 0L
                var fired = false
                var lastTapTime = 0L
                var lastTapPos = Offset.Zero
                // Latch state for HOLD mode, so a second double-tap releases.
                var latchedCode = 0

                fun pulse(code: Int) {
                    if (code == 0) return
                    // The emulated pad drops an instant down+up, so a gesture has to HOLD briefly —
                    // see the input-timing note in MainActivityRuntime. 40ms comfortably clears one
                    // 60Hz sample without feeling sticky.
                    MainActivityRuntime.instance?.let {
                        kotlin.concurrent.thread(name = "armsx2-gesture-pulse") {
                            runCatching {
                                kr.co.iefriends.pcsx2.NativeApp.setPadButton(code, 0, true)
                                Thread.sleep(40)
                                kr.co.iefriends.pcsx2.NativeApp.setPadButton(code, 0, false)
                            }
                        }
                    }
                    TouchControls.noteTouchInteraction()
                }

                fun toggleLatch(code: Int) {
                    if (code == 0) return
                    runCatching {
                        if (latchedCode == code) {
                            kr.co.iefriends.pcsx2.NativeApp.setPadButton(code, 0, false)
                            latchedCode = 0
                        } else {
                            if (latchedCode != 0)
                                kr.co.iefriends.pcsx2.NativeApp.setPadButton(latchedCode, 0, false)
                            kr.co.iefriends.pcsx2.NativeApp.setPadButton(code, 0, true)
                            latchedCode = code
                        }
                    }
                    TouchControls.noteTouchInteraction()
                }

                awaitPointerEventScope {
                    while (true) {
                        val ev = awaitPointerEvent()
                        for (ch in ev.changes) {
                            if (ch.changedToDown()) {
                                // Reject a DOWN a control already claimed, and ignore extra fingers.
                                if (ch.isConsumed || tracking != null) continue
                                tracking = ch.id
                                startPos = ch.position
                                startTime = ev.changes.first().uptimeMillis
                                fired = false
                                continue
                            }
                            if (ch.id != tracking) continue

                            if (ch.pressed) {
                                if (fired) continue
                                val d = ch.position - startPos
                                if (abs(d.x) >= threshold || abs(d.y) >= threshold) {
                                    // Dominant axis wins, so a diagonal drag fires one direction
                                    // rather than two.
                                    val code = if (abs(d.x) > abs(d.y)) {
                                        if (d.x > 0) right else left
                                    } else {
                                        if (d.y > 0) down else up
                                    }
                                    pulse(code)
                                    fired = true
                                }
                            } else {
                                // Lift. A short, near-stationary press is a tap; two in 300ms is a
                                // double-tap.
                                val now = ch.uptimeMillis
                                val moved = (ch.position - startPos).getDistance()
                                if (!fired && moved <= tapSlop && now - startTime <= 250) {
                                    val nearLast = (ch.position - lastTapPos).getDistance() <= tapSlop * 3f
                                    if (now - lastTapTime <= 300 && nearLast) {
                                        if (holdMode) toggleLatch(dtap) else pulse(dtap)
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
                }
            },
    )
}
