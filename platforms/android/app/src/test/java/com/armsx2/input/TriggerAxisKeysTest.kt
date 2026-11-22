// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.input

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TriggerAxisKeysTest {
    private val pad = 7
    private val other = 8

    private fun TriggerAxisKeys.down(dev: Int = pad, left: Boolean = false, repeat: Boolean = false, keyEdges: Boolean = false) =
        drop(dev, left, down = true, repeat = repeat, needsKeyEdges = keyEdges)

    private fun TriggerAxisKeys.up(dev: Int = pad, left: Boolean = false) =
        drop(dev, left, down = false, repeat = false, needsKeyEdges = false)

    // A pad that reports the trigger both as an axis and as a key: once the axis has moved, the
    // key press, its repeats and its release never reach the pad, whatever the binding target.
    @Test fun keyFromPadWithMovingAxisIsDropped() {
        val keys = TriggerAxisKeys()
        keys.axisMoved(pad, left = false)
        assertTrue(keys.down())
        assertTrue(keys.down(repeat = true))
        assertTrue(keys.up())
    }

    // A pad whose trigger is a key only (no axis, or an axis that never moves) keeps its key.
    @Test fun keyOnlyPadKeepsItsKey() {
        val keys = TriggerAxisKeys()
        assertFalse(keys.down())
        assertFalse(keys.down(repeat = true))
        assertFalse(keys.up())
    }

    // Turbo and tap-to-hold act on key edges, which the axis path does not produce.
    @Test fun turboOrTapToHoldTargetKeepsItsKey() {
        val keys = TriggerAxisKeys()
        keys.axisMoved(pad, left = true)
        assertFalse(keys.down(left = true, keyEdges = true))
        assertFalse(keys.up(left = true))
    }

    // The first press of a session can reach the key path before the axis has reported travel.
    // That press keeps the key path for its repeats and its release, so nothing stays held; the
    // next press goes to the axis.
    @Test fun pressThatReachedThePadIsReleasedThroughIt() {
        val keys = TriggerAxisKeys()
        assertFalse(keys.down())
        keys.axisMoved(pad, left = false)
        assertFalse(keys.down(repeat = true))
        assertFalse(keys.up())
        assertTrue(keys.down())
        assertTrue(keys.up())
    }

    // Each side and each device is learned on its own.
    @Test fun sidesAndDevicesAreIndependent() {
        val keys = TriggerAxisKeys()
        keys.axisMoved(pad, left = true)
        assertFalse(keys.down(left = false))
        assertFalse(keys.up(left = false))
        assertFalse(keys.down(dev = other, left = true))
        assertFalse(keys.up(dev = other, left = true))
        assertTrue(keys.down(left = true))
        assertTrue(keys.up(left = true))
    }

    // A fresh press decides again even if the previous release never arrived.
    @Test fun freshPressDecidesAgainAfterALostRelease() {
        val keys = TriggerAxisKeys()
        assertFalse(keys.down())
        keys.axisMoved(pad, left = false)
        assertTrue(keys.down())
        assertTrue(keys.up())
    }

    // A release whose press never reached this filter (a hotkey took it, or the game was paused)
    // must not zero the value the axis is sending; on a key-only pad it still goes through.
    @Test fun unpairedReleaseFollowsTheAxis() {
        val keys = TriggerAxisKeys()
        assertFalse(keys.up())
        keys.axisMoved(pad, left = false)
        assertTrue(keys.up())
    }

    // A removed or changed device starts over: its key works until its axis moves again.
    @Test fun forgetDeviceStartsOver() {
        val keys = TriggerAxisKeys()
        keys.axisMoved(pad, left = false)
        keys.forgetDevice(pad)
        assertFalse(keys.down())
        assertFalse(keys.up())
    }
}
