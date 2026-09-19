package com.armsx2.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.Colors

/**
 * RetroAchievements as a standard setting, so it can be set per game like any other: off in the
 * global settings and on in the games you want it for, or the other way round.
 *
 * Only the on/off switch lives here. The account, hardcore mode and the notification options stay
 * on the RetroAchievements screen this page links to; they are properties of the account and the
 * session rather than of a game.
 */
@Composable
fun AchievementsSettingsTab(state: MutableState<Settings>) {
    val s = state.value
    val perGame = InGameOverlay.settingsScope.value == SettingsScope.Game

    Column(modifier = Modifier.fillMaxWidth()) {
        HelpText(str(if (perGame) "ra.settings.help.game" else "ra.settings.help.global"))
        ToggleRow(
            str(if (perGame) "ra.enable.thisGame" else "ra.enable.label"),
            s.achievementsEnabled,
            description = str("ra.enable.description"),
        ) { InGameOverlay.saveSettings(s.copy(achievementsEnabled = it)) }
        SettingsDivider()
        AchievementsScreenRow()
    }
}

/** Opens the RetroAchievements screen: the account, hardcore mode and notification options. */
@Composable
private fun AchievementsScreenRow() {
    val open = {
        if (com.armsx2.ui.WindowImpl.inGameScreen.value != null)
            com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.Achievements)
        else
            com.armsx2.navigation.UiNavigator.navigate(com.armsx2.navigation.AppRoute.Achievements)
    }
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .controllerFocusable("settings.ra.open", RoundedCornerShape(16.dp), onConfirm = open)
            .clickable(onClick = open)
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(
                str("ra.viewAchievements"),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                str("ra.settings.openScreen.description"),
                color = Colors.pasx2_blue,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}
