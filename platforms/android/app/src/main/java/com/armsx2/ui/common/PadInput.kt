package com.armsx2.ui.common

import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.compose.runtime.mutableStateOf
import kotlin.math.abs

/**
 * Whether the controller is what drives the frontend right now: set by pad input, cleared by a
 * touch. Selection highlights follow it.
 *
 * Without it they followed state that is always there whatever the user is holding: the cover
 * grid's selected index (0 from the start), and Compose focus, which Android hands to the first
 * focusable control once the game surface lets go of it, so the library's "Clear All" wore a focus
 * ring nobody had put there. Android's own touch mode cannot stand in: the D-pad of the Odin 3 and
 * similar handhelds arrives as a HAT axis, never as a key, and a motion event does not leave touch
 * mode.
 *
 * Fed first thing from MainActivityRuntime's dispatchKeyEvent / dispatchGenericMotionEvent /
 * dispatchTouchEvent, all on the main thread, so it is current before any screen sees the event.
 */
object PadInput {
    val active = mutableStateOf(false)

    /** True while the event being dispatched is the one that switched from touch to the pad. A
     *  screen can spend that press showing where its selection is, since nothing was highlighted
     *  until now, instead of moving it (or launching it) somewhere nobody could see. */
    var justActivated = false
        private set

    fun onKey(event: KeyEvent) {
        justActivated = false
        // The navigation bar's Back is a virtual key; the hardware volume and power keys say
        // nothing about who is driving the menus.
        if (event.action != KeyEvent.ACTION_DOWN || event.deviceId == KeyCharacterMap.VIRTUAL_KEYBOARD) return
        when (event.keyCode) {
            KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE,
            KeyEvent.KEYCODE_POWER, KeyEvent.KEYCODE_CAMERA, KeyEvent.KEYCODE_FOCUS,
            KeyEvent.KEYCODE_HEADSETHOOK, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY, KeyEvent.KEYCODE_MEDIA_PAUSE,
            KeyEvent.KEYCODE_MEDIA_NEXT, KeyEvent.KEYCODE_MEDIA_PREVIOUS -> return
        }
        activate()
    }

    fun onMotion(event: MotionEvent) {
        if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
            !event.isFromSource(InputDevice.SOURCE_GAMEPAD) &&
            !event.isFromSource(InputDevice.SOURCE_DPAD)
        ) return
        justActivated = false
        // Past the deadzone only: a resting stick reports small values all the time.
        if (AXES.any { abs(event.getAxisValue(it)) > 0.5f }) activate()
    }

    fun onTouch(event: MotionEvent) {
        if (event.actionMasked == MotionEvent.ACTION_DOWN && active.value) active.value = false
    }

    private fun activate() {
        if (active.value) return
        active.value = true
        justActivated = true
    }

    private val AXES = intArrayOf(
        MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
    )
}
