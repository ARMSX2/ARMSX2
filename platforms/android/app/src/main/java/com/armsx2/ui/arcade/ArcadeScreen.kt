// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.arcade

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
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
 * games, the boot files they start from, the board's BIOS, then the games: each one's image and dongle
 * imported on their own, after which the library shows it with no refresh to tap.
 */
@Composable
fun ArcadeScreen(onBack: () -> Unit, viewModel: ArcadeViewModel = viewModel()) {
    val state = viewModel.state.value
    val nativeReady = MainActivityRuntime.nativeReady.value
    // Again once the core is up: the list of games comes from its database.
    LaunchedEffect(nativeReady) { viewModel.refresh() }

    // An import is a game, then each of its parts on its own (its image, its dongle, Soul Calibur II's
    // Conquest card), each from a picker of its own. Saveable: a picker can outlive the activity, and
    // its answer still belongs to this game and part.
    var choosingGame by rememberSaveable { mutableStateOf(false) }
    var game by rememberSaveable(stateSaver = TitleSaver) { mutableStateOf<ArcadeLibrary.Title?>(null) }
    var picking by rememberSaveable { mutableStateOf<ArcadeLibrary.Part?>(null) }
    var confirmUninstall by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(game?.id) { viewModel.showFiles(game?.id) }

    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let(viewModel::chooseFolder)
    }
    val partPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val g = game
        val part = picking
        picking = null
        if (uri != null && g != null && part != null) viewModel.import(g, part, uri)
    }

    val folderReady = state.folderName != null
    val bootReady = state.bootGames > 0
    // The list of games comes from the core's database, there a moment after the app starts.
    val busy = state.importing != null || state.removing != null
    val importReady = folderReady && bootReady && !busy && state.titles.isNotEmpty()

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
            InstalledGames(
                games = state.installed,
                enabled = !busy,
                onOpen = { g -> game = ArcadeLibrary.Title(g.id, g.name, g.board, g.media) },
            )
            Spacer(Modifier.height(12.dp))
        }
    }

    if (choosingGame) {
        GameChooser(
            titles = state.titles,
            onPick = { picked ->
                choosingGame = false
                game = picked
            },
            onDismiss = { choosingGame = false },
        )
    }
    game?.let { g ->
        val files = state.files?.takeIf { it.id == g.id }
        GameImports(
            title = g,
            files = files,
            enabled = !busy,
            onImport = { part ->
                picking = part
                partPicker.launch(arrayOf("*/*"))
            },
            onUninstall = if (files?.exists == true) ({ confirmUninstall = true }) else null,
            onClose = {
                confirmUninstall = false
                game = null
            },
        )
        if (confirmUninstall) {
            com.armsx2.ui.common.ConfirmOverlay(
                title = str("arcade.uninstall.title").format(g.name),
                message = str("arcade.uninstall.desc"),
                confirmLabel = str("arcade.uninstall.button"),
                destructive = true,
                idPrefix = "arcade.uninstall",
                onConfirm = {
                    confirmUninstall = false
                    game = null
                    viewModel.uninstall(g)
                },
                onDismiss = { confirmUninstall = false },
            )
        }
    }
    state.importing?.let { importing ->
        Progress(
            "arcade-importing", str("arcade.import.copying").format(importing.name), viewModel.importProgress.floatValue,
            onCancel = viewModel::cancelImport,
        )
    }
    state.removing?.let { removing ->
        Progress("arcade-removing", str("arcade.uninstall.progress").format(removing.name), viewModel.removeProgress.floatValue)
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

/** The games in the arcade folder, each opening its imports: to replace a part, or to uninstall it. */
@Composable
private fun InstalledGames(
    games: List<ArcadeLibrary.Installed>,
    enabled: Boolean,
    onOpen: (ArcadeLibrary.Installed) -> Unit,
) {
    GlassPanel(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(str("arcade.step.games"), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                str(if (games.isEmpty()) "arcade.games.none" else "arcade.games.hint"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val partNames = mapOf(
                ArcadeLibrary.Part.DONGLE to str("arcade.part.dongle"),
                ArcadeLibrary.Part.CARD to str("arcade.part.card"),
                ArcadeLibrary.Part.BOOT to str("arcade.part.boot"),
            )
            val ready = str("arcade.games.ready")
            val missing = str("arcade.games.missing")
            games.forEach { g ->
                Surface(
                    onClick = { onOpen(g) },
                    enabled = enabled,
                    modifier = Modifier
                        .fillMaxWidth()
                        .controllerFocusable(
                            if (enabled) "arcade.games.${g.id}" else null,
                            RoundedCornerShape(12.dp),
                            onConfirm = { onOpen(g) },
                        ),
                    shape = RoundedCornerShape(12.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
                ) {
                    Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(g.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(g.id, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Text(
                            if (g.missing.isEmpty()) ready else missing.format(g.missing.joinToString(", ") { partNames[it].orEmpty() }),
                            style = MaterialTheme.typography.bodySmall,
                            color = if (g.missing.isEmpty()) Success else MaterialTheme.colorScheme.error,
                        )
                        Spacer(Modifier.width(10.dp))
                        Text("›", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
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

/**
 * One game's imports: its image (disc or hard drive), its dongle and, for Soul Calibur II, its Conquest
 * card. Each from a picker of its own, in any order, each showing whether it is in and, under them all,
 * what the game still needs. [onUninstall], when the game is in the folder at all, removes it again.
 */
@Composable
private fun GameImports(
    title: ArcadeLibrary.Title,
    files: ArcadeLibrary.GameFiles?,
    enabled: Boolean,
    onImport: (ArcadeLibrary.Part) -> Unit,
    onUninstall: (() -> Unit)?,
    onClose: () -> Unit,
) {
    val layer = "arcade-game"
    val drive = title.media.equals("HDD", ignoreCase = true)
    // Blank while the folder is still being looked at, rather than a moment of "not imported".
    val none = if (files == null) "" else str("arcade.import.none")
    val choose = str("arcade.import.choose")
    val replace = str("arcade.import.replace")
    com.armsx2.ui.common.PadModal(key = layer, onDismiss = onClose) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 520.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier
                    .padding(20.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.86f).dp),
            ) {
                Text(title.name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                Text(
                    listOf(title.id, ArcadeLibrary.boardName(title.board), title.media).filter { it.isNotBlank() }.joinToString("  ·  "),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    str("arcade.import.parts.desc"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(10.dp))
                Column(
                    Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    StepCard(
                        number = 1,
                        title = str(if (drive) "arcade.import.drive" else "arcade.import.disc"),
                        description = str(if (drive) "arcade.import.drive.desc" else "arcade.import.disc.desc"),
                        status = files?.image ?: none,
                        done = files?.image != null,
                        button = if (files?.image != null) replace else choose,
                        id = "$layer.image",
                        enabled = enabled,
                        onClick = { onImport(ArcadeLibrary.Part.IMAGE) },
                    )
                    StepCard(
                        number = 2,
                        title = str("arcade.import.dongle"),
                        description = str("arcade.import.dongle.desc"),
                        status = files?.dongle ?: none,
                        done = files?.dongle != null,
                        button = if (files?.dongle != null) replace else choose,
                        id = "$layer.dongle",
                        enabled = enabled,
                        onClick = { onImport(ArcadeLibrary.Part.DONGLE) },
                    )
                    if (title.id == SOUL_CALIBUR_II) {
                        StepCard(
                            number = 3,
                            title = str("arcade.import.cardPart"),
                            description = str("arcade.import.card.desc"),
                            status = files?.card ?: none,
                            done = files?.card != null,
                            button = if (files?.card != null) replace else choose,
                            id = "$layer.card",
                            enabled = enabled,
                            onClick = { onImport(ArcadeLibrary.Part.CARD) },
                        )
                    }
                }
                Spacer(Modifier.height(14.dp))
                val ready = files?.image != null && files.dongle != null
                val needs = when {
                    files == null -> ""
                    ready -> str("arcade.import.state.ready")
                    files.image != null -> str("arcade.import.state.needDongle")
                    files.dongle != null -> str(if (drive) "arcade.import.state.needDrive" else "arcade.import.state.needDisc")
                    else -> ""
                }
                if (needs.isNotEmpty()) {
                    Text(
                        needs,
                        style = MaterialTheme.typography.bodySmall,
                        color = if (ready) Success else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(10.dp))
                }
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    if (onUninstall != null) {
                        val error = MaterialTheme.colorScheme.error
                        OutlinedButton(
                            onClick = onUninstall,
                            enabled = enabled,
                            shape = RoundedCornerShape(12.dp),
                            border = BorderStroke(1.dp, error.copy(alpha = if (enabled) 0.7f else 0.25f)),
                            colors = ButtonDefaults.outlinedButtonColors(contentColor = error),
                            modifier = Modifier.controllerFocusable(
                                if (enabled) "$layer.uninstall" else null,
                                RoundedCornerShape(12.dp),
                                onConfirm = onUninstall,
                            ),
                        ) { Text(str("arcade.uninstall.button")) }
                    }
                    Spacer(Modifier.weight(1f))
                    OutlinedButton(
                        onClick = onClose,
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier.controllerFocusable("$layer.close", RoundedCornerShape(12.dp), onConfirm = onClose),
                    ) { Text(str("action.close")) }
                }
            }
        }
    }
}

/** An import's copy (for a DVD or hard drive image, a while) or an uninstall: [title] and how far it is.
 *  It ends by itself; [onCancel], where there is one, stops it early. [key] is its pad layer. */
@Composable
private fun Progress(key: String, title: String, progress: Float, onCancel: (() -> Unit)? = null) {
    com.armsx2.ui.common.PadModal(key = key, onDismiss = null) {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 420.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(title, style = MaterialTheme.typography.titleMedium)
                LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
                Text(
                    "${(progress * 100).toInt()}%",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (onCancel != null) {
                    // Pressed once: the copy stops at its next megabyte, so the button just waits for that.
                    var cancelling by remember { mutableStateOf(false) }
                    val cancel = {
                        cancelling = true
                        onCancel()
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        OutlinedButton(
                            onClick = cancel,
                            enabled = !cancelling,
                            shape = RoundedCornerShape(12.dp),
                            modifier = Modifier.controllerFocusable(
                                if (cancelling) null else "$key.cancel",
                                RoundedCornerShape(12.dp),
                                onConfirm = cancel,
                            ),
                        ) { Text(str("action.cancel")) }
                    }
                }
            }
        }
    }
}
