package com.armsx2.input

/** Remembers the physical buttons that started a hold action until either is released. */
internal class HotkeyHoldState {
    private var keys: Set<Int> = emptySet()

    fun start(mainKey: Int, modifierKey: Int?) {
        keys = if (modifierKey == null) setOf(mainKey) else setOf(mainKey, modifierKey)
    }

    /** Returns true exactly once when a button belonging to the active hold is released. */
    fun release(key: Int): Boolean {
        if (key !in keys) return false
        keys = emptySet()
        return true
    }

    fun clear() {
        keys = emptySet()
    }
}
