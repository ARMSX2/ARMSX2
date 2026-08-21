package com.armsx2.ui.emulation

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.EaseIn
import androidx.compose.animation.core.EaseOut
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.layout
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.achievements.AchievementItem
import com.armsx2.ui.common.GameCoverArt
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.touch.TouchControls
import com.armsx2.ui.theme.Danger
import com.armsx2.ui.common.StatusChip
import com.armsx2.ui.theme.Success
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

@Composable
fun EmulationMenuScreen(viewModel: EmulationMenuViewModel = viewModel()) {
    val state = viewModel.state.value
    val scope = rememberCoroutineScope()
    var shown by remember { mutableStateOf(false) }
    var dismissing by remember { mutableStateOf(false) }
    var friendsOpen by remember { mutableStateOf(false) }
    val closeMenu: () -> Unit = remember(viewModel, scope) {
        {
            if (!dismissing) {
                dismissing = true
                shown = false
                // ★ Dispatchers.Main, NOT the composition's own dispatcher. rememberCoroutineScope
                // inherits the composition context, which on Android is AndroidUiDispatcher — it
                // dispatches continuations on CHOREOGRAPHER FRAME CALLBACKS. We have just set
                // shown = false, so once the exit animation settles Compose has nothing left to
                // invalidate, no frame is scheduled, and the continuation after this delay is
                // never dispatched: the VM is simply never told to resume. The game sits paused
                // with the OSD reading "FPS: N/A" until something incidentally causes a frame —
                // which is exactly why tapping the on-screen controls "speeds up" the recovery
                // (touch input schedules a frame) and why waiting also eventually works.
                // Dispatchers.Main is a plain main-looper Handler dispatcher with no frame
                // dependency, so the resume fires on time whether or not anything is drawing.
                scope.launch(Dispatchers.Main) {
                    delay(220)
                    viewModel.dismissHandler = null
                    viewModel.resumeImmediately()
                }
            }
        }
    }

    DisposableEffect(viewModel, closeMenu) {
        viewModel.dismissHandler = closeMenu
        EmulationMenuInputController.bind(viewModel)
        onDispose {
            viewModel.dismissHandler = null
            EmulationMenuInputController.unbind(viewModel)
        }
    }
    LaunchedEffect(Unit) { shown = true }

    // Hand pad input to the Friends panel while it is open, and give it back on close.
    //
    // The nav registry is shared between the menu and the panel, so ownership has to be explicit:
    // the selection is cleared on both edges, because a selection left pointing at a control on
    // the other side of the transition highlights something the user cannot see.
    DisposableEffect(friendsOpen) {
        if (friendsOpen) {
            EmulationMenuInputController.overlayDismiss = { friendsOpen = false }
            com.armsx2.ui.settings.SettingsControllerNav.clearSelection()
        }
        onDispose {
            EmulationMenuInputController.overlayDismiss = null
            com.armsx2.ui.settings.SettingsControllerNav.clearSelection()
        }
    }
    // Highlight the panel's first control once it has actually composed. Selecting in the same
    // frame the panel opens would find an empty registry — controllerFocusable only registers
    // items that exist, and the panel's do not until AnimatedVisibility has run.
    LaunchedEffect(friendsOpen) {
        if (friendsOpen) {
            delay(260)
            if (friendsOpen) com.armsx2.ui.settings.SettingsControllerNav.selectFirstInLayer()
        }
    }
    // Back closes the friends overlay first when it is up. Without this, opening Friends and
    // pressing Back would dismiss the entire pause menu and resume the game, which is not what
    // anyone means by "go back" from a panel sitting on top of another panel.
    BackHandler(onBack = { if (friendsOpen) friendsOpen = false else closeMenu() })

    state.pendingHardcore?.let { enabling ->
        androidx.compose.runtime.DisposableEffect(Unit) {
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_OPEN)
            onDispose { com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.POPUP_CLOSE) }
        }
        // Was an AlertDialog, which is its own focused Android window and so consumed the pad
        // before the Activity dispatcher that owns every D-pad route in this app could see it.
        // The prompt sat on top of the pause menu, which IS pad-navigable, so it read as the
        // controller having gone dead the moment the confirmation appeared.
        com.armsx2.ui.common.ConfirmOverlay(
            title = str(if (enabling) "ra.hardcore.enable.title" else "ra.hardcore.disable.title"),
            message = str(if (enabling) "ra.hardcore.enable.body" else "ra.hardcore.disable.body"),
            confirmLabel = str(if (enabling) "ra.hardcore.enable.confirm" else "ra.hardcore.disable.confirm"),
            idPrefix = "hardcore",
            onConfirm = viewModel::confirmToggleHardcore,
            onDismiss = viewModel::cancelToggleHardcore,
        )
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val compact = maxWidth < 700.dp
        // The one place the layout is chosen is the one place that tells the pad which way it
        // runs — compact puts the tabs in a Row above the content, wide puts them in a rail to
        // its right, and the D-pad axis follows from here rather than from a constant that can
        // fall out of step with the UI (which is exactly what it had done).
        androidx.compose.runtime.SideEffect {
            EmulationMenuInputController.tabsHorizontal.value = compact
        }
        AnimatedVisibility(
            visible = shown,
            enter = fadeIn(tween(190, easing = EaseOut)),
            exit = fadeOut(tween(190, easing = EaseIn)),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.62f))
                    .clickable(onClick = closeMenu),
            )
        }
        AnimatedVisibility(
            visible = shown,
            enter = slideInHorizontally(tween(320, easing = EaseOut)) { it },
            exit = slideOutHorizontally(tween(220, easing = EaseIn)) { it },
            modifier = Modifier.align(Alignment.CenterEnd),
        ) {
            if (compact) {
                Surface(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(0.96f)
                        .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical)),
                    shape = RoundedCornerShape(topStart = 28.dp, bottomStart = 28.dp),
                    color = MaterialTheme.colorScheme.surface,
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                    shadowElevation = 22.dp,
                ) {
                    MenuPage(
                        state = state,
                        viewModel = viewModel,
                        compact = true,
                        modifier = Modifier.fillMaxSize(),
                        onOpenFriends = { friendsOpen = true },
                    )
                }
            } else {
                Row(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(0.64f)
                        .widthIn(max = 900.dp)
                        .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical))
                        .padding(top = 14.dp, end = 12.dp, bottom = 14.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Surface(
                        modifier = Modifier.weight(1f).fillMaxHeight(),
                        shape = RoundedCornerShape(28.dp),
                        color = MaterialTheme.colorScheme.surface,
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                        shadowElevation = 22.dp,
                    ) {
                        MenuPage(
                            state = state,
                            viewModel = viewModel,
                            compact = false,
                            modifier = Modifier.fillMaxSize(),
                            onOpenFriends = { friendsOpen = true },
                        )
                    }
                    Surface(
                        modifier = Modifier.width(76.dp).fillMaxHeight(),
                        shape = RoundedCornerShape(28.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.94f),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                        shadowElevation = 18.dp,
                    ) {
                        MenuRail(state.tab, viewModel::selectTab)
                    }
                }
            }
        }

        // Friends, as its own panel over the menu.
        //
        // Composed here rather than as an AlertDialog on purpose: a Dialog gets its own focused
        // window, and a focused window swallows gamepad keys before our input plumbing ever sees
        // them — the pause menu would stop responding to the pad the moment this opened.
        AnimatedVisibility(
            visible = friendsOpen,
            enter = fadeIn(tween(160, easing = EaseOut)),
            exit = fadeOut(tween(140, easing = EaseIn)),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.72f))
                    .clickable { friendsOpen = false },
            )
        }
        AnimatedVisibility(
            visible = friendsOpen,
            enter = fadeIn(tween(190, easing = EaseOut)),
            exit = fadeOut(tween(150, easing = EaseIn)),
            modifier = Modifier.align(Alignment.Center),
        ) {
            Surface(
                modifier = Modifier
                    .fillMaxWidth(if (compact) 0.94f else 0.6f)
                    .widthIn(max = 620.dp)
                    .fillMaxHeight(0.9f)
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical)),
                shape = RoundedCornerShape(24.dp),
                color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                shadowElevation = 24.dp,
            ) {
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 16.dp, end = 10.dp, top = 14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            str("friends.title"),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                        )
                        // Between the title and Close: whose Discord this is.
                        com.armsx2.ui.friends.SelfChip(
                            Modifier.weight(1f).padding(horizontal = 12.dp),
                        )
                        TextButton(
                            onClick = { friendsOpen = false },
                            modifier = Modifier.controllerFocusable(
                                "menu.friends.close",
                                onConfirm = { friendsOpen = false },
                            ),
                        ) { Text(str("action.close")) }
                    }
                    com.armsx2.ui.friends.FriendsPanel(Modifier.padding(horizontal = 12.dp))
                }
            }
        }
    }
}

@Composable
private fun MenuPage(
    state: EmulationMenuUiState,
    viewModel: EmulationMenuViewModel,
    compact: Boolean,
    modifier: Modifier,
    onOpenFriends: () -> Unit,
) {
    val tabScrollStates = remember {
        EmulationMenuTab.entries.associateWith {
            ScrollState(initial = InGameOverlay.menuTabScroll[it.name] ?: 0)
        }
    }
    // Remember each tab's scroll offset when the menu closes so reopening a tab (especially
    // the long Fixes list) returns to where you were instead of snapping back to the top.
    androidx.compose.runtime.DisposableEffect(Unit) {
        onDispose {
            tabScrollStates.forEach { (tab, ss) -> InGameOverlay.menuTabScroll[tab.name] = ss.value }
        }
    }
    val scrollState = tabScrollStates.getValue(state.tab)
    // Provide the pane's scroll state to the settings widgets so the Fixes pane's
    // right-stick free-scroll (settingsScrollState / ControllerAutoScroll) drives the
    // pane the user is actually looking at. Per-control bring-into-view handles the
    // primary "keep selection on screen" via the nearest scrollable ancestor already.
    androidx.compose.runtime.CompositionLocalProvider(
        com.armsx2.ui.settings.LocalSettingsScrollState provides scrollState,
    ) {
        Column(
            modifier
                .verticalScroll(scrollState)
                .padding(bottom = 18.dp),
        ) {
            if (compact) CompactMenuTabs(state.tab, viewModel::selectTab)
            MenuHeader(compact, state.hardcore, state.richPresence, state.gameCRC, onOpenFriends)
            HorizontalDivider(
                modifier = Modifier.padding(horizontal = 8.dp),
                color = MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
            )
            Column(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                when (state.tab) {
                    EmulationMenuTab.Session -> SessionPane(state, viewModel)
                    EmulationMenuTab.Graphics -> GraphicsPane(state, viewModel)
                    // The full Fixes settings tab, live-applying via InGameOverlay's
                    // shared Settings state (same as the rest of the overlay); its
                    // controls are SettingsControllerNav items, so the pause menu's
                    // content-pane nav drives them for free.
                    EmulationMenuTab.Fixes -> com.armsx2.ui.settings.FixesTab(InGameOverlay.settingsState)
                    EmulationMenuTab.Performance -> PerformancePane(state, viewModel)
                    EmulationMenuTab.Controls -> ControlsPane(state, viewModel)
                    EmulationMenuTab.Options -> OptionsPane(state, viewModel)
                    EmulationMenuTab.Achievements -> AchievementsPane(state, viewModel)
                }
            }
        }
    }
}

@Composable
private fun CompactMenuTabs(selected: EmulationMenuTab, onSelect: (EmulationMenuTab) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        EmulationMenuTab.entries.forEach { tab ->
            MenuTab(tab, tab == selected, onSelect)
        }
    }
}

@Composable
private fun MenuRail(
    selected: EmulationMenuTab,
    onSelect: (EmulationMenuTab) -> Unit,
) {
    Column(
        Modifier
            .fillMaxHeight()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        // Centred, not top-aligned: the rail fills the full height, so with the tabs pinned
        // to the top the column left a block of dead space at the bottom once the duplicate
        // All Settings shortcut was removed from under them. Centring keeps the group
        // balanced regardless of how many tabs there are, and still scrolls if it ever
        // outgrows the rail.
        verticalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterVertically),
    ) {
        EmulationMenuTab.entries.forEach { tab ->
            MenuRailTab(tab, tab == selected, onSelect)
        }
    }
}

@Composable
private fun MenuRailTab(tab: EmulationMenuTab, active: Boolean, onSelect: (EmulationMenuTab) -> Unit) {
    val bring = remember { BringIntoViewRequester() }
    val label = str(tab.titleKey)
    LaunchedEffect(active) { if (active) runCatching { bring.bringIntoView() } }
    Surface(
        onClick = { onSelect(tab) },
        modifier = Modifier.size(56.dp).bringIntoViewRequester(bring).semantics { contentDescription = label },
        shape = RoundedCornerShape(18.dp),
        color = if (active) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface.copy(alpha = 0.5f),
        border = BorderStroke(
            if (active) 2.dp else 1.dp,
            if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
        ),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = tabGlyph(tab),
                fontSize = 23.sp,
                fontWeight = FontWeight.Bold,
                color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun MenuTab(tab: EmulationMenuTab, active: Boolean, onSelect: (EmulationMenuTab) -> Unit) {
    // Keep the active tab scrolled into view so controller nav reaches tabs that fall off
    // the rail on short screens — e.g. the 7th "Achievements" (RA) tab on a Retroid Pocket
    // in landscape. Mirrors the settings-hub / library camera-follow. Resolves against the
    // nearest scrollable ancestor, so it works for both the vertical rail and the compact
    // horizontal strip.
    val bring = remember { BringIntoViewRequester() }
    LaunchedEffect(active) { if (active) runCatching { bring.bringIntoView() } }
    Surface(
        onClick = { onSelect(tab) },
        modifier = Modifier
            .widthIn(min = 132.dp, max = 210.dp)
            .padding(vertical = 3.dp)
            .bringIntoViewRequester(bring),
        shape = RoundedCornerShape(14.dp),
        color = if (active) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.56f),
        border = BorderStroke(
            if (active) 1.5.dp else 1.dp,
            if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.72f)
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.32f),
        ),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 11.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Surface(
                modifier = Modifier.size(30.dp),
                shape = RoundedCornerShape(9.dp),
                color = if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.14f)
                else MaterialTheme.colorScheme.surface.copy(alpha = 0.72f),
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Text(
                        text = tabGlyph(tab),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            Text(
                text = str(tab.titleKey),
                color = if (active) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = if (active) FontWeight.Bold else FontWeight.Medium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

// Rail tab icons. No monochrome Unicode exists for gamepad/wrench/trophy/display, so those
// use color emoji (the bundled NotoColorEmoji renders them); Session keeps its clean text
// glyph. Performance uses the high-voltage emoji so it reads as a yellow lightning bolt.
// Options carries the settings gear; the full-settings shortcut below the rail divider uses
// a distinct "open" glyph so there aren't two gears.
private fun tabGlyph(tab: EmulationMenuTab): String = when (tab) {
    EmulationMenuTab.Session -> "☰"
    EmulationMenuTab.Graphics -> "🖥️"
    EmulationMenuTab.Fixes -> "🔧"
    EmulationMenuTab.Performance -> "⚡"
    EmulationMenuTab.Controls -> "🎮"
    EmulationMenuTab.Options -> "⚙"
    EmulationMenuTab.Achievements -> "🏆"
}

@Composable
private fun MenuHeader(
    compact: Boolean,
    hardcore: Boolean,
    richPresence: String,
    gameCRC: String,
    onOpenFriends: () -> Unit,
) {
    val game = MainActivityRuntime.currentGame.value
    Row(
        Modifier.fillMaxWidth().padding(horizontal = if (compact) 12.dp else 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (game != null) {
            GameCoverArt(game, Modifier.width(if (compact) 38.dp else 44.dp).height(if (compact) 52.dp else 60.dp))
            Spacer(Modifier.width(11.dp))
        }
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    game?.title ?: "PlayStation 2",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f, fill = false),
                )
                if (hardcore) {
                    Spacer(Modifier.width(8.dp))
                    HardcoreBadge()
                }
                // File-type chip after the HC badge (ISO / CHD / …), mirroring the library
                // list view so the pause/RA header shows the same at-a-glance file info.
                game?.let { g ->
                    Spacer(Modifier.width(6.dp))
                    com.armsx2.ui.common.StatusChip(g.extension.ifBlank { g.platform.key.uppercase() })
                }
            }
            // Serial and CRC together: a PNACH is named <SERIAL>_<CRC>.pnach, so the two values
            // needed to name one should not live on separate screens.
            //
            // The live VM CRC is preferred but cannot be relied on: for ISO boots the core hands
            // ELFLoadingOnCPUThread an empty path, so UpdateELFInfo takes its failure branch and
            // leaves s_current_crc at 0 — the emulog shows the loader computing the real CRC and
            // the VM then reporting 00000000. When that happens, identify the image instead, which
            // is the same path the Info tab and the library's long-press sheet already take.
            val resolvedCRC by androidx.compose.runtime.produceState(gameCRC, gameCRC, game?.uri) {
                value = gameCRC.ifBlank {
                    game?.uri?.let { com.armsx2.DiscIdentity.resolve(it, game.serial) }.orEmpty()
                }
            }
            val identity = buildList {
                game?.serial?.takeIf { it.isNotBlank() }?.let(::add)
                resolvedCRC.takeIf { it.isNotBlank() }?.let { add("CRC $it") }
            }.joinToString("  ·  ")
            if (identity.isNotBlank()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    identity,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // RetroAchievements rich presence — the live "what you're doing" line
            // (e.g. "Pooh & Piglet are in a Scaring Contest"). Restored from the old UI.
            if (richPresence.isNotBlank()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    richPresence,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        // Clock + battery, same cluster as the library toolbar. Worth having here specifically:
        // this menu is what you open mid-session on a handheld, so it's exactly when you want to
        // know the time and how much charge is left. Not controllerFocusable — it's a readout.
        Spacer(Modifier.width(8.dp))
        com.armsx2.ui.common.LibraryStatusCluster(
            Modifier.align(Alignment.CenterVertically),
        )

        // Friends, in the header where it is always visible, with the online count on it. A build
        // without the SDK has nothing to show, so it does not take up header space there.
        if (com.armsx2.DiscordPresence.available()) {
            Spacer(Modifier.width(8.dp))
            Surface(
                onClick = onOpenFriends,
                modifier = Modifier.controllerFocusable(
                    "menu.friends",
                    RoundedCornerShape(14.dp),
                    onConfirm = onOpenFriends,
                ),
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            ) {
                Box(Modifier.padding(horizontal = 11.dp, vertical = 9.dp)) {
                    com.armsx2.ui.friends.FriendsGlyphWithBadge(
                        color = MaterialTheme.colorScheme.onSurface,
                        glyphSize = 19.sp,
                    )
                }
            }
        }
    }
}

@Composable
private fun SessionPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    ActionGrid(
        actions = listOf(
            MenuAction(str("action.resume"), str("action.play"), "▶", Success, viewModel::resume),
            MenuAction(
                str("action.fastForward"),
                if (MainActivityRuntime.fastForwardToggleActive) str("action.fastForward.on") else str("action.fastForward.detail"),
                "⏩",
                if (MainActivityRuntime.fastForwardToggleActive) Success else null,
            ) { MainActivityRuntime.instance?.toggleFastForward(); viewModel.resume() },
            MenuAction(str("memcard.restart"), str("action.reset"), "↻", null, MainActivityRuntime::restart),
            MenuAction(str("action.swapDisc"), str("action.swapDisc.detail"), "⏏", null, MainActivityRuntime::promptSwapDisc),
            MenuAction(str("action.close"), MainActivityRuntime.currentGame.value?.title.orEmpty(), "■", Danger) {
                MainActivityRuntime.closeGame()
            },
        ),
    )
    // On-screen display — a single universal on/off (old-UI style); the per-stat
    // toggles live in All Settings. Plus a frame-limit switch so fast-forward is one
    // tap away.
    SectionCard(str("tab.overlay")) {
        // #357: the pause button replaced the settings cog, so it's front-and-centre here. This is
        // "tap to reveal", NOT show/hide: on = the glyph stays hidden until you tap its top-right
        // corner, which surfaces it. Either way that corner always opens this menu, so unlike the
        // old on/off toggle there's no setting here that can lock you out of it.
        MenuSwitchRow(str("pad.pauseTapToReveal.label"), TouchControls.pauseTapToReveal.value) {
            TouchControls.setPauseTapToReveal(it)
        }
        Spacer(Modifier.height(6.dp))
        // OSD mode selector — one control (Full / Minimal / Custom / Off) in place of the old
        // master + simple toggles, cycled here and by the "Cycle Perf Stats (OSD)" hotkey. Custom
        // = the detailed per-stat selection from All Settings > On-Screen.
        val osdModes = com.armsx2.ui.InGameOverlay.OsdMode.entries
        val osdModeIndex = osdModes.indexOf(com.armsx2.ui.InGameOverlay.osdMode.value).coerceAtLeast(0)
        MenuCycleRow(
            title = str("overlay.master.label"),
            valueLabel = com.armsx2.ui.InGameOverlay.osdModeLabel(osdModes[osdModeIndex]),
        ) { step ->
            val size = osdModes.size
            val next = ((osdModeIndex + step) % size + size) % size
            com.armsx2.ui.InGameOverlay.setOsdMode(osdModes[next])
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("perf.frameLimit.label"), state.settings.frameLimitEnable) { value ->
            viewModel.updateSettings { it.copy(frameLimitEnable = value) }
        }
        Spacer(Modifier.height(6.dp))
        // Fast-forward SPEED — how fast the FF hotkey/button runs: 2..10x, or Unlimited (the
        // default, uncapped) at the top. Global pref; re-applied live if FF is currently engaged.
        var ffSpeed by remember { mutableStateOf(MainActivityRuntime.fastForwardSpeed()) }
        val ffUnlimitedLabel = str("common.unlimited") // hoisted: str() is @Composable, can't run in the formatter lambda
        com.armsx2.ui.settings.IntSliderRow(
            label = str("perf.ffSpeed.label"),
            value = ffSpeed,
            min = 2,
            max = MainActivityRuntime.FF_SPEED_UNLIMITED,
            valueFormatter = { if (it >= MainActivityRuntime.FF_SPEED_UNLIMITED) ffUnlimitedLabel else "${it}×" },
            onChange = { v ->
                ffSpeed = v
                MainActivityRuntime.setFastForwardSpeed(v)
                if (MainActivityRuntime.fastForwardToggleActive)
                    runCatching { kr.co.iefriends.pcsx2.NativeApp.speedhackLimitermode(MainActivityRuntime.ffLimiterMode()) }
            },
        )
        Spacer(Modifier.height(6.dp))
        // OSD colour, cycled in place. Shares the palette with the All Settings picker rather
        // than carrying its own copy. Safe to add here: this card's rows are plain switches with
        // their own callbacks, and every control on this pane — grid rows included — now
        // registers its own id with the nav registry, so inserting a row cannot shift what any
        // other row does.
        val osdColorIndex = com.armsx2.ui.settings.OSD_COLORS
            .indexOf(state.settings.osdColor).coerceAtLeast(0)
        MenuCycleRow(
            title = str("overlay.osdColor.label"),
            valueLabel = str(com.armsx2.ui.settings.OSD_COLOR_LABEL_KEYS[osdColorIndex]),
        ) { step ->
            val size = com.armsx2.ui.settings.OSD_COLORS.size
            val next = ((osdColorIndex + step) % size + size) % size
            viewModel.updateSettings { it.copy(osdColor = com.armsx2.ui.settings.OSD_COLORS[next]) }
        }
    }
    SectionCard(str("savestate.title.loadManage")) {
        Text(
            "${str("memcard.slot1").substringBefore(' ')} ${state.saveSlot + 1}",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(8.dp))
        Row(
            Modifier
                .fillMaxWidth()
                .bleedHorizontal(13.dp)
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 13.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            repeat(10) { slot ->
                OptionChip(
                    label = "${slot + 1}",
                    selected = slot == state.saveSlot,
                    controllerId = "pause.saveslot.$slot",
                    onClick = { viewModel.setSaveSlot(slot) },
                )
            }
        }
        Spacer(Modifier.height(9.dp))
        // Save / Load open the rich slot picker (thumbnails + autosave + the
        // auto-save/-load toggles), matching the old UI. The slot chips above stay
        // the quick-slot selector used by the on-screen / hotkey quick-save.
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CompactAction(str("savestate.title.save"), "↥", Modifier.weight(1f)) {
                com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.SaveState)
            }
            CompactAction(str("touch.stateAction.load"), "↧", Modifier.weight(1f)) {
                com.armsx2.ui.WindowImpl.openInGameScreen(com.armsx2.ui.InGameScreen.LoadState)
            }
        }
    }
}

@Composable
private fun GraphicsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    val settings = state.settings
    HorizontalOptions(
        title = str("tab.renderer"),
        options = listOf(
            "auto" to str("backend.renderer.auto"),
            "vulkan" to "Vulkan",
            "opengl" to "OpenGL",
            "software" to str("backend.renderer.software"),
        ),
        selected = settings.renderer,
        onSelect = viewModel::setRenderer,
    )
    // GPU driver manager (download/import/select) — Vulkan only — plus Apply &
    // Restart, since renderer + driver changes only take effect on renderer init.
    // For OpenGL the "custom driver" is ANGLE (GLES-on-Vulkan), same picker shape.
    if (settings.renderer == "vulkan") {
        com.armsx2.ui.common.DriverManagerSection()
    } else if (settings.renderer == "opengl") {
        com.armsx2.ui.common.AngleDriverSection(settings.useAngleOpenGL) { on ->
            viewModel.updateSettings { it.copy(useAngleOpenGL = on) }
        }
    }
    // GS Multi-threading (GV7 front/back split). Restart-required like the renderer /
    // driver above, so it lives in the same group — hit Apply & Restart below to apply.
    // Off = single-threaded; On = GS on a dedicated back thread (Pipelined, enum 3).
    // The Inline/Lockstep dev rungs are not exposed. Description shown inline so users
    // who never open full settings still understand what it does.
    MenuSwitchRow(
        str("renderer.gsBackThread.label"),
        settings.gsBackThreadMode >= 3,
        description = str("renderer.gsBackThread.description"),
    ) { on ->
        viewModel.updateSettings { it.copy(gsBackThreadMode = if (on) 3 else 0) }
    }
    // Every phone GPU is a tiler, so this belongs in the in-game menu next to the other
    // renderer levers, not just in full settings — it is the kind of thing you toggle while
    // looking at the framerate.
    MenuSwitchRow(
        str("renderer.coalesceRenderPasses.label"),
        settings.coalesceRenderPasses,
        description = str("renderer.coalesceRenderPasses.description"),
    ) { on ->
        viewModel.updateSettings { it.copy(coalesceRenderPasses = on) }
    }
    CompactAction(str("backend.applyRestart"), "↻", Modifier.fillMaxWidth(), MainActivityRuntime::restart)
    HorizontalOptions(
        title = str("renderer.upscale.label"),
        // Share the full settings-tab list so the sub-native 0.25/0.5/0.75/Native
        // options aren't dropped in the in-game quick menu.
        options = com.armsx2.ui.settings.UPSCALE_OPTIONS.map { it.value to it.label },
        selected = settings.upscaleFloat,
        onSelect = viewModel::setUpscale,
    )
    // Custom internal resolution, same control as the settings tab — the quick menu only offered
    // the preset steps, so a value between them (or set per-game) could be neither seen nor
    // changed from in-game. Percentage of native: 107% is roughly true 480p height.
    com.armsx2.ui.settings.IntSliderRow(
        label = str("renderer.upscale.customScale"),
        value = (settings.upscaleFloat * 100f).roundToInt().coerceIn(25, 800),
        min = 25,
        max = 800,
        description = str("renderer.upscale.customScale.description"),
        valueFormatter = { "$it%" },
        onReset = { viewModel.setUpscale(1.0f) },
        onChange = { pct -> viewModel.setUpscale(pct / 100f) },
    )
    // FSR sits with the resolution controls rather than the effects, because that is what it
    // is: the two rows above choose how big the frame is RENDERED, and this chooses how it
    // gets to the screen. In full settings it lives under Display Effects next to CAS, which
    // is the wrong shelf for finding it while you are looking at the framerate.
    // "auto" is the DEFAULT and resolves to Vulkan on Android, so gating on the literal string
    // "vulkan" hid this row from almost everyone — which is exactly what happened. Only OpenGL
    // and software genuinely cannot run it.
    if (settings.renderer != "opengl" && settings.renderer != "software") {
        val fsr1On = settings.upscaler == com.armsx2.config.Settings.UPSCALER_FSR1
        MenuSwitchRow(
            str("renderer.fsr1.label"),
            fsr1On,
            description = str("renderer.fsr1.description"),
        ) { on ->
            viewModel.updateSettings {
                it.copy(upscaler = if (on) com.armsx2.config.Settings.UPSCALER_FSR1 else com.armsx2.config.Settings.UPSCALER_OFF)
            }
        }
        if (fsr1On) {
            com.armsx2.ui.settings.IntSliderRow(
                label = str("renderer.fsr1.sharpness.label"),
                value = settings.fsrSharpness.coerceIn(0, 100),
                min = 0,
                max = 100,
                valueFormatter = { "$it%" },
                onChange = { pct -> viewModel.updateSettings { it.copy(fsrSharpness = pct) } },
            )
        }
    }
    HorizontalOptions(
        title = str("renderer.displayMode.label"),
        options = listOf(
            0 to str("setup.aspect.stretch"),
            1 to str("setup.aspect.auto"),
            2 to "4:3",
            3 to "16:9",
            4 to "10:7",
            5 to "21:9",
            6 to "20:9",
            7 to "19.5:9",
            8 to "Custom",
        ),
        selected = settings.aspectRatio,
        onSelect = viewModel::setAspectRatio,
    )
    // Overlay artwork, switchable from in-game — trying bezels means seeing them ON the game, and
    // having to leave for All Settings each time made that unusable. Import still lives in the
    // settings tab (it opens a file picker); this is the picker for what is already imported.
    run {
        val overlayCtx = androidx.compose.ui.platform.LocalContext.current
        val entries = remember { com.armsx2.OverlayRepo.list(overlayCtx) }
        // Shown even with nothing imported. Hiding it when the list was empty is why this looked
        // absent from the in-game menu entirely — with no overlays there was no row to find, and
        // no hint that the feature existed or where to add one.
        HorizontalOptions(
            title = str("renderer.overlayArt.label"),
            options = listOf("" to str("renderer.overlayArt.none")) +
                entries.map { it.imagePath to it.name },
            selected = com.armsx2.OverlayRepo.activePath.value,
            onSelect = { com.armsx2.OverlayRepo.setActive(it) },
        )
        if (entries.isEmpty()) {
            Text(
                str("renderer.overlayArt.emptyHint"),
                color = Color(0xFF9AA0A6),
                fontSize = 12.sp,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
            )
        }
    }
    HorizontalOptions(
        title = str("renderer.blendingAccuracy.label"),
        options = listOf("Minimum", "Basic", "Medium", "High", str("fixes.opt.full"), str("fixes.opt.max")).mapIndexed { index, label -> index to label },
        selected = settings.accurateBlendingUnit,
        onSelect = viewModel::setBlending,
    )
    HorizontalOptions(
        title = str("renderer.textureFiltering.label"),
        options = listOf("Nearest", str("fixes.opt.forced"), "PS2", str("fixes.opt.sprite")).mapIndexed { index, label -> index to label },
        selected = settings.textureFiltering,
        onSelect = viewModel::setTextureFiltering,
    )
    HorizontalOptions(
        title = str("renderer.texturePreloading.label"),
        options = listOf(str("fixes.opt.off"), "Partial", str("fixes.opt.full")).mapIndexed { index, label -> index to label },
        selected = settings.texturePreloading,
        onSelect = viewModel::setTexturePreloading,
    )
    HorizontalOptions(
        title = str("renderer.hardwareDownloadMode.label"),
        // Index == GSHardwareDownloadMode, so the order is load-bearing. "Async" is 5 and must stay
        // last; it is experimental (non-blocking readback) and is not the default.
        options = listOf("Accurate", "Force Full", "No Readbacks", "Unsync", "Disabled", "Async")
            .mapIndexed { index, label -> index to label },
        selected = settings.hardwareDownloadMode,
        onSelect = viewModel::setHardwareDownloadMode,
    )
    HorizontalOptions(
        title = str("renderer.deinterlacing.label"),
        options = listOf("Auto", "Off", "Weave TFF", "Weave BFF", "Bob TFF", "Bob BFF", "Blend TFF", "Blend BFF", "Adapt TFF", "Adapt BFF")
            .mapIndexed { index, label -> index to label },
        selected = settings.deinterlaceMode,
        onSelect = { value -> viewModel.updateSettings { it.copy(deinterlaceMode = value) } },
    )
    HorizontalOptions(
        title = str("renderer.displayFilter.label"),
        options = listOf("Nearest", "Smooth", "Sharp").mapIndexed { index, label -> index to label },
        selected = settings.displayBilinear,
        onSelect = { value -> viewModel.updateSettings { it.copy(displayBilinear = value) } },
    )
    HorizontalOptions(
        title = str("renderer.tvShader.label"),
        options = listOf("Off", "Scanline", "Diagonal", "Tri", "Wave", "Lottes", "4xRGSS", "NxAGSS")
            .mapIndexed { index, label -> index to label },
        selected = settings.tvShader,
        onSelect = { value -> viewModel.updateSettings { it.copy(tvShader = value) } },
    )
    HorizontalOptions(
        title = str("fixes.dithering.label"),
        options = listOf(str("fixes.opt.off"), str("fixes.opt.scaled"), str("fixes.opt.unscaled"), str("fixes.opt.force32"))
            .mapIndexed { index, label -> index to label },
        selected = settings.dithering,
        onSelect = { value -> viewModel.updateSettings { it.copy(dithering = value) } },
    )
    MenuSwitchRow(str("renderer.hwMipmapping.label"), settings.hwMipmap) {
        viewModel.updateSettings { current -> current.copy(hwMipmap = it) }
    }
    MenuSwitchRow(str("fixes.integerScaling.label"), settings.integerScaling) {
        viewModel.updateSettings { current -> current.copy(integerScaling = it) }
    }
    MenuSwitchRow("VSync", settings.vsyncEnable) {
        viewModel.updateSettings { current -> current.copy(vsyncEnable = it) }
    }
    MenuSwitchRow(str("renderer.shadeboost.label"), settings.shadeBoost) {
        viewModel.updateSettings { current -> current.copy(shadeBoost = it) }
    }
    MenuSwitchRow(str("fixes.antiBlur.label"), settings.antiBlur) {
        viewModel.updateSettings { current -> current.copy(antiBlur = it) }
    }
    MenuSwitchRow(str("fixes.screenOffsets.label"), settings.screenOffsets) {
        viewModel.updateSettings { current -> current.copy(screenOffsets = it) }
    }
    MenuSwitchRow(str("fixes.showOverscan.label"), settings.showOverscan) {
        viewModel.updateSettings { current -> current.copy(showOverscan = it) }
    }
    MenuSwitchRow(str("fixes.syncToHostRefresh.label"), settings.syncToHostRefresh) {
        viewModel.updateSettings { current -> current.copy(syncToHostRefresh = it) }
    }
    MenuSwitchRow(str("renderer.loadTexturePacks.label"), settings.loadTextureReplacements) {
        viewModel.updateSettings { current -> current.copy(loadTextureReplacements = it) }
    }
    MenuSwitchRow(str("renderer.asyncTextureLoading.label"), settings.loadTextureReplacementsAsync) {
        viewModel.updateSettings { current -> current.copy(loadTextureReplacementsAsync = it) }
    }
    MenuSwitchRow(str("renderer.precacheTexturePacks.label"), settings.precacheTextureReplacements) {
        viewModel.updateSettings { current -> current.copy(precacheTextureReplacements = it) }
    }
    // RetroArch shaders, end-to-end in-game: toggle → pick a preset → download more.
    // Same composables the Settings renderer tab renders (single definition in ui/common);
    // only the save lambda differs. updateSettings routes through InGameOverlay.saveSettings,
    // which persists via ConfigStore.save(scope, serial) — honouring the overlay's
    // Global/Game scope — and live-applies with Settings.applyTo(). Identical to how every
    // other row in this pane (shadeboost, tvShader, dithering…) saves; both GS keys ride
    // writeGsToNative(), and the device rebuilds the chain on the next frame, so a preset
    // change is live with no restart.
    com.armsx2.ui.common.ShaderChainSection(
        enabled = settings.shaderChainEnabled,
        preset = settings.shaderChainPreset,
        params = settings.shaderChainParams,
        onEnabledChange = { on -> viewModel.updateSettings { it.copy(shaderChainEnabled = on) } },
        onPresetChange = { path -> viewModel.updateSettings { it.copy(shaderChainPreset = path) } },
        onParamsChange = { next -> viewModel.updateSettings { it.copy(shaderChainParams = next) } },
    )
    com.armsx2.ui.common.ShaderManagerSection()
}

@Composable
private fun PerformancePane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    val settings = state.settings
    SectionCard(str("perf.speedLimit.label")) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (settings.frameLimitEnable) "${settings.nominalSpeedPercent}%" else str("setup.toggle.off"),
                modifier = Modifier.weight(1f),
                color = MaterialTheme.colorScheme.primary,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
            )
            Switch(
                checked = settings.frameLimitEnable,
                onCheckedChange = { enabled ->
                    viewModel.updateSettings { it.copy(frameLimitEnable = enabled) }
                },
            )
        }
        Spacer(Modifier.height(8.dp))
        HorizontalOptionRow(
            options = listOf(50, 75, 90, 100, 110, 125, 150, 200).map { it to "$it%" },
            selected = settings.nominalSpeedPercent,
            keyPrefix = str("perf.speedLimit.label"),
            onSelect = viewModel::setSpeed,
        )
    }
    HorizontalOptions(
        title = str("perf.displayFpsCap.label"),
        options = listOf(0, 20, 30, 45, 60, 90, 120).map {
            it to if (it == 0) str("setup.toggle.off") else "$it FPS"
        },
        selected = settings.fpsLimit,
        onSelect = viewModel::setFpsLimit,
    )
    HorizontalOptions(
        title = str("perf.frameSkip.label"),
        options = (0..5).map { it to if (it == 0) str("setup.toggle.off") else "$it" },
        selected = settings.frameSkip,
        onSelect = viewModel::setFrameSkip,
    )
    FramerateSlider(
        title = str("perf.ntscFramerate.label"),
        value = settings.framerateNtsc,
        onValue = { value -> viewModel.updateSettings { it.copy(framerateNtsc = value) } },
    )
    FramerateSlider(
        title = str("perf.palFramerate.label"),
        value = settings.frameratePal,
        onValue = { value -> viewModel.updateSettings { it.copy(frameratePal = value) } },
    )
    HorizontalOptions(
        title = str("perf.eeCycleRate.label"),
        options = (-3..3).map { it to if (it > 0) "+$it" else "$it" },
        selected = settings.eeCycleRate,
        onSelect = viewModel::setEeCycleRate,
    )
    HorizontalOptions(
        title = str("perf.eeCycleSkip.label"),
        options = (0..3).map { it to "$it" },
        selected = settings.eeCycleSkip,
        onSelect = viewModel::setEeCycleSkip,
    )
    HorizontalOptions(
        title = str("perf.eeFpuClamping.label"),
        options = listOf(str("perf.clamp.none"), str("perf.clamp.normal"), str("perf.clamp.extra"), str("perf.clamp.full"), str("perf.clamp.exact"))
            .mapIndexed { index, label -> index to label },
        selected = settings.eeClampMode,
        onSelect = { value -> viewModel.updateSettings { it.copy(eeClampMode = value) } },
    )
    HorizontalOptions(
        title = str("perf.vuClamping.label"),
        options = listOf(str("perf.clamp.none"), str("perf.clamp.normal"), str("perf.clamp.extra"), str("perf.clamp.extraSign"))
            .mapIndexed { index, label -> index to label },
        selected = settings.vuClampMode,
        onSelect = { value -> viewModel.updateSettings { it.copy(vuClampMode = value) } },
    )
    HorizontalOptions(
        title = str("perf.eeFpuRoundMode.label"),
        options = listOf(str("perf.round.nearest"), str("perf.round.negative"), str("perf.round.positive"), str("perf.round.chop"))
            .mapIndexed { index, label -> index to label },
        selected = settings.eeFpuRoundMode,
        onSelect = { value -> viewModel.updateSettings { it.copy(eeFpuRoundMode = value) } },
    )
    MenuSwitchRow(str("perf.hack.mtvu"), settings.mtvu) {
        viewModel.updateSettings { current -> current.copy(mtvu = it) }
    }
    MenuSwitchRow(str("perf.hack.instantVu1"), settings.vu1Instant) {
        viewModel.updateSettings { current -> current.copy(vu1Instant = it) }
    }
    MenuSwitchRow(str("perf.hack.fastCdvd"), settings.fastCDVD) {
        viewModel.updateSettings { current -> current.copy(fastCDVD = it) }
    }
    MenuSwitchRow(str("perf.hack.skipDupeFrames"), settings.skipDuplicateFrames) {
        viewModel.updateSettings { current -> current.copy(skipDuplicateFrames = it) }
    }
    MenuSwitchRow(str("perf.hack.vuFlagHack"), settings.vuFlagHack) {
        viewModel.updateSettings { current -> current.copy(vuFlagHack = it) }
    }
    MenuSwitchRow(str("perf.hack.intcStat"), settings.intcStat) {
        viewModel.updateSettings { current -> current.copy(intcStat = it) }
    }
    MenuSwitchRow(str("perf.hack.waitLoop"), settings.waitLoop) {
        viewModel.updateSettings { current -> current.copy(waitLoop = it) }
    }
    // Frame generation, in its own card: it changes what is PRESENTED rather than what is
    // emulated, so it does not belong among the speedhacks above. Github flavour only — the
    // card is a no-op stub in the Play build, which contains no frame generation at all.
    com.armsx2.ui.common.LsfgEmulationCard(
        enabled = settings.lsfgEnabled,
        multiplier = settings.lsfgMultiplier,
        dllPath = settings.lsfgDllPath,
        performance = settings.lsfgPerformance,
        flowScale = settings.lsfgFlowScale,
    ) { on, mult, dll, perf, flow ->
        viewModel.updateSettings {
            it.copy(
                lsfgEnabled = on,
                lsfgMultiplier = mult,
                lsfgDllPath = dll,
                lsfgPerformance = perf,
                lsfgFlowScale = flow,
            )
        }
    }
    SectionCard(str("tab.recompiler")) {
        MenuSwitchRow("EE (R5900)", settings.recEE) { value -> viewModel.updateSettings { it.copy(recEE = value) } }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow("IOP (R3000)", settings.recIOP) { value -> viewModel.updateSettings { it.copy(recIOP = value) } }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow("VU0", settings.recVU0) { value -> viewModel.updateSettings { it.copy(recVU0 = value) } }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow("VU1", settings.recVU1) { value -> viewModel.updateSettings { it.copy(recVU1 = value) } }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow("Fastmem", settings.enableFastmem) { value -> viewModel.updateSettings { it.copy(enableFastmem = value) } }
    }
    SectionCard(str("tab.overlay")) {
        MenuSwitchRow(str("overlay.toggle.fps"), settings.osdShowFps) { value ->
            viewModel.updateSettings { it.copy(osdShowFps = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.emulationSpeed"), settings.osdShowSpeed) { value ->
            viewModel.updateSettings { it.copy(osdShowSpeed = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.cpuUsage"), settings.osdShowCpu) { value ->
            viewModel.updateSettings { it.copy(osdShowCpu = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.gpuUsage"), settings.osdShowGpu) { value ->
            viewModel.updateSettings { it.copy(osdShowGpu = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.internalResolution"), settings.osdShowResolution) { value ->
            viewModel.updateSettings { it.copy(osdShowResolution = value) }
        }
        Spacer(Modifier.height(6.dp))
        MenuSwitchRow(str("overlay.toggle.onScreenNotifications"), settings.osdShowMessages) { value ->
            viewModel.updateSettings { it.copy(osdShowMessages = value) }
        }
    }
}

@Composable
private fun ControlsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    MenuSwitchRow(str("pad.onScreenControls.label"), state.touchControlsVisible) {
        viewModel.toggleTouchControls()
    }
    MenuSwitchRow(
        title = str("pad.rumble.label"),
        checked = state.rumbleEnabled,
        onCheckedChange = viewModel::setRumble,
    )
    // Vibration Strength — the same global 0-200% haptic multiplier as All Settings ›
    // Controls, reachable here in-game. Local state drives the live update since it's a
    // plain pref (not part of EmulationMenuUiState).
    var haptic by remember { mutableStateOf(com.armsx2.input.ControllerMappings.hapticIntensity()) }
    com.armsx2.ui.settings.IntSliderRow(
        label = str("pad.hapticStrength.label"),
        value = haptic,
        min = 0,
        max = 200,
        description = str("pad.hapticStrength.description"),
        valueFormatter = { if (it == 0) "Off" else "${it}%" },
        onChange = { haptic = it; com.armsx2.input.ControllerMappings.setHapticIntensity(it) },
    )
    MenuSwitchRow(str("pad.multitap.label"), state.multitapEnabled, onCheckedChange = viewModel::setMultitap)
    MenuSwitchRow(str("network.emulateUsbKeyboard"), state.settings.usbKeyboard) {
        viewModel.updateSettings { current -> current.copy(usbKeyboard = it) }
    }

    // Gesture control, in-game. Worth having here rather than only in All Settings: the swipe
    // distance and the Tap/Hold choice are things you only discover the right value for while
    // actually playing, and walking out to the settings tree to nudge them loses the moment.
    // Local state, like the haptic slider above — these are plain prefs, not part of the ui state.
    var gestureOn by remember { mutableStateOf(TouchControls.gestureEnabled.value) }
    MenuSwitchRow(str("pad.gesture.enable.label"), gestureOn) {
        gestureOn = it
        TouchControls.setGestureEnabled(it)
    }
    if (gestureOn) {
        var swipeSens by remember { mutableStateOf((TouchControls.gestureSwipeSensitivity.floatValue * 100f).toInt()) }
        com.armsx2.ui.settings.IntSliderRow(
            label = str("pad.gesture.sensitivity.label"),
            value = swipeSens,
            min = 5,
            max = 60,
            description = str("pad.gesture.sensitivity.description"),
            valueFormatter = { "${it}%" },
            onChange = { swipeSens = it; TouchControls.setGestureSensitivity(it / 100f) },
        )
        var holdMode by remember { mutableStateOf(TouchControls.gestureDoubleTapHold.value) }
        HorizontalOptions(
            title = str("pad.gesture.doubleTapMode.label"),
            options = listOf(
                0 to str("pad.gesture.doubleTapMode.tap"),
                1 to str("pad.gesture.doubleTapMode.hold"),
            ),
            selected = if (holdMode) 1 else 0,
            onSelect = { holdMode = it == 1; TouchControls.setGestureDoubleTapHold(holdMode) },
        )
        // The four swipe/double-tap ASSIGNMENTS stay in All Settings — six button pickers would
        // swamp this pane, and you set them once rather than mid-session.
    }
    CompactAction(str("pad.controllerMapping"), "⌁", Modifier.fillMaxWidth(), viewModel::openControlsManager)
    Spacer(Modifier.height(6.dp))
    CompactAction(str("pad.editTouchLayout"), "✥", Modifier.fillMaxWidth(), viewModel::editTouchControls)
    Spacer(Modifier.height(6.dp))
    // Sits with the touch layout because it's the same job: what the on-screen pad LOOKS
    // like, right after where it's laid out. Full-screen like Controller mapping.
    CompactAction(str("tab.skins"), "◈", Modifier.fillMaxWidth(), viewModel::openSkins)
    // Motion / gyroscope controls in-game (mode, sensitivity, smoothing, invert). Global scope
    // to match the rumble/multitap toggles above; the per-game scope lives in All Settings › Controls.
    com.armsx2.ui.settings.GyroSection()
    // Macros — edit each M1-M4 button set here in-game too (physical-trigger binding stays
    // in All Settings › Controls, which hosts the key-capture listener).
    com.armsx2.ui.settings.MacrosSection()
}

@Composable
private fun OptionsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    val settings = state.settings
    // Item 3: gateway to the full per-game settings (all categories the compact menu omits:
    // OSD, Skins, Audio, Hotkeys, Network, Recompiler, ...).
    CompactAction(str("action.allSettings"), "⚙", Modifier.fillMaxWidth(), viewModel::openFullSettings)
    Spacer(Modifier.height(6.dp))
    // In-game access to the manager screens (the library drawer's Memory Cards /
    // Patches & Cheats / Controller mapping) — open over the paused game.
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        CompactAction(str("memcard.title"), "▤", Modifier.weight(1f), viewModel::openMemcard)
        CompactAction(str("patches.dialog.patchesAndCheats"), "✦", Modifier.weight(1f), viewModel::openPatches)
    }
    Spacer(Modifier.height(6.dp))
    // Texture packs belong here too: the pack folder has to match the RUNNING game's serial,
    // so the screen only tells you anything useful with a game loaded — and buried in
    // All Settings -> Renderer it was effectively unreachable mid-session.
    // Glyph must be one already proven to render in the shipped font — "▩" (U+25A9) and
    // "⏻" (U+23FB) come out as tofu boxes on device. "▣" is used by the BIOS/onboarding
    // screens, so it is known good.
    CompactAction(str("renderer.section.texturePacks"), "▣", Modifier.fillMaxWidth(), viewModel::openTextures)
    Spacer(Modifier.height(6.dp))
    MenuSwitchRow(str("patches.enablePatches.label"), settings.enablePatches) {
        viewModel.updateSettings { current -> current.copy(enablePatches = it) }
    }
    MenuSwitchRow(
        if (state.hardcore) str("patches.cheats.labelHardcore") else str("patches.cheats.label"),
        settings.enableCheats && !state.hardcore,
        enabled = !state.hardcore,
    ) {
        viewModel.updateSettings { current -> current.copy(enableCheats = it) }
    }
    MenuSwitchRow(str("patches.widescreen.label"), settings.enableWideScreenPatches) {
        viewModel.updateSettings { current -> current.copy(enableWideScreenPatches = it) }
    }
    MenuSwitchRow(str("patches.noInterlacing.label"), settings.enableNoInterlacingPatches) {
        viewModel.updateSettings { current -> current.copy(enableNoInterlacingPatches = it) }
    }
    MenuSwitchRow(str("perf.fix.skipBios"), settings.enableFastBoot) {
        viewModel.updateSettings { current -> current.copy(enableFastBoot = it) }
    }
    MenuSwitchRow(str("perf.fix.gamedbFixes"), settings.enableGameFixes) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = it) }
    }
    MenuSwitchRow(str("perf.fix.skipMpeg"), settings.gamefixSkipMpeg) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixSkipMpeg = it) }
    }
    MenuSwitchRow(str("perf.fix.fmvSoftware"), settings.gamefixSoftwareRendererFmv) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixSoftwareRendererFmv = it) }
    }
    MenuSwitchRow(str("perf.fix.eeTiming"), settings.gamefixEETiming) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixEETiming = it) }
    }
    MenuSwitchRow(str("perf.fix.instantDma"), settings.gamefixInstantDma) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixInstantDma = it) }
    }
    MenuSwitchRow(str("perf.fix.blitFps"), settings.gamefixBlitInternalFps) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixBlitInternalFps = it) }
    }
    MenuSwitchRow(str("perf.fix.vuAddSub"), settings.gamefixVuAddSub) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixVuAddSub = it) }
    }
    MenuSwitchRow(str("perf.fix.vuSync"), settings.gamefixVuSync) {
        viewModel.updateSettings { current -> current.copy(enableGameFixes = true, gamefixVuSync = it) }
    }
}

@Composable
private fun AchievementsPane(state: EmulationMenuUiState, viewModel: EmulationMenuViewModel) {
    // Gateway to the full RetroAchievements screen (unlock list + presentation options).
    CompactAction(str("ra.viewAchievements"), "★", Modifier.fillMaxWidth(), viewModel::openAchievements)
    Spacer(Modifier.height(4.dp))
    SectionCard("RetroAchievements") {
        // Signed-in account: avatar + name + both point totals (hardcore / softcore).
        if (state.raUserName.isNotBlank()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (state.raAvatarUrl.isNotBlank()) {
                    AsyncImage(
                        state.raAvatarUrl,
                        state.raUserName,
                        Modifier.size(46.dp).clip(CircleShape),
                        contentScale = ContentScale.Crop,
                    )
                    Spacer(Modifier.width(12.dp))
                }
                Column(Modifier.weight(1f)) {
                    Text(
                        state.raUserName,
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "${state.raScore} HC",
                            style = MaterialTheme.typography.bodySmall,
                            color = com.armsx2.ui.theme.Danger,
                        )
                        Text(
                            "  ·  ${state.raSoftcoreScore} SC",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
        }
        Text(
            state.achievementSummary,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(8.dp))
        MenuSwitchRow(
            str(if (state.hardcore) "ra.mode.hardcore" else "ra.mode.casual"),
            state.hardcore,
            onCheckedChange = { viewModel.requestToggleHardcore() },
        )
    }
    // Inline unlock list, right below the hardcore toggle — no need to open the full
    // screen (it's still available via the button above).
    state.achievements.forEach { item -> InGameAchievementRow(item) }
}

@Composable
private fun InGameAchievementRow(item: AchievementItem) {
    Surface(
        Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        color = if (item.unlocked) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(
            1.dp,
            if (item.unlocked) MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.3f),
        ),
    ) {
        Row(Modifier.padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (item.iconUrl.isNotBlank()) {
                AsyncImage(
                    item.iconUrl,
                    item.title,
                    Modifier.size(40.dp).clip(RoundedCornerShape(9.dp)),
                    contentScale = ContentScale.Crop,
                )
            } else {
                Box(
                    Modifier.size(40.dp).clip(RoundedCornerShape(9.dp))
                        .background(MaterialTheme.colorScheme.surface),
                    contentAlignment = Alignment.Center,
                ) { Text(if (item.unlocked) "★" else "☆") }
            }
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                Text(item.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(item.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
                if (item.progress.isNotBlank()) {
                    Text(item.progress, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                }
            }
            Spacer(Modifier.width(8.dp))
            // Flag missables in-game — the actionable warning while you're actually playing.
            // Progression/Win badges are left to the full achievements screen to avoid clutter here.
            if (item.type == 1) {
                StatusChip(str("ra.typeChip.missable"), Color(0xFFF5A623))
                Spacer(Modifier.width(8.dp))
            }
            Text(
                "${item.points}",
                style = MaterialTheme.typography.labelMedium,
                color = if (item.unlocked) Success else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

@Composable
private fun HardcoreBadge() {
    // Firebrick red to match the old UI's hardcore pill (theme Danger reads pink here).
    Surface(shape = RoundedCornerShape(6.dp), color = Color(0xFFB22222)) {
        Text(
            "HC",
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp),
            color = Color.White,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
        )
    }
}

private data class MenuAction(
    val title: String,
    val detail: String,
    val glyph: String,
    val accent: Color?,
    val action: () -> Unit,
)

@Composable
private fun ActionGrid(actions: List<MenuAction>) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        actions.forEachIndexed { index, item ->
            val id = "pause.action.$index"
            // The registry is the ONE source of truth for which row is selected. The tint used
            // to come from a separate index in the view model that advanced only when a row was
            // ACTIVATED, while the D-pad moved the registry — so the menu drew one selection and
            // moved another, and the row you were pointing at was never the one lit up.
            val active = com.armsx2.ui.settings.SettingsControllerNav.isSelected(id)
            Surface(
                onClick = item.action,
                modifier = Modifier
                    .fillMaxWidth()
                    .controllerFocusable(id, onConfirm = item.action),
                shape = RoundedCornerShape(16.dp),
                color = if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.14f)
                else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.48f),
                border = BorderStroke(
                    1.dp,
                    if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.72f)
                    else MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
                ),
            ) {
                Row(Modifier.padding(horizontal = 13.dp, vertical = 11.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        item.glyph,
                        color = item.accent ?: MaterialTheme.colorScheme.primary,
                        fontSize = 19.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.width(30.dp),
                    )
                    Column(Modifier.weight(1f)) {
                        Text(item.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                        if (item.detail.isNotBlank()) {
                            Text(
                                item.detail,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
internal fun SectionCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(17.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun <T> HorizontalOptions(
    title: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
) {
    SectionCard(title) {
        HorizontalOptionRow(options, selected, keyPrefix = title, onSelect = onSelect)
    }
}

// Free-choice framerate slider (20–120 Hz) instead of a couple of fixed chips.
// The default (59.94 / 50) is kept exactly, and the 60/50 stops snap back to
// those exact PS2 rates (canonicalFramerate) so the true default is always
// recoverable; every other stop is whole Hz for easy targets (72/90/120).
@Composable
private fun FramerateSlider(title: String, value: Float, onValue: (Float) -> Unit) {
    SectionCard(title) {
        Column(
            Modifier.fillMaxWidth().controllerFocusable(
                "pause.framerate.$title",
                onLeft = { onValue(canonicalFramerate((Math.round(value) - 1).coerceAtLeast(20))) },
                onRight = { onValue(canonicalFramerate((Math.round(value) + 1).coerceAtMost(120))) },
            ),
        ) {
            val label = if (value % 1f == 0f) "${value.toInt()} Hz" else "%.2f Hz".format(value)
            Text(label, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.primary)
            Slider(
                value = value.coerceIn(20f, 120f),
                onValueChange = { onValue(canonicalFramerate(Math.round(it))) },
                valueRange = 20f..120f,
            )
        }
    }
}

// The PS2's true NTSC/PAL rates are 59.94/50.00 Hz; the integer slider stops at
// 60/50 map back to those exact defaults so the canonical rate stays recoverable
// (dragging otherwise snaps to whole Hz and loses 59.94 forever).
private fun canonicalFramerate(hz: Int): Float = when (hz) {
    60 -> 59.94f
    50 -> 50.00f
    else -> hz.toFloat()
}

@Composable
private fun <T> HorizontalOptionRow(
    options: List<Pair<T, String>>,
    selected: T,
    keyPrefix: String,
    onSelect: (T) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .bleedHorizontal(13.dp)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 13.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        options.forEach { (value, label) ->
            OptionChip(label, selected == value, controllerId = "pause.$keyPrefix.$label") { onSelect(value) }
        }
    }
}

@Composable
private fun OptionChip(label: String, selected: Boolean, controllerId: String? = null, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.controllerFocusable(controllerId, RoundedCornerShape(12.dp), onConfirm = onClick),
        shape = RoundedCornerShape(12.dp),
        color = if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.17f)
        else MaterialTheme.colorScheme.surface,
        border = BorderStroke(
            1.dp,
            if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.38f),
        ),
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
            maxLines = 2,
        )
    }
}

@Composable
private fun MenuSwitchRow(
    title: String,
    checked: Boolean,
    enabled: Boolean = true,
    description: String? = null,
    onCheckedChange: (Boolean) -> Unit,
) {
    Surface(
        onClick = { if (enabled) onCheckedChange(!checked) },
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(
                "pause.switch.$title",
                onConfirm = { if (enabled) onCheckedChange(!checked) },
                onLeft = { if (enabled) onCheckedChange(false) },
                onRight = { if (enabled) onCheckedChange(true) },
            ),
        enabled = enabled,
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = if (enabled) 0.42f else 0.24f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Row(
            Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    title,
                    style = MaterialTheme.typography.titleSmall,
                    color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                )
                if (description != null) {
                    Text(
                        description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 3,
                    )
                }
            }
            Spacer(Modifier.width(10.dp))
            Switch(checked = checked, onCheckedChange = if (enabled) onCheckedChange else null)
        }
    }
}

/** Label + current value, cycled in place: tap/confirm and Right advance, Left steps back.
 *  The compact menu has no picker of its own and a segmented control doesn't fit its width,
 *  so multi-option settings cycle rather than expand. */
@Composable
private fun MenuCycleRow(
    title: String,
    valueLabel: String,
    onStep: (Int) -> Unit,
) {
    Surface(
        onClick = { onStep(1) },
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(
                "pause.cycle.$title",
                onConfirm = { onStep(1) },
                onLeft = { onStep(-1) },
                onRight = { onStep(1) },
            ),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.34f)),
    ) {
        Row(
            Modifier.padding(horizontal = 13.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title,
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 2,
            )
            Spacer(Modifier.width(10.dp))
            Text(
                valueLabel,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun CompactAction(title: String, glyph: String, modifier: Modifier, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = modifier.controllerFocusable("pause.compact.$title", onConfirm = onClick),
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
    ) {
        Row(
            Modifier.padding(horizontal = 12.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(glyph, color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
            Text(title, style = MaterialTheme.typography.labelLarge, maxLines = 2)
        }
    }
}

private fun Modifier.bleedHorizontal(edge: androidx.compose.ui.unit.Dp): Modifier = layout { measurable, constraints ->
    val edgePx = edge.roundToPx()
    val expandedMin = (constraints.minWidth + edgePx * 2).coerceAtMost(constraints.maxWidth + edgePx * 2)
    val expandedMax = constraints.maxWidth + edgePx * 2
    val placeable = measurable.measure(
        constraints.copy(
            minWidth = expandedMin,
            maxWidth = expandedMax,
        ),
    )
    layout(constraints.maxWidth, placeable.height) {
        placeable.placeRelative(-edgePx, 0)
    }
}
