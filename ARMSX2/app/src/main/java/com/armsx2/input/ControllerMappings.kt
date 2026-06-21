package com.armsx2.input

import android.view.KeyEvent
import androidx.compose.runtime.mutableStateOf
import com.armsx2.Main

object ControllerMappings {
    data class Action(
        val id: String,
        val label: String,
        val targetKeyCode: Int,
        val defaultPhysicalKeyCode: Int,
    )

    val actions = listOf(
        Action("dpad_up", "D-Pad Up", KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_UP),
        Action("dpad_down", "D-Pad Down", KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_DOWN),
        Action("dpad_left", "D-Pad Left", KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_LEFT),
        Action("dpad_right", "D-Pad Right", KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_RIGHT),
        Action("cross", "Cross", KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_A),
        Action("circle", "Circle", KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_B),
        Action("square", "Square", KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_X),
        Action("triangle", "Triangle", KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_Y),
        Action("l1", "L1", KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L1),
        Action("r1", "R1", KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_R1),
        Action("l2", "L2", KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_L2),
        Action("r2", "R2", KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_R2),
        Action("l3", "L3", KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBL),
        Action("r3", "R3", KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_THUMBR),
        Action("select", "Select", KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_SELECT),
        Action("start", "Start", KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_START),
    )

    private const val KEY_PREFIX = "pad.map."

    fun physicalFor(action: Action): Int =
        Main.prefs.getInt(KEY_PREFIX + action.id, action.defaultPhysicalKeyCode)

    fun labelForKey(keyCode: Int): String = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> "D-Pad Up"
        KeyEvent.KEYCODE_DPAD_DOWN -> "D-Pad Down"
        KeyEvent.KEYCODE_DPAD_LEFT -> "D-Pad Left"
        KeyEvent.KEYCODE_DPAD_RIGHT -> "D-Pad Right"
        KeyEvent.KEYCODE_BUTTON_A -> "Button A"
        KeyEvent.KEYCODE_BUTTON_B -> "Button B"
        KeyEvent.KEYCODE_BUTTON_X -> "Button X"
        KeyEvent.KEYCODE_BUTTON_Y -> "Button Y"
        KeyEvent.KEYCODE_BUTTON_L1 -> "L1"
        KeyEvent.KEYCODE_BUTTON_R1 -> "R1"
        KeyEvent.KEYCODE_BUTTON_L2 -> "L2"
        KeyEvent.KEYCODE_BUTTON_R2 -> "R2"
        KeyEvent.KEYCODE_BUTTON_THUMBL -> "L3"
        KeyEvent.KEYCODE_BUTTON_THUMBR -> "R3"
        KeyEvent.KEYCODE_BUTTON_SELECT -> "Select"
        KeyEvent.KEYCODE_BUTTON_START -> "Start"
        else -> KeyEvent.keyCodeToString(keyCode).removePrefix("KEYCODE_")
    }

    fun bind(action: Action, physicalKeyCode: Int) {
        Main.prefs.edit().putInt(KEY_PREFIX + action.id, physicalKeyCode).apply()
    }

    fun reset() {
        val edit = Main.prefs.edit()
        actions.forEach { edit.remove(KEY_PREFIX + it.id) }
        edit.apply()
    }

    fun targetForPhysical(physicalKeyCode: Int): Int? =
        actions.firstOrNull { physicalFor(it) == physicalKeyCode }?.targetKeyCode

    // ---- System hotkeys (menu / quick save / quick load) -----------------
    // Physical buttons bound to app actions, NOT forwarded to the PS2. Handled in
    // Main.dispatchKeyEvent (so they can catch KEYCODE_BACK / back-paddle keys the
    // back dispatcher would otherwise swallow). KEYCODE_UNKNOWN = unbound.
    enum class SysHotkey(val prefKey: String, val label: String) {
        MENU("pad.menu.keycode", "Menu / Pause"),
        SAVE_STATE("pad.savestate.keycode", "Quick Save State"),
        LOAD_STATE("pad.loadstate.keycode", "Quick Load State"),
        CYCLE_SLOT("pad.cycleslot.keycode", "Cycle Save Slot"),
        TEXTURE_DUMP("pad.texdump.keycode", "Toggle Texture Dumping"),
        FAST_FORWARD("pad.fastforward.keycode", "Fast Forward (hold)"),
        RES_UP("pad.resup.keycode", "Increase Resolution"),
        RES_DOWN("pad.resdown.keycode", "Decrease Resolution"),
        ACHIEVEMENTS("pad.achievements.keycode", "Open Achievements"),
        CLOSE_GAME("pad.closegame.keycode", "Close Game"),
    }

    fun hotkeyCode(h: SysHotkey): Int =
        Main.prefs.getInt(h.prefKey, KeyEvent.KEYCODE_UNKNOWN)

    fun bindHotkey(h: SysHotkey, physicalKeyCode: Int) {
        Main.prefs.edit().putInt(h.prefKey, physicalKeyCode).apply()
    }

    fun clearHotkey(h: SysHotkey) {
        Main.prefs.edit().putInt(h.prefKey, KeyEvent.KEYCODE_UNKNOWN).apply()
    }

    /** Which hotkey (if any) [physicalKeyCode] is bound to. */
    fun hotkeyFor(physicalKeyCode: Int): SysHotkey? {
        if (physicalKeyCode == KeyEvent.KEYCODE_UNKNOWN) return null
        return SysHotkey.values().firstOrNull { hotkeyCode(it) == physicalKeyCode }
    }

    // Capture bridge: PadTab sets [captureHotkey]; the next key seen by
    // Main.dispatchKeyEvent is bound to it. Observed for UI feedback.
    val captureHotkey = mutableStateOf<SysHotkey?>(null)

    /** Bumped after a (re)bind so observing UI recomposes. */
    val hotkeyBindTick = mutableStateOf(0)
}
