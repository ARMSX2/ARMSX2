package com.armsx2.ui

import com.armsx2.EmuState
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GameplayBackTest {
    @Test fun runningGameOwnsBack() {
        assertTrue(handlesGameplayBack(EmuState.RUNNING, frontendCovers = false))
    }

    @Test fun pausedGameWithoutMenuOwnsBack() {
        assertTrue(handlesGameplayBack(EmuState.PAUSED, frontendCovers = false))
    }

    @Test fun menusAndOtherOverlaysKeepTheirOwnBackNavigation() {
        for (state in EmuState.entries) {
            assertFalse(handlesGameplayBack(state, frontendCovers = true))
        }
    }

    @Test fun frontendAndUnsupportedStatesDoNotOpenGameMenu() {
        for (state in listOf(EmuState.STOPPED, EmuState.EMULATOR_UNSUPPORTED, EmuState.RENDER_UNSUPPORTED)) {
            assertFalse(handlesGameplayBack(state, frontendCovers = false))
        }
    }
}
