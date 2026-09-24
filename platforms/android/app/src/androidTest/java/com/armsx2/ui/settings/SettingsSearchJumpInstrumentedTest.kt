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
    fun searchOpensOnlyMatchingCollapsedSectionAndNormalCollapseStillWorks() {
        var target by mutableStateOf<String?>("Merge Sprite")
        composeRule.setContent {
            MaterialTheme {
                CompositionLocalProvider(LocalSettingsSearchTarget provides target) {
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
        composeRule.runOnIdle { target = null }
        composeRule.waitForIdle()
        composeRule.onNodeWithText("Merge Sprite").assertIsDisplayed()
        composeRule.onNodeWithText("GPU Target CLUT").assertDoesNotExist()
        assertTrue(SettingsControllerNav.selectByLabel("Merge Sprite"))

        composeRule.onNodeWithText("Upscaling Fixes").performClick()
        composeRule.waitForIdle()
        composeRule.onNodeWithText("Merge Sprite").assertDoesNotExist()
        assertFalse(SettingsControllerNav.selectByLabel("Merge Sprite"))
    }

    @Test
    fun sectionTitleResultOpensAndFocusesItsHeader() {
        var target by mutableStateOf<String?>("Hardware Fixes")
        composeRule.setContent {
            MaterialTheme {
                CompositionLocalProvider(LocalSettingsSearchTarget provides target) {
                    CollapsibleSection("Hardware Fixes") {
                        ToggleRow("GPU Target CLUT", false, onChange = {})
                    }
                }
            }
        }

        composeRule.waitForIdle()
        composeRule.runOnIdle { target = null }
        composeRule.waitForIdle()
        composeRule.onNodeWithText("GPU Target CLUT").assertIsDisplayed()
        assertTrue(SettingsControllerNav.selectByLabel("Hardware Fixes"))
    }
}
