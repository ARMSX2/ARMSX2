package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class SettingsSearchJumpInstrumentedTest {
    @get:Rule val composeRule = createComposeRule()

    @Test
    fun jumpOpensOnlyTheNamedSectionAndNormalCollapseStillWorks() {
        // The provider is non-empty only while the jump is resolving, exactly as SettingsScreen
        // supplies it; clearing it is what ends the jump.
        var openSections by mutableStateOf(setOf("Upscaling Fixes"))
        composeRule.setContent {
            MaterialTheme {
                CompositionLocalProvider(LocalSettingsSearchOpenSections provides openSections) {
                    Column {
                        CollapsibleSection("Upscaling Fixes") {
                            ToggleRow("Merge Sprite", false, onChange = {})
                        }
                        CollapsibleSection("Hardware Fixes") {
                            ToggleRow("GPU Target CLUT", false, onChange = {})
                        }
                    }
                }
            }
        }

        composeRule.waitForIdle()
        composeRule.onNodeWithText("Merge Sprite").assertIsDisplayed()
        composeRule.onNodeWithText("GPU Target CLUT").assertDoesNotExist()
        assertTrue(SettingsControllerNav.selectByLabel("Merge Sprite"))
        // A setting behind a section the jump did not open stays unreachable for the pad.
        assertFalse(SettingsControllerNav.selectByLabel("GPU Target CLUT"))

        // The jump is over, so the open-sections set is empty and a manual collapse sticks.
        composeRule.runOnIdle { openSections = emptySet() }
        composeRule.onNodeWithText("Upscaling Fixes").performClick()
        composeRule.waitForIdle()
        composeRule.onNodeWithText("Merge Sprite").assertDoesNotExist()
        assertFalse(SettingsControllerNav.selectByLabel("Merge Sprite"))
    }

    @Test
    fun sectionTitleResultOpensAndFocusesItsHeader() {
        composeRule.setContent {
            MaterialTheme {
                CompositionLocalProvider(LocalSettingsSearchOpenSections provides setOf("Hardware Fixes")) {
                    CollapsibleSection("Hardware Fixes") {
                        ToggleRow("GPU Target CLUT", false, onChange = {})
                    }
                }
            }
        }

        composeRule.waitForIdle()
        composeRule.onNodeWithText("GPU Target CLUT").assertIsDisplayed()
        assertTrue(SettingsControllerNav.selectByLabel("Hardware Fixes"))
    }
}
