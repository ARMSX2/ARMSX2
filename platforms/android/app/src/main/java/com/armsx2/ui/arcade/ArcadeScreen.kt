// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.arcade

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.arcade.ArcadeLibrary
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

/** Soul Calibur II: the one game with a second memory card, its Conquest card. */
private const val SOUL_CALIBUR_II = "NM00007"

/** The game being imported, kept across the activity being recreated while a picker is open. */
private val TitleSaver = Saver<ArcadeLibrary.Title?, List<String>>(
    save = { t -> t?.let { arrayListOf(it.id, it.name, it.board, it.media) } },
    restore = { l -> if (l.size == 4) ArcadeLibrary.Title(l[0], l[1], l[2], l[3]) else null },
)

/**
 * NAMCO System 246/256: everything an arcade game needs, in the order it needs it. A folder for the
 * games, the boot files they start from, the board's BIOS, then one import per game (the game, its
 * image, its dongle), after which the library shows it with no refresh to tap.
 */
@Composable
fun ArcadeScreen(onBack: () -> Unit, viewModel: ArcadeViewModel = viewModel()) {
    val state = viewModel.state.value
    val nativeReady = MainActivityRuntime.nativeReady.value
    // Again once the core is up: the list of games comes from its database.
    LaunchedEffect(nativeReady) { viewModel.refresh() }

    // An import is a game, then its image, then its dongle (then Soul Calibur II's Conquest card).
    // Saveable: a picker can outlive the activity, and its answer still belongs to this import.
    var choosingGame by rememberSaveable { mutableStateOf(false) }
    var game by rememberSaveable(stateSaver = TitleSaver) { mutableStateOf<ArcadeLibrary.Title?>(null) }
    var image by rememberSaveable { mutableStateOf<Uri?>(null) }
    var dongle by rememberSaveable { mutableStateOf<Uri?>(null) }
    var askCard by rememberSaveable { mutableStateOf(false) }

    fun startImport(card: Uri?) {
        val g = game
        val i = image
        val d = dongle
        game = null
        image = null
        dongle = null
        if (g != null && i != null && d != null) viewModel.import(g, i, d, card)
    }

    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let(viewModel::chooseFolder)
    }
    val cardPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        startImport(uri)
    }
    val donglePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) {
            game = null
            image = null
            return@rememberLauncherForActivityResult
        }
        dongle = uri
        if (game?.id == SOUL_CALIBUR_II) askCard = true else startImport(null)
    }
    val imagePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) {
            game = null
            return@rememberLauncherForActivityResult
        }
        image = uri
        donglePicker.launch(arrayOf("*/*"))
    }

    val folderReady = state.folderName != null
    val bootReady = state.bootGames > 0
    // The list of games comes from the core's database, there a moment after the app starts.
    val importReady = folderReady && bootReady && state.importing == null && state.titles.isNotEmpty()

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
            StepCard(
                number = 1,
                title = str("arcade.step.folder"),
                description = str("arcade.step.folder.desc"),
                status = state.folderName ?: str("arcade.folder.none"),
                done = folderReady,
                button = if (folderReady) str("arcade.folder.change") else str("arcade.folder.choose"),
                id = "arcade.folder",
                onClick = { folderPicker.launch(null) },
            )
            val percent = (viewModel.downloadProgress.floatValue * 100).toInt()
            StepCard(
                number = 2,
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
                number = 3,
                title = str("arcade.step.bios"),
                description = str("arcade.step.bios.desc"),
                status = state.bios?.let { str("arcade.bios.found").format(it) } ?: str("arcade.bios.none"),
                done = state.bios != null,
                button = str("arcade.bios.open"),
                id = "arcade.bios",
                onClick = { UiNavigator.navigate(AppRoute.BiosManager(returnToArcade = true)) },
            )
            StepCard(
                number = 4,
                title = str("arcade.step.import"),
                description = str("arcade.step.import.desc"),
                status = if (folderReady && bootReady) str("arcade.import.ready") else str("arcade.import.needs"),
                done = false,
                button = str("arcade.import.button"),
                id = "arcade.import",
                enabled = importReady,
                onClick = { choosingGame = true },
            )
            InstalledGames(state.installed)
            Spacer(Modifier.height(12.dp))
        }
    }

    if (choosingGame) {
        GameChooser(
            titles = state.titles,
            onPick = { picked ->
                choosingGame = false
                game = picked
                imagePicker.launch(arrayOf("*/*"))
            },
            onDismiss = { choosingGame = false },
        )
    }
    if (askCard) {
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("arcade.import.card"),
            message = str("arcade.import.card.desc"),
            confirmLabel = str("arcade.import.card.add"),
            dismissLabel = str("arcade.import.card.skip"),
            idPrefix = "arcade.card",
            onConfirm = {
                askCard = false
                cardPicker.launch(arrayOf("*/*"))
            },
            onDismiss = {
                askCard = false
                startImport(null)
            },
        )
    }
    state.importing?.let { importing ->
        ImportProgress(importing.name, viewModel.importProgress.floatValue)
    }
    (state.error ?: state.message)?.let { text ->
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

@Composable
private fun InstalledGames(games: List<ArcadeLibrary.Installed>) {
    GlassPanel(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(str("arcade.step.games"), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            if (games.isEmpty()) {
                Text(
                    str("arcade.games.none"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            val partNames = mapOf(
                ArcadeLibrary.Part.DONGLE to str("arcade.part.dongle"),
                ArcadeLibrary.Part.CARD to str("arcade.part.card"),
                ArcadeLibrary.Part.BOOT to str("arcade.part.boot"),
            )
            val ready = str("arcade.games.ready")
            val missing = str("arcade.games.missing")
            games.forEach { g ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(g.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(g.id, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Text(
                        if (g.missing.isEmpty()) ready else missing.format(g.missing.joinToString(", ") { partNames[it].orEmpty() }),
                        style = MaterialTheme.typography.bodySmall,
                        color = if (g.missing.isEmpty()) Success else MaterialTheme.colorScheme.error,
                    )
                }
            }
        }
    }
}

/** Which game to import. A plain scrolling Column: the pad's nav registry only knows rows that are
 *  composed, and the list is bounded by the database. */
@Composable
private fun GameChooser(
    titles: List<ArcadeLibrary.Title>,
    onPick: (ArcadeLibrary.Title) -> Unit,
    onDismiss: () -> Unit,
) {
    val layer = "arcade-pick"
    com.armsx2.ui.common.PadModal(key = layer, onDismiss = onDismiss) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 460.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier
                    .padding(20.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.82f).dp),
            ) {
                Text(str("arcade.import.pick"), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(4.dp))
                Text(
                    str("arcade.import.pick.desc"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(10.dp))
                Column(
                    Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    titles.forEach { t ->
                        Surface(
                            onClick = { onPick(t) },
                            modifier = Modifier
                                .fillMaxWidth()
                                .controllerFocusable("$layer.${t.id}", RoundedCornerShape(14.dp), onConfirm = { onPick(t) }),
                            shape = RoundedCornerShape(14.dp),
                            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                        ) {
                            Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
                                Text(t.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Text(
                                    listOf(t.id, ArcadeLibrary.boardName(t.board), t.media).filter { it.isNotBlank() }.joinToString("  ·  "),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
                Spacer(Modifier.height(14.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    OutlinedButton(
                        onClick = onDismiss,
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier.controllerFocusable("$layer.cancel", RoundedCornerShape(12.dp), onConfirm = onDismiss),
                    ) { Text(str("action.cancel")) }
                }
            }
        }
    }
}

/** The copy, which for a DVD or hard drive image takes a while. Nothing to press: it ends by itself. */
@Composable
private fun ImportProgress(name: String, progress: Float) {
    com.armsx2.ui.common.PadModal(key = "arcade-importing", onDismiss = null) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 420.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(str("arcade.import.copying").format(name), style = MaterialTheme.typography.titleMedium)
                LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
                Text(
                    "${(progress * 100).toInt()}%",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
