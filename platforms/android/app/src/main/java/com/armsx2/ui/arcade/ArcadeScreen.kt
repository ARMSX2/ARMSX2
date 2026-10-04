// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.arcade

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.i18n.str
import com.armsx2.navigation.AppRoute
import com.armsx2.navigation.UiNavigator
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.ControllerAutoScroll
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.theme.Success

/**
 * NAMCO System 246/256: what every arcade game needs, and the games the library found. The boot files
 * they start from, the board's BIOS, then the games themselves, which stay where the player keeps them:
 * an image named after its game with its dongle beside it, or a folder laid out by PCSX2x6's template,
 * anywhere in the game folders. Nothing is copied but the dongle, into the memory cards folder, at the
 * game's first start.
 */
@Composable
fun ArcadeScreen(onBack: () -> Unit, viewModel: ArcadeViewModel = viewModel()) {
    val state = viewModel.state.value
    val nativeReady = MainActivityRuntime.nativeReady.value
    // Again once the core is up: the games' names come from its database.
    LaunchedEffect(nativeReady) { viewModel.refresh() }

    val bootReady = state.bootGames > 0
    val scroll = rememberScrollState()
    ControllerAutoScroll(scroll)
    ArmsBackdrop {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(scroll)
                .padding(horizontal = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            ArmsTopBar(
                title = str("arcade.title"),
                leading = { RoundAction("←", str("action.back"), onBack) },
                horizontalPadding = 0.dp,
            )
            Text(
                str("arcade.intro"),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 6.dp),
            )
            val percent = (viewModel.downloadProgress.floatValue * 100).toInt()
            StepCard(
                number = 1,
                title = str("arcade.step.boot"),
                description = str("arcade.step.boot.desc"),
                status = when {
                    state.downloading -> str("arcade.boot.downloading") + " $percent%"
                    bootReady -> str("arcade.boot.ready").format(state.bootGames)
                    else -> str("arcade.boot.none")
                },
                done = bootReady && !state.downloading,
                button = if (bootReady) str("arcade.boot.redownload") else str("arcade.boot.download"),
                id = "arcade.boot",
                enabled = !state.downloading,
                progress = if (state.downloading) viewModel.downloadProgress.floatValue else null,
                onClick = viewModel::downloadBootFiles,
            )
            StepCard(
                number = 2,
                title = str("arcade.step.bios"),
                description = str("arcade.step.bios.desc"),
                status = state.bios?.let { str("arcade.bios.found").format(it) } ?: str("arcade.bios.none"),
                done = state.bios != null,
                button = str("arcade.bios.open"),
                id = "arcade.bios",
                onClick = { UiNavigator.navigate(AppRoute.BiosManager(returnToArcade = true)) },
            )
            StepCard(
                number = 3,
                title = str("arcade.step.add"),
                description = str("arcade.step.add.desc"),
                status = when {
                    state.looking -> str("arcade.games.looking")
                    state.games.isEmpty() -> str("arcade.games.none")
                    else -> str("arcade.games.found").format(state.games.size)
                },
                done = state.games.isNotEmpty() && !state.looking,
                button = str("arcade.games.lookAgain"),
                id = "arcade.look",
                enabled = !state.looking,
                onClick = viewModel::lookAgain,
            )
            ArcadeGames(state.games)
            Spacer(Modifier.height(12.dp))
        }
    }

    state.error?.let { text ->
        com.armsx2.ui.common.NotifyOverlay(
            title = str("arcade.title"),
            message = text,
            onDismiss = viewModel::dismissMessage,
            idPrefix = "arcade.message",
        )
    }
}

@Composable
private fun StepCard(
    number: Int,
    title: String,
    description: String,
    status: String,
    done: Boolean,
    button: String,
    id: String,
    enabled: Boolean = true,
    progress: Float? = null,
    onClick: () -> Unit,
) {
    GlassPanel(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Surface(
                    modifier = Modifier.size(34.dp),
                    shape = CircleShape,
                    color = (if (done) Success else MaterialTheme.colorScheme.primary).copy(alpha = 0.16f),
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Text(
                            if (done) "✓" else number.toString(),
                            fontWeight = FontWeight.Bold,
                            color = if (done) Success else MaterialTheme.colorScheme.primary,
                        )
                    }
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                    Text(
                        description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    status,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (done) Success else MaterialTheme.colorScheme.onSurface,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(10.dp))
                OutlinedButton(
                    onClick = onClick,
                    enabled = enabled,
                    shape = RoundedCornerShape(12.dp),
                    modifier = Modifier.controllerFocusable(
                        if (enabled) id else null,
                        RoundedCornerShape(12.dp),
                        onConfirm = onClick,
                    ),
                ) { Text(button) }
            }
            if (progress != null) LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
        }
    }
}

/** The arcade games the library found, each with what still keeps it from starting. The rows take the
 *  pad's selection, so a long list scrolls into view with it. */
@Composable
private fun ArcadeGames(games: List<ArcadeGame>) {
    if (games.isEmpty()) return
    GlassPanel(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(str("arcade.step.games"), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            val ready = str("arcade.games.ready")
            val missing = str("arcade.games.missing")
            val attract = str("arcade.games.attract")
            games.forEachIndexed { index, g ->
                Surface(
                    modifier = Modifier
                        .fillMaxWidth()
                        .controllerFocusable("arcade.games.$index", RoundedCornerShape(12.dp)),
                    shape = RoundedCornerShape(12.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
                ) {
                    Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(g.title, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(g.id, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Spacer(Modifier.width(10.dp))
                        // A game that only reaches its attract demo is not "ready to play", nor broken.
                        Text(
                            when {
                                g.missing.isNotEmpty() -> missing.format(g.missing.joinToString(", "))
                                g.attractOnly -> attract
                                else -> ready
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = when {
                                g.missing.isNotEmpty() -> MaterialTheme.colorScheme.error
                                g.attractOnly -> MaterialTheme.colorScheme.onSurfaceVariant
                                else -> Success
                            },
                        )
                    }
                }
            }
        }
    }
}
