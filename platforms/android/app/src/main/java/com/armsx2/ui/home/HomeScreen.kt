package com.armsx2.ui.home

import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.foundation.layout.heightIn
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import com.armsx2.CoverArtStyle
import com.armsx2.EnglishTitles
import com.armsx2.GridLabels
import com.armsx2.R
import com.armsx2.ui.theme.ToolbarPositionPreferences
import com.armsx2.ui.theme.LibraryChromePreferences
import androidx.compose.ui.layout.layout
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import coil.size.Precision
import com.armsx2.CustomCovers
import com.armsx2.GameInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.SearchField
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.common.StatusChip
import com.armsx2.ui.settings.controllerFocusable
import kotlin.math.abs

private val LocalCustomCoverMap = staticCompositionLocalOf<Map<String, java.io.File>> { emptyMap() }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    onOpenMenu: () -> Unit,
    onOpenGameSettings: (GameInfo) -> Unit,
    modifier: Modifier = Modifier,
    viewModel: HomeViewModel = viewModel(),
) {
    val state = viewModel.state.value
    val directories = MainActivityRuntime.romsDirs.value
    val nativeReady = MainActivityRuntime.nativeReady.value
    val context = LocalContext.current
    val coverVersion = CustomCovers.version.value
    val customCoverMap = remember(coverVersion) { CustomCovers.loadAll(context) }
    var overflowMenu by remember { mutableStateOf(false) }
    var showExitConfirm by remember { mutableStateOf(false) }
    var menuGame by remember { mutableStateOf<GameInfo?>(null) }
    var showClearRecentsConfirm by remember { mutableStateOf(false) }
    // #9 custom library background — inert until the user picks an image.
    LaunchedEffect(Unit) { LibraryBackground.ensureLoaded(); CoverArtStyle.load() }
    val backgroundPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { picked ->
        picked?.let { LibraryBackground.set(context, it) }
    }
    // Search: both the controller (A on the Search zone) AND a touch tap open the app's own D-pad +
    // touch keyboard (LibraryKeyboard). The search bar is no longer an editable TextField, so the
    // Android system IME never appears. (Registered once the library has loaded.)
    val showSearch = LibraryChromePreferences.showSearch.value
    val showRecents = LibraryChromePreferences.showRecents.value
    val searchPlaceholder = str("games.search.placeholder")
    LaunchedEffect(state.initialized, showSearch) {
        if (!showSearch && viewModel.state.value.query.isNotEmpty()) viewModel.setQuery("")
        HomeInputController.setSearchAction(state.initialized && showSearch) {
            LibraryKeyboard.open(viewModel.state.value.query, viewModel::setQuery, searchPlaceholder)
        }
    }
    LaunchedEffect(directories, nativeReady) { viewModel.load(directories, nativeReady) }
    DisposableEffect(viewModel, onOpenMenu) {
        HomeInputController.bind(viewModel, onOpenMenu, onOpenGameMenu = { menuGame = it })
        onDispose { HomeInputController.unbind(viewModel) }
    }

    CompositionLocalProvider(LocalCustomCoverMap provides customCoverMap) {
    ArmsBackdrop(
        // Full-bleed wallpaper: the library image + readability scrim, drawn edge-to-edge
        // (behind the gesture bar) so it never leaves an exposed strip at the bottom —
        // that strip was the "blue bar" in landscape.
        backgroundLayer = {
            val libraryBg = LibraryBackground.uri.value
            if (libraryBg == null) {
                // Default: the live PS3-XMB wave (XmbGlView — a GLES3 port of linkev's
                // grid-displacement mesh, matching iOS). When GL can't init — older Mali without
                // float-texture filtering, or any EGL failure — we fall back to LibraryWaveBackground,
                // a procedural PPSSPP-style animated background drawn on the hardware 2D Canvas (no
                // GLES3, runs anywhere) that reads the SAME colour prefs as the GL wave, so Mali users
                // finally get an animated, recolourable backdrop instead of the old fixed GIF. The
                // bundled still is the cheap floor shown during GL startup (and, once the wave is up,
                // sits hidden behind it). Custom backgrounds below override all of this.
                if (LibraryBackground.flurry.value) {
                    // Flurry, in the same shell as the XMB wave: if GL cannot come up we fall back
                    // to the 2D backdrop rather than leaving a hole, exactly as XmbGlView does.
                    var flurryGl by remember { mutableStateOf<Boolean?>(null) }
                    if (flurryGl == false) {
                        LibraryWaveBackground(Modifier.fillMaxSize())
                    } else {
                        AndroidView(
                            factory = {
                                FlurryGlView(it, LibraryBackground.flurryPreset.value).apply {
                                    onGlStatus = { ok -> flurryGl = ok }
                                }
                            },
                            modifier = Modifier.fillMaxSize(),
                            // Stops the render thread. Without it the EGL thread outlives the
                            // composition and keeps drawing to a dead surface.
                            onRelease = { it.stop() },
                        )
                    }
                } else if (LibraryBackground.animated2D.value) {
                    // User opted into the lightweight 2D animated wave everywhere (#Luminz) — the same
                    // backdrop GL-fail devices get; skip the GLES3 XmbGlView entirely.
                    LibraryWaveBackground(Modifier.fillMaxSize())
                } else {
                    var xmbGlState by remember { mutableStateOf<Boolean?>(null) } // null=starting, true=up, false=failed
                    if (xmbGlState == false) {
                        LibraryWaveBackground(Modifier.fillMaxSize())
                    } else {
                        Image(
                            painter = painterResource(R.drawable.library_bg_xmb),
                            contentDescription = null,
                            modifier = Modifier.fillMaxSize(),
                            contentScale = ContentScale.Crop,
                        )
                    }
                    AndroidView(
                        factory = { XmbGlView(it).apply { onGlStatus = { ok -> xmbGlState = ok } } },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            } else {
                // User-picked still image / GIF (Coil handles both).
                AsyncImage(
                    model = ImageRequest.Builder(context).data(libraryBg).crossfade(true).build(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                )
            }
            // Scrim so covers and text stay readable over the backdrop. A user-picked image can
            // be any brightness, so it gets the full dark scrim. The XMB is our own controlled
            // backdrop (dark at the top where the content sits) and a heavy scrim just muddied
            // its blue into navy — so it gets only a whisper of dimming, letting the vivid blue
            // read through.
            val scrimTop = if (libraryBg == null) 0.06f else 0.55f
            val scrimBottom = if (libraryBg == null) 0.20f else 0.80f
            Box(
                Modifier.fillMaxSize().background(
                    Brush.verticalGradient(
                        listOf(
                            MaterialTheme.colorScheme.background.copy(alpha = scrimTop),
                            MaterialTheme.colorScheme.background.copy(alpha = scrimBottom),
                        ),
                    ),
                ),
            )
        },
    ) {
        BoxWithConstraints(modifier.fillMaxSize()) {
            val compact = maxWidth < 600.dp
            // Adaptive cells alone give a tablet MORE columns rather than BIGGER art, so scale the
            // cell width. Bigger cells mean bigger covers and fewer, better-spaced columns. Opt-in:
            // 1.0 everywhere until the user moves the Cover size slider.
            val coverScale = com.armsx2.ui.UiScale.coverScale.value
            val gridCellDp = (if (compact) 104f else 118f) * coverScale
            val columns = if (state.layout == LibraryLayout.Grid) {
                GridCells.Adaptive(gridCellDp.dp)
            } else {
                // List and Shelf are full-width rows.
                GridCells.Fixed(1)
            }
            // Column count feeds HomeInputController's Up/Down step. Shelf view lays
            // covers out in rows of `perShelf`, so it must report that count (not 1) —
            // otherwise Up/Down move one cover at a time (feeling like Left/Right) and
            // only the very first cover can step up into the Recents row.
            val estimatedColumns = when (state.layout) {
                // MUST track gridCellDp — this feeds HomeInputController's Up/Down step, so if the
                // estimate and the real column count diverge, controller navigation skips rows.
                LibraryLayout.Grid ->
                    (maxWidth.value / ((if (compact) 112f else 128f) * coverScale)).toInt().coerceAtLeast(1)
                LibraryLayout.Shelf ->
                    (maxWidth.value / (((if (compact) 84f else 100f) * coverScale) + 20f)).toInt().coerceIn(3, 8)
                LibraryLayout.List -> 1
            }
            LaunchedEffect(estimatedColumns) { HomeInputController.setColumnCount(estimatedColumns) }

            val gridState = rememberLazyGridState()
            val density = LocalDensity.current
            LaunchedEffect(gridState) {
                var lastFrame = withFrameNanos { it }
                while (true) {
                    val frame = withFrameNanos { it }
                    val dt = ((frame - lastFrame).coerceAtMost(50_000_000L)).toFloat() / 1_000_000_000f
                    lastFrame = frame
                    val velocity = HomeInputController.scrollVelocity.floatValue
                    if (abs(velocity) > 0.08f) {
                        val pxPerSecond = with(density) { 1500.dp.toPx() }
                        gridState.scrollBy(velocity * pxPerSecond * dt)
                    }
                }
            }
            // The Recently-Played games shown above the grid (empty while searching);
            // register them so the controller's Recents zone can launch them.
            val shownRecents = if (showRecents && state.recentGames.isNotEmpty() && state.query.isBlank()) {
                state.recentGames.take(10)
            } else {
                emptyList()
            }
            LaunchedEffect(shownRecents) {
                HomeInputController.setRecents(shownRecents.size) { i ->
                    shownRecents.getOrNull(i)?.let(viewModel::launch)
                }
            }
            // Camera-follow: while browsing with a controller (Grid zone), keep the
            // selected cover on screen. The grid has leading full-span items (toolbar,
            // search, recents section, All-Games header) before the game cells, so the
            // selected game's lazy-grid index = leading + its row (shelf) / flat index
            // (grid, list). Only scroll when it's actually off-screen so paging through a
            // visible screenful doesn't jitter. (Previously this only ran while searching,
            // so normal browsing never followed the selector.)
            val allGamesHeaderShown = shownRecents.isNotEmpty() &&
                state.initialized && state.visibleGames.isNotEmpty()
            LaunchedEffect(state.selectedIndex, state.visibleGames.size, state.layout, HomeInputController.zone.value) {
                if (HomeInputController.zone.value != HomeZone.Grid) return@LaunchedEffect
                // Don't follow (and thus don't scroll away from the top) until the user
                // has actually navigated — otherwise a cold open snaps the grid to the
                // initially-selected cover and hides the toolbar/search/recents header.
                if (!HomeInputController.userNavigated) return@LaunchedEffect
                val sel = state.selectedIndex
                if (sel < 0 || state.visibleGames.isEmpty()) return@LaunchedEffect
                val leading = 1 + // toolbar
                    (if (state.initialized && showSearch) 1 else 0) + // search field
                    (if (shownRecents.isNotEmpty()) 1 else 0) + // recents section
                    (if (allGamesHeaderShown) 1 else 0) // All Games header
                val cols = estimatedColumns.coerceAtLeast(1)
                val target = leading + if (state.layout == LibraryLayout.Shelf) sel / cols else sel
                val vis = gridState.layoutInfo.visibleItemsInfo
                val first = vis.firstOrNull()?.index
                val last = vis.lastOrNull()?.index
                if (first == null || last == null || target < first || target > last) {
                    gridState.animateScrollToItem(target.coerceAtLeast(0))
                }
            }
            // Entering a chrome zone (Toolbar / Search / Recents) scrolls the grid to the
            // very top so those rows are on-screen when focused.
            LaunchedEffect(HomeInputController.zone.value) {
                if (HomeInputController.zone.value != HomeZone.Grid) gridState.animateScrollToItem(0)
            }

            // Keep covers/text out of the display's unsafe edge (cutout / rounded
            // corner) — add the cutout inset to the side padding. The full-bleed
            // shelf/recent rows only negate the base 8dp, so they stop at the safe
            // edge rather than bleeding under the cutout.
            val cutout = WindowInsets.displayCutout.asPaddingValues()
            val ld = LocalLayoutDirection.current

            // Toolbar position is an App setting. At the top it's the first grid item;
            // at the bottom it's a pinned bar (identical rounded-pill shape). The grid
            // reserves the matching inset so nothing hides behind whichever edge it's on.
            val toolbarBottom = ToolbarPositionPreferences.atBottom.value
            LaunchedEffect(toolbarBottom) { HomeInputController.setToolbarAtBottom(toolbarBottom) }
            val statusBarTop = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()
            val navBarBottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
            val libraryToolbar: @Composable (Boolean) -> Unit = { bottomEdge ->
                // Register the toolbar button actions (left→right order) so the
                // controller's toolbar zone can fire them, and read the highlight
                // state so the focused button lights up.
                HomeInputController.setToolbarActions(
                    listOf(
                        onOpenMenu,
                        { viewModel.refresh() },
                        { viewModel.toggleLayout() },
                        { overflowMenu = true },
                    ),
                )
                val tb = HomeInputController.zone.value == HomeZone.Toolbar
                val tbi = HomeInputController.toolbarIndex.intValue
                ArmsTopBar(
                    title = str("games.section.library"),
                    subtitle = if (state.scanning) {
                        str("games.scanningRoms")
                    } else {
                        "${str("games.library.totalGames")}: ${state.allGames.size}"
                    },
                    leading = {
                        RoundAction(
                            "☰",
                            str("games.overflow.openNavigation"),
                            onOpenMenu,
                            selected = tb && tbi == 0,
                            framed = true,
                            buttonSize = 44.dp,
                            buttonShape = RoundedCornerShape(14.dp),
                            subtleFrame = true,
                        )
                    },
                    actions = {
                        // Clock + battery, ahead of the buttons so it reads as status rather than
                        // as another control. Deliberately NOT controllerFocusable — it isn't
                        // interactive, and registering it would put a dead stop in the pad's path
                        // through the toolbar.
                        com.armsx2.ui.common.LibraryStatusCluster(
                            // align(): the title block makes the bar taller than this two-line
                            // cluster, so without it the pair sits high relative to the buttons.
                            Modifier.align(Alignment.CenterVertically).padding(end = 6.dp),
                            // Portrait: single compact row so the narrow bar doesn't cram it.
                            compact = LocalConfiguration.current.orientation ==
                                android.content.res.Configuration.ORIENTATION_PORTRAIT,
                        )
                        RoundAction(
                            "↻",
                            str("games.card.refresh"),
                            viewModel::refresh,
                            selected = tb && tbi == 1,
                            framed = false,
                        )
                        RoundAction(
                            when (state.layout) {
                                LibraryLayout.Grid -> "☷"
                                LibraryLayout.List -> "▦"
                                LibraryLayout.Shelf -> "▤"
                            },
                            str("games.toolbar.rows"),
                            viewModel::toggleLayout,
                            selected = tb && tbi == 2,
                            framed = false,
                        )
                        var overflowAnchor by remember {
                            mutableStateOf(androidx.compose.ui.geometry.Offset.Zero)
                        }
                        Box(
                            Modifier.onGloballyPositioned {
                                // Bottom-left of the button: the panel hangs below it, the way
                                // the dropdown it replaces did.
                                val p = it.positionInRoot()
                                overflowAnchor = androidx.compose.ui.geometry.Offset(
                                    p.x, p.y + it.size.height,
                                )
                            },
                        ) {
                            RoundAction(
                                "⋮",
                                str("games.toolbar.more"),
                                { overflowMenu = true },
                                selected = overflowMenu || tb && tbi == 3,
                                framed = false,
                            )
                            LibraryOverflowMenu(
                                expanded = overflowMenu,
                                selectedSort = state.sort,
                                use3dCovers = CoverArtStyle.use3d.value,
                                showGridNames = GridLabels.show.value,
                                customNames = com.armsx2.CustomNames.enabled.value,
                                englishTitles = EnglishTitles.enabled.value,
                                showHidden = com.armsx2.HiddenGames.showHidden.value,
                                hasCustomBackground = LibraryBackground.uri.value != null,
                                onDismiss = { overflowMenu = false },
                                onOpenNavigation = onOpenMenu,
                                onSort = viewModel::setSort,
                                onToggleCoverStyle = { CoverArtStyle.set(!CoverArtStyle.use3d.value) },
                                onToggleGridNames = { GridLabels.set(!GridLabels.show.value) },
                                onToggleCustomNames = { com.armsx2.CustomNames.set(!com.armsx2.CustomNames.enabled.value) },
                                onToggleEnglishTitles = { EnglishTitles.set(!EnglishTitles.enabled.value) },
                                onToggleShowHidden = { viewModel.setShowHidden(!com.armsx2.HiddenGames.showHidden.value) },
                                onChooseBackground = { backgroundPicker.launch(arrayOf("image/*")) },
                                onClearBackground = LibraryBackground::clear,
                                onExitApp = { showExitConfirm = true },
                                anchor = overflowAnchor,
                            )
                            if (showExitConfirm) {
                                com.armsx2.ui.common.ConfirmOverlay(
                                    title = str("games.exit.title"),
                                    message = str("games.exit.message"),
                                    confirmLabel = str("games.toolbar.exit"),
                                    idPrefix = "library-exit",
                                    onConfirm = {
                                        showExitConfirm = false
                                        MainActivityRuntime.exitApp()
                                    },
                                    onDismiss = { showExitConfirm = false },
                                )
                            }
                        }
                    },
                    horizontalPadding = 0.dp,
                    bottomEdge = bottomEdge,
                )
            }
            LazyVerticalGrid(
                columns = columns,
                state = gridState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(
                    start = 8.dp + cutout.calculateStartPadding(ld),
                    end = 8.dp + cutout.calculateEndPadding(ld),
                    top = if (toolbarBottom) statusBarTop + 8.dp else 0.dp,
                    bottom = if (toolbarBottom) navBarBottom + 72.dp else 16.dp,
                ),
                horizontalArrangement = Arrangement.spacedBy(9.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (!toolbarBottom) {
                    item(span = { GridItemSpan(maxLineSpan) }) { libraryToolbar(false) }
                }
                if (state.initialized && showSearch) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        SearchField(
                            value = state.query,
                            onClick = { LibraryKeyboard.open(viewModel.state.value.query, viewModel::setQuery, searchPlaceholder) },
                            placeholder = searchPlaceholder,
                            modifier = Modifier.fillMaxWidth(),
                            selected = HomeInputController.zone.value == HomeZone.Search,
                        )
                    }
                }

                if (shownRecents.isNotEmpty()) {
                    val recentsSelected = HomeInputController.zone.value == HomeZone.Recents
                    val recentSel = if (recentsSelected) HomeInputController.recentIndex.intValue else -1
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Column {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                // In shelf view nudge the header right so it lines up with
                                // the first cover's left edge (the shelf's 12dp inset).
                                SectionTitle(
                                    str("games.section.recentlyPlayed"),
                                    modifier = Modifier.padding(
                                        start = if (state.layout == LibraryLayout.Shelf) 4.dp else 0.dp,
                                    ),
                                )
                                Spacer(Modifier.width(10.dp))
                                val clearAll = { showClearRecentsConfirm = true }
                                Surface(
                                    onClick = clearAll,
                                    modifier = Modifier.controllerFocusable(
                                        controllerId = "home.recents.clearAll",
                                        shape = RoundedCornerShape(12.dp),
                                        onConfirm = clearAll,
                                    ),
                                    shape = RoundedCornerShape(12.dp),
                                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
                                ) {
                                    Text(
                                        str("games.recent.clearAll"),
                                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 5.dp),
                                        style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                            Spacer(Modifier.height(9.dp))
                            if (state.layout == LibraryLayout.Shelf) {
                                // Same frosted-glass plank as All Games (bagas: one
                                // shelf design everywhere).
                                GameShelf(
                                    games = shownRecents,
                                    shelfRes = R.drawable.shelf_frosted,
                                    coverWidth = ((if (compact) 84f else 100f) * coverScale).dp,
                                    scroll = true,
                                    selectedIndex = recentSel,
                                    onLaunch = { viewModel.launch(it) },
                                    onDetails = { menuGame = it },
                                    modifier = Modifier.layout { measurable, constraints ->
                                        val edge = 8.dp.roundToPx()
                                        val placeable = measurable.measure(
                                            constraints.copy(
                                                minWidth = constraints.maxWidth + edge * 2,
                                                maxWidth = constraints.maxWidth + edge * 2,
                                            ),
                                        )
                                        layout(constraints.maxWidth, placeable.height) { placeable.placeRelative(-edge, 0) }
                                    },
                                )
                            } else {
                                val recentsRowState = rememberLazyListState()
                                LaunchedEffect(recentSel) {
                                    if (recentSel >= 0) recentsRowState.animateScrollToItem(recentSel)
                                }
                                LazyRow(
                                    state = recentsRowState,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .layout { measurable, constraints ->
                                            val edge = 8.dp.roundToPx()
                                            val placeable = measurable.measure(
                                                constraints.copy(
                                                    minWidth = constraints.minWidth + edge * 2,
                                                    maxWidth = constraints.maxWidth + edge * 2,
                                                ),
                                            )
                                            layout(constraints.maxWidth, placeable.height) {
                                                placeable.placeRelative(-edge, 0)
                                            }
                                        },
                                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                                    contentPadding = PaddingValues(horizontal = 8.dp),
                                ) {
                                    itemsIndexed(shownRecents, key = { _, g -> g.uri.toString() }) { index, game ->
                                        RecentGameCard(
                                            game = game,
                                            selected = index == recentSel,
                                            onClick = { viewModel.launch(game) },
                                            onDetails = { menuGame = game },
                                        )
                                    }
                                }
                            }
                        }
                    }
                }

                // Separate the Recently Played shelf from the full library with an
                // "All Games" header so the two rows don't run together.
                if (shownRecents.isNotEmpty() && state.initialized && state.visibleGames.isNotEmpty()) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        HorizontalDivider(
                            modifier = Modifier.padding(vertical = 8.dp),
                            color = MaterialTheme.colorScheme.outline.copy(alpha = 0.34f),
                        )
                    }
                }

                if (!state.initialized) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Box(Modifier.fillMaxWidth().height(240.dp), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator()
                        }
                    }
                } else if (state.visibleGames.isEmpty()) {
                    emptyLibrary(state.query.isBlank())
                } else if (state.layout == LibraryLayout.Shelf) {
                    // Fill each plank: chunk by how many covers fit the shelf width.
                    val shelfCoverW = ((if (compact) 84f else 100f) * coverScale).dp
                    val perShelf = (maxWidth.value / (shelfCoverW.value + 20f)).toInt().coerceIn(3, 8)
                    val shelfRows = state.visibleGames.chunked(perShelf)
                    items(
                        shelfRows.size,
                        span = { GridItemSpan(maxLineSpan) },
                        key = { "shelf_$it" },
                    ) { rowIndex ->
                        GameShelf(
                            games = shelfRows[rowIndex],
                            shelfRes = R.drawable.shelf_frosted,
                            coverWidth = shelfCoverW,
                            scroll = false,
                            // Every row lays out on the same perShelf-slot grid, so a
                            // short last row keeps its covers packed left in sequence.
                            slotsPerRow = perShelf,
                            selectedIndex = state.selectedIndex,
                            startIndex = rowIndex * perShelf,
                            onLaunch = { viewModel.launch(it) },
                            onDetails = { menuGame = it },
                            // Bleed past the grid's 8dp side padding so the glass shelf
                            // reaches both screen edges instead of floating inset.
                            modifier = Modifier.layout { measurable, constraints ->
                                val edge = 8.dp.roundToPx()
                                val placeable = measurable.measure(
                                    constraints.copy(
                                        minWidth = constraints.maxWidth + edge * 2,
                                        maxWidth = constraints.maxWidth + edge * 2,
                                    ),
                                )
                                layout(constraints.maxWidth, placeable.height) { placeable.placeRelative(-edge, 0) }
                            },
                        )
                    }
                } else {
                    itemsIndexed(
                        state.visibleGames,
                        key = { _, game -> game.uri.toString() },
                        contentType = { _, _ -> state.layout.name },
                    ) { index, game ->
                        if (state.layout == LibraryLayout.Grid) {
                            GameGridCard(
                                game = game,
                                selected = index == state.selectedIndex,
                                onSelect = { viewModel.setSelection(index) },
                                onLaunch = { viewModel.launch(game) },
                                onDetails = { menuGame = game },
                            )
                        } else {
                            GameListCard(
                                game = game,
                                selected = index == state.selectedIndex,
                                onClick = { viewModel.setSelection(index); viewModel.launch(game) },
                                onDetails = { menuGame = game },
                            )
                        }
                    }
                }
            }

            // Pinned bottom toolbar (App setting) — same rounded-pill component as the
            // top placement. Match the top's width by applying the SAME side inset the
            // grid's contentPadding gives the top bar (8dp + display cutout); otherwise
            // the bottom bar spans edge-to-edge and reads wider than the top.
            if (toolbarBottom) {
                Box(
                    Modifier
                        .align(Alignment.BottomCenter)
                        .fillMaxWidth()
                        .padding(
                            start = 8.dp + cutout.calculateStartPadding(ld),
                            end = 8.dp + cutout.calculateEndPadding(ld),
                        ),
                ) {
                    libraryToolbar(true)
                }
            }
        }
        // The keyboard is hosted once in WindowImpl, above every surface — hosting it here as
        // well would draw it twice, and a per-screen host is what made it invisible from
        // Settings (a nav destination that unmounts this screen).
    }
    }

    // Sits outside the grid so it draws over the whole library. Not an AlertDialog — a Compose
    // dialog is its own window and would swallow the D-pad, and this has to be pad-navigable.
    if (showClearRecentsConfirm) {
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("games.recent.clearAll.title"),
            message = str("games.recent.clearAll.message"),
            confirmLabel = str("games.recent.clearAll"),
            destructive = true,
            idPrefix = "clear-recents",
            onConfirm = {
                viewModel.clearRecent()
                showClearRecentsConfirm = false
            },
            onDismiss = { showClearRecentsConfirm = false },
        )
    }

    menuGame?.let { game ->
        // Tri-state on purpose: null while identifying, blank when the image cannot be identified.
        // produceState alone cannot tell those apart — both are null — so an unidentifiable game
        // would sit on "…" forever, reading as still-loading when it is actually unknown.
        val menuCRC by androidx.compose.runtime.produceState<String?>(initialValue = null, game.uri) {
            value = com.armsx2.DiscIdentity.resolve(game.uri, game.serial) ?: ""
        }
        // A bottom-aligned panel rather than a ModalBottomSheet: that is its own focused
        // Android window, so every row in here was unreachable by pad. Same look — it still
        // rises from the bottom edge, full width, rounded at the top. Swipe-to-dismiss is the
        // one thing lost; B and a tap on the scrim both close it.
        com.armsx2.ui.common.PadModal(
            key = "game-menu",
            onDismiss = { menuGame = null },
            alignment = Alignment.BottomCenter,
        ) {
          Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 2.dp,
          ) {
            Column(
                Modifier.fillMaxWidth().heightIn(max = 520.dp).verticalScroll(rememberScrollState()).padding(start = 8.dp, end = 8.dp, bottom = 20.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                // Keeps the sheet's grab-handle silhouette now that the real one is gone.
                Box(Modifier.fillMaxWidth().padding(top = 10.dp, bottom = 2.dp), contentAlignment = Alignment.Center) {
                    Box(
                        Modifier
                            .size(width = 32.dp, height = 4.dp)
                            .clip(RoundedCornerShape(2.dp))
                            .background(MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)),
                    )
                }
                Text(
                    game.displayTitle(EnglishTitles.enabled.value),
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
                )
                // Serial and CRC — the two halves of a <SERIAL>_<CRC>.pnach filename.
                val identity = buildList {
                    game.serial?.takeIf { it.isNotBlank() }?.let(::add)
                    add("CRC " + when (menuCRC) {
                        null -> "…"   // still identifying
                        "" -> "—"     // identified as unknown
                        else -> menuCRC
                    })
                }.joinToString("  ·  ")
                Text(
                    identity,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 8.dp),
                )
                GameMenuAction("▶", str("action.play"), "game-menu.play") {
                    menuGame = null
                    viewModel.launch(game)
                }
                // Save states for this game, straight from the library. Only offered when some
                // exist — a "Load state" row that opens onto nothing is worse than no row.
                val slots by androidx.compose.runtime.produceState(initialValue = emptyList<com.armsx2.SaveSlotLookup.Slot>(), game.serial) {
                    value = withContext(Dispatchers.IO) {
                        com.armsx2.SaveSlotLookup.slotsFor(context, game.serial)
                    }
                }
                if (slots.isNotEmpty())
                {
                    GameMenuAction("💾", str("games.loadState"), "game-menu.loadstate") {
                        menuGame = null
                        // ★ The real Save Manager, not a bespoke list.
                        //
                        // It already renders slots as a grid with preview thumbnails and has its
                        // own back button — which is both what was asked for and the answer to a
                        // modal with no touch exit. contextGame exists for exactly this: it is how
                        // the Save Manager already operates on a game that is not running.
                        com.armsx2.runtime.MainActivityRuntime.contextGame.value = game
                        com.armsx2.navigation.UiNavigator.navigate(com.armsx2.navigation.AppRoute.SaveManager)
                    }
                }
                GameMenuAction("⚙", str("action.settings"), "game-menu.settings") {
                    menuGame = null
                    onOpenGameSettings(game)
                }
                // Per-game BIOS: open the BIOS manager scoped to THIS game (no need to load it),
                // since the BIOS manager isn't reachable from the in-game menu.
                GameMenuAction("📀", str("bios.perGame.menu"), "game-menu.bios") {
                    menuGame = null
                    com.armsx2.navigation.UiNavigator.navigate(com.armsx2.navigation.AppRoute.BiosManager(game))
                }
                // Cover art from another region, for THIS game only. The library-wide switch
                // in the overflow menu is the wrong grain by itself: wanting the Japanese cover
                // for a couple of games does not mean wanting every Western cover swapped. Only
                // offered for a game we can actually look up — the index is keyed by serial.
                game.serial?.takeIf { it.isNotBlank() }?.let { gameSerial ->
                    val coverCtx = context
                    // Read the generation so this row (and the grid behind it) recomposes on a pin.
                    com.armsx2.CoverRegionIndex.perGameGeneration.intValue
                    val pinned = com.armsx2.CoverRegionIndex.regionFor(gameSerial)
                    GameMenuAction(
                        "A/あ",
                        str("games.overflow.coverRegion"),
                        "game-menu.coverregion",
                        trailing = str(
                            when (pinned) {
                                1 -> "games.overflow.coverRegion.usa"
                                2 -> "games.overflow.coverRegion.eur"
                                3 -> "games.overflow.coverRegion.jpn"
                                0 -> "games.overflow.coverRegion.disc"
                                else -> "games.coverRegion.library"
                            },
                        ),
                    ) {
                        // Cycles Library -> Disc -> USA -> EUR -> Jpn -> Library. "Library" is the
                        // absence of a pin, which is why it is null and not a fifth region.
                        val next = when (pinned) {
                            null -> 0
                            3 -> null
                            else -> pinned + 1
                        }
                        com.armsx2.CoverRegionIndex.setFor(gameSerial, next)
                        if (next != null && next != 0)
                            com.armsx2.CoverRegionIndex.ensureBuilt(coverCtx)
                        // Menu stays open: picking a region is a cycle, and closing after every
                        // press would mean re-opening the menu three times to reach Japan.
                    }
                }
                // Per-game memory cards without booting the game. The card picker already
                // does per-game assignment whenever it is handed a game — until now the only
                // caller that handed it one was the in-game menu, so from the library you got
                // the global slots and no way to reach the per-game ones.
                GameMenuAction("🗃️", str("memcard.perGame.menu"), "game-menu.memcard") {
                    menuGame = null
                    com.armsx2.navigation.UiNavigator.navigate(
                        com.armsx2.navigation.AppRoute.MemoryCardManager(game))
                }
                // Pin to the launcher (issue #242). The action was lost when this menu was
                // rebuilt, leaving HomeShortcuts with no call site at all (issue #335).
                // pin() returns false only when the launcher can't pin — surface that.
                val addToHomeFailed = str("games.addToHome.unsupported")
                GameMenuAction("📌", str("games.addToHome"), "game-menu.pin") {
                    menuGame = null
                    if (!com.armsx2.HomeShortcuts.pin(context, game))
                        Toast.makeText(context, addToHomeFailed, Toast.LENGTH_LONG).show()
                }
                // Only offered when the game is actually in Recently Played — this drops
                // just this one entry, unlike the library-wide "Show Recently Played" toggle.
                if (state.recentGames.any { it.uri == game.uri }) {
                    GameMenuAction("🕐", str("games.removeRecent"), "game-menu.recent") {
                        viewModel.removeFromRecent(game)
                        menuGame = null
                    }
                }
                val hidden = com.armsx2.HiddenGames.isHidden(game)
                GameMenuAction(if (hidden) "◍" else "🚫", str(if (hidden) "games.unhide" else "games.hide"), "game-menu.hide") {
                    viewModel.setHidden(game, !hidden)
                    menuGame = null
                }
            }
          }
        }
    }
}

@Composable
private fun GameMenuAction(
    glyph: String,
    label: String,
    id: String,
    trailing: String? = null,
    onClick: () -> Unit,
) {
    Surface(
        onClick = onClick,
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(controllerId = id, shape = RoundedCornerShape(18.dp), onConfirm = onClick),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.45f)),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text(glyph, color = MaterialTheme.colorScheme.primary, fontSize = 20.sp)
            Text(label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            trailing?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
        }
    }
}

@Composable
private fun LibraryOverflowMenu(
    expanded: Boolean,
    selectedSort: HomeSort,
    use3dCovers: Boolean,
    showGridNames: Boolean,
    customNames: Boolean,
    englishTitles: Boolean,
    showHidden: Boolean,
    hasCustomBackground: Boolean,
    onDismiss: () -> Unit,
    onOpenNavigation: () -> Unit,
    onSort: (HomeSort) -> Unit,
    onToggleCoverStyle: () -> Unit,
    onToggleGridNames: () -> Unit,
    onToggleCustomNames: () -> Unit,
    onToggleEnglishTitles: () -> Unit,
    onToggleShowHidden: () -> Unit,
    onChooseBackground: () -> Unit,
    onClearBackground: () -> Unit,
    onExitApp: () -> Unit,
    anchor: androidx.compose.ui.geometry.Offset,
) {
    fun closeThen(action: () -> Unit) {
        onDismiss()
        action()
    }

    if (!expanded) return
    // Anchored under its own ⋮ button, so it still reads as that button's menu rather than a
    // prompt about the whole screen. Was a DropdownMenu, which is its own focused Android
    // window and therefore had no controller route to any of these rows.
    com.armsx2.ui.common.PadModal(
        key = "library-overflow",
        onDismiss = onDismiss,
        anchor = anchor,
        // A menu belonging to one button should not black out the library behind it.
        scrimAlpha = 0.32f,
    ) {
      Surface(
        modifier = Modifier.widthIn(min = 320.dp, max = 380.dp),
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 8.dp,
        shadowElevation = 14.dp,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
      ) {
        // Plain Column, never Lazy — the registry only sees composed rows.
        Column(Modifier.heightIn(max = 460.dp).verticalScroll(rememberScrollState())) {
        Text(
            text = str("games.section.library"),
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Bold,
        )
        LibraryOverflowItem(
            glyph = "A–Z",
            label = str("games.overflow.sortTitle"),
            selected = selectedSort == HomeSort.Title,
        ) {
            closeThen { onSort(HomeSort.Title) }
        }
        LibraryOverflowItem(
            glyph = "⇅",
            label = str("games.overflow.sortRecent"),
            selected = selectedSort == HomeSort.RecentlyPlayed,
        ) {
            closeThen { onSort(HomeSort.RecentlyPlayed) }
        }
        OverflowSeparator()
        LibraryOverflowItem(
            glyph = if (use3dCovers) "3D" else "2D",
            label = str("games.overflow.coverStyle"),
            trailing = if (use3dCovers) "3D" else "2D",
        ) {
            closeThen(onToggleCoverStyle)
        }
        LibraryOverflowItem(
            glyph = "Aa",
            label = str("games.overflow.customNames"),
            trailing = if (customNames) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleCustomNames)
        }
        // Cover region: show another region's box art. Cycles Disc -> USA -> Europe -> Japan.
        // The lookup needs the GameDB index, so building it is kicked off the first time anyone
        // leaves "Disc" — a user who never touches this never pays for the parse.
        run {
            val regionCtx = androidx.compose.ui.platform.LocalContext.current
            val r = com.armsx2.CoverRegionIndex.region.intValue
            LibraryOverflowItem(
                glyph = "A/あ",
                label = str("games.overflow.coverRegion"),
                trailing = str(
                    when (r) {
                        1 -> "games.overflow.coverRegion.usa"
                        2 -> "games.overflow.coverRegion.eur"
                        3 -> "games.overflow.coverRegion.jpn"
                        else -> "games.overflow.coverRegion.disc"
                    },
                ),
            ) {
                closeThen {
                    val next = (r + 1) % 4
                    com.armsx2.CoverRegionIndex.set(next)
                    if (next != 0) com.armsx2.CoverRegionIndex.ensureBuilt(regionCtx)
                }
            }
        }
        LibraryOverflowItem(
            glyph = "A/あ",
            label = str("games.overflow.englishTitles"),
            trailing = if (englishTitles) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleEnglishTitles)
        }
        LibraryOverflowItem(
            glyph = "◍",
            label = str("games.overflow.showHidden"),
            trailing = if (showHidden) str("common.on") else str("common.off"),
        ) {
            closeThen(onToggleShowHidden)
        }
        OverflowSeparator()
        LibraryOverflowItem("▧", str("games.background.choose")) {
            closeThen(onChooseBackground)
        }
        if (hasCustomBackground) {
            LibraryOverflowItem("×", str("games.background.clear")) {
                closeThen(onClearBackground)
            }
        }
        OverflowSeparator()
        // Exit, back where it used to live. It moved to the drawer, which put it below every other
        // destination -- so quitting, one of the most frequent things anyone does here, meant
        // opening the drawer and scrolling to the bottom every time (issue #460, and shinobumaehara
        // is right that frequency should decide placement). It stays in the drawer too; this is the
        // short path, not a replacement.
        //
        // The confirmation is the point of the row and travels with it: quitting mid-session
        // without one loses whatever is not saved.
        // onExitApp was still a parameter and its confirmation dialog was still wired up — only
        // the row that reached them had been removed. So this restores the item, not the feature.
        LibraryOverflowItem(
            glyph = "⏻",
            label = str("games.toolbar.exit"),
            iconRes = com.armsx2.R.drawable.ic_power,
            iconTint = Color(0xFFE60012),
        ) {
            closeThen(onExitApp)
        }
        }
      }
    }
}

@Composable
private fun OverflowSeparator() {
    androidx.compose.material3.HorizontalDivider(
        modifier = Modifier.padding(horizontal = 18.dp, vertical = 6.dp),
        color = MaterialTheme.colorScheme.outline.copy(alpha = 0.30f),
    )
}

@Composable
private fun LibraryOverflowItem(
    glyph: String,
    label: String,
    selected: Boolean = false,
    trailing: String? = null,
    // A real drawable instead of a text glyph. Exit needs this: the power symbol (U+23FB) is not
    // in the bundled font and rendered as a tofu box. Null keeps the glyph path for every other row.
    iconRes: Int? = null,
    iconTint: Color? = null,
    onClick: () -> Unit,
) {
    DropdownMenuItem(
        text = {
            Text(
                text = label,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                maxLines = 2,
                lineHeight = 20.sp,
                overflow = TextOverflow.Ellipsis,
            )
        },
        onClick = onClick,
        leadingIcon = {
            Surface(
                modifier = Modifier.size(34.dp),
                shape = RoundedCornerShape(11.dp),
                color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            ) {
                Box(contentAlignment = Alignment.Center) {
                    if (iconRes != null) {
                        androidx.compose.material3.Icon(
                            painter = androidx.compose.ui.res.painterResource(iconRes),
                            contentDescription = null,
                            tint = iconTint ?: MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.size(19.dp),
                        )
                    } else {
                        Text(
                            text = glyph,
                            fontSize = if (glyph.length > 2) 11.sp else 17.sp,
                            fontWeight = FontWeight.Bold,
                            color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurface,
                        )
                    }
                }
            }
        },
        trailingIcon = {
            when {
                selected -> Text("✓", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
                trailing != null -> Text(trailing, color = MaterialTheme.colorScheme.onSurfaceVariant, fontWeight = FontWeight.Bold)
            }
        },
        modifier = Modifier
            .padding(horizontal = 6.dp)
            // Labels are unique within this menu, so they make stable ids without threading an
            // extra argument through all twelve call sites.
            .controllerFocusable(
                controllerId = "library-overflow:$label",
                shape = RoundedCornerShape(12.dp),
                onConfirm = onClick,
            ),
    )
}

private fun LazyGridScope.emptyLibrary(noFolders: Boolean) {
    item(span = { GridItemSpan(maxLineSpan) }) {
        EmptyState(
            title = if (noFolders) str("games.empty.noFolders.title") else str("games.search.placeholder"),
            message = if (noFolders) str("games.empty.noFolders.body") else str("games.search.hint"),
            actionLabel = if (noFolders) str("games.toolbar.setup") else null,
            onAction = if (noFolders) MainActivityRuntime::reopenSetup else null,
            modifier = Modifier.fillMaxWidth().height(260.dp),
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameGridCard(
    game: GameInfo,
    selected: Boolean,
    onSelect: () -> Unit,
    onLaunch: () -> Unit,
    onDetails: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = { onSelect(); onLaunch() }, onLongClick = onDetails),
    ) {
        GameCover(
            game,
            Modifier
                .fillMaxWidth()
                .aspectRatio(coverAspectRatio())
                .coverFrame(selected, 2.dp, MaterialTheme.colorScheme.primary),
        )
        if (GridLabels.show.value) {
            Spacer(Modifier.height(4.dp))
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                style = MaterialTheme.typography.labelSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(horizontal = 2.dp),
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameListCard(game: GameInfo, selected: Boolean, onClick: () -> Unit, onDetails: () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth().combinedClickable(onClick = onClick, onLongClick = onDetails),
        shape = RoundedCornerShape(15.dp),
        color = MaterialTheme.colorScheme.surface.copy(alpha = LibraryChromePreferences.libraryOpacity.value / 100f),
        // Same contrast problem as coverFrame: primary-on-themed-background disappears when the
        // two share a hue. A thicker stroke blended toward inverseSurface keeps it readable on
        // any theme without abandoning the accent colour entirely.
        border = BorderStroke(
            if (selected) 3.dp else 1.dp,
            if (selected)
                lerp(MaterialTheme.colorScheme.primary, MaterialTheme.colorScheme.inverseSurface, 0.35f)
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.42f),
        ),
    ) {
        Row(Modifier.padding(7.dp), verticalAlignment = Alignment.CenterVertically) {
            GameCover(game, Modifier.width(54.dp).aspectRatio(coverAspectRatio()))
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(game.displayTitle(EnglishTitles.enabled.value), style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Spacer(Modifier.height(5.dp))
                GameMetadata(game)
            }
            Text("▶", color = MaterialTheme.colorScheme.primary, modifier = Modifier.padding(10.dp))
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RecentGameCard(game: GameInfo, selected: Boolean = false, onClick: () -> Unit, onDetails: () -> Unit) {
    Column(
        modifier = Modifier
            .width(102.dp)
            .combinedClickable(onClick = onClick, onLongClick = onDetails),
    ) {
        GameCover(
            game,
            Modifier.fillMaxWidth().aspectRatio(coverAspectRatio())
                .coverFrame(selected, 2.5.dp, Color(0xFF3DA5FF)),
        )
        Spacer(Modifier.height(5.dp))
        Text(game.displayTitle(EnglishTitles.enabled.value), style = MaterialTheme.typography.labelMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun GameMetadata(game: GameInfo) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
        StatusChip(game.extension.ifBlank { game.platform.key.uppercase() })
        game.regionFlag?.let { Text(it, fontSize = 13.sp) }
        if (game.compatibility > 0) {
            Text("★".repeat(game.compatibility), color = Color(0xFFFFC857), fontSize = 9.sp, maxLines = 1)
        }
        // Playtime and last-known achievement progress, shown only when there is something to show
        // so an untouched library looks exactly as before. Playtime is app-side per serial; the
        // achievement counts are whatever the game last reported while running (the core cannot be
        // asked about a game it has not loaded).
        val rev = com.armsx2.PlayTime.revision.value
        val played = remember(game.serial, rev) {
            com.armsx2.PlayTime.formatPlayed(com.armsx2.PlayTime.playedSeconds(game.serial))
        }
        if (played.isNotEmpty()) {
            Text("⏳ $played", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 11.sp, maxLines = 1)
        }
        val ach = remember(game.serial, rev) { com.armsx2.PlayTime.achievements(game.serial) }
        ach?.let { p ->
            // Hardcore and softcore are different accomplishments on RetroAchievements — hardcore
            // forbids save states, cheats and slow motion — so they get different treatment rather
            // than being merged into one number. Lead with hardcore when any exists (it is the
            // stricter figure), and only mention softcore separately when it is actually ahead.
            val leadHardcore = p.hardcore > 0
            val shown = if (leadHardcore) p.hardcore else p.softcore
            val mastered = if (leadHardcore) p.masteredHardcore else p.masteredSoftcore
            Text(
                "🏆 $shown/${p.total}",
                color = when {
                    mastered && leadHardcore -> Color(0xFFFFC857)  // gold: mastered in hardcore
                    mastered -> Color(0xFFB9C2CC)                  // silver: completed in softcore
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
                fontSize = 11.sp,
                maxLines = 1,
            )
            if (leadHardcore) {
                Text(
                    "HC",
                    color = Color(0xFFFFC857),
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
            }
            // Softcore ahead of hardcore: show what is still only earned in casual mode, so the gap
            // is visible instead of the row silently under-reporting the collection.
            if (leadHardcore && p.softcore > p.hardcore) {
                Text(
                    "+${p.softcore - p.hardcore} SC",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 9.sp,
                    maxLines = 1,
                )
            }
        }
    }
}

/**
 * Aspect ratio of a cover slot, matched to the artwork actually served.
 *
 * xlenore's 3D case renders are 567x878 (0.646) while the flat 2D scans are 512x736 (0.696). The
 * slot was hardcoded to 0.72 for both, so with ContentScale.Fit the 3D art — being narrower than
 * its slot — sat with transparent margins down each side: the "poorly filled square" that 2D does
 * not show, because 0.696 all but fills 0.72. Reported by Isshin.
 */
@Composable
private fun coverAspectRatio(): Float = if (CoverArtStyle.use3d.value) 0.646f else 0.72f

/**
 * Frame around a cover slot.
 *
 * The idle 1dp frame only makes sense for the flat 2D scans, which fill their slot as a rectangle
 * so the frame hugs the artwork. A 3D case render is transparent around the angled case, so the
 * same frame draws a rounded rectangle through empty space beside it — the stray outline and edge
 * lines reported in grid view. The shelf never showed them because it frames only the selected
 * cover; do the same here, so 3D gets a selection frame and nothing else.
 */
@Composable
/**
 * The selection frame.
 *
 * ★ Two rings, not one. A single ring in [selectedColor] is the theme's primary, and the library
 * background is themed from the same palette — so on a blue theme the highlight was blue on blue
 * and effectively invisible when navigating by controller, which is the only way it is navigated.
 *
 * The outer ring is drawn in a colour derived from the SURFACE rather than the accent, so it
 * contrasts with the background whatever hue the user picked; the accent ring sits inside it and
 * keeps the theme's identity. Whichever of the two the background happens to match, the other one
 * still reads.
 */
private fun Modifier.coverFrame(selected: Boolean, selectedWidth: Dp, selectedColor: Color): Modifier {
    val idle = !CoverArtStyle.use3d.value
    val contrast = MaterialTheme.colorScheme.inverseSurface
    return when {
        selected -> this
            .border(selectedWidth + 2.dp, contrast, RoundedCornerShape(13.dp))
            .border(selectedWidth, selectedColor, RoundedCornerShape(12.dp))
        idle -> this.border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f), RoundedCornerShape(12.dp))
        else -> this
    }
}

@Composable
private fun GameCover(
    game: GameInfo,
    modifier: Modifier = Modifier,
    cornerRadius: Dp = 12.dp,
    // Fit (not Crop) everywhere so the whole box art shows — Crop trims the top of
    // covers whose source art is a touch taller than the 0.7 slot.
    contentScale: ContentScale = ContentScale.Fit,
    placeholderText: Boolean = true,
) {
    val context = LocalContext.current
    // Read the 3D-cover flag explicitly (not just via game.coverUrl, which is
    // skipped when a custom cover wins) so EVERY card — the Recently Played shelf
    // included — is subscribed and re-resolves when the toolbar toggle flips.
    val use3d = CoverArtStyle.use3d.value
    // Same reasoning for Cover Region: game.coverUrl resolves it internally, so nothing here was
    // subscribed to a region change and cards kept their old art until something else happened to
    // recompose them. Both the library-wide setting and the per-game pins are read.
    val coverRegion = com.armsx2.CoverRegionIndex.region.intValue
    val coverPins = com.armsx2.CoverRegionIndex.perGameGeneration.intValue
    val customCoverMap = LocalCustomCoverMap.current
    val custom = remember(game.uri, customCoverMap) { CustomCovers.matchIn(customCoverMap, game) }
    val model = custom ?: game.coverUrl
    val request = remember(model, use3d, coverRegion, coverPins) {
        ImageRequest.Builder(context)
            .data(model)
            .size(360, 500)
            .precision(Precision.INEXACT)
            .allowHardware(true)
            .crossfade(false)
            .build()
    }
    Box(modifier.clip(RoundedCornerShape(cornerRadius))) {
        if (model == null) {
            CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText)
        } else {
            // No fill behind the art: 3D box-art PNGs are transparent around the
            // angled case, and any backing shows as a dark/coloured "notch" at the
            // top. Keeping it transparent lets the case sit directly on the shelf.
            SubcomposeAsyncImage(
                model = request,
                contentDescription = game.displayTitle(EnglishTitles.enabled.value),
                modifier = Modifier.fillMaxSize(),
                contentScale = contentScale,
                loading = { CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText) },
                error = {
                    // A regional cover that isn't in the art repo would otherwise blank a cover the
                    // user already had — reported as "some games lose their covers when switching
                    // regions". Retry with this disc's own serial before giving up.
                    val discUrl = custom?.let { null } ?: game.discCoverUrl
                    if (discUrl != null && discUrl != model) {
                        SubcomposeAsyncImage(
                            model = discUrl,
                            contentDescription = game.displayTitle(EnglishTitles.enabled.value),
                            modifier = Modifier.fillMaxSize(),
                            contentScale = contentScale,
                            loading = { CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText) },
                            error = { CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText) },
                        )
                    } else {
                        CoverPlaceholder(game.displayTitle(EnglishTitles.enabled.value), game.serial, showText = placeholderText)
                    }
                },
            )
        }
    }
}

@Composable
private fun CoverPlaceholder(title: String, serial: String?, showText: Boolean) {
    Box(
        Modifier.fillMaxSize().background(
            Brush.linearGradient(
                listOf(MaterialTheme.colorScheme.primaryContainer, MaterialTheme.colorScheme.surfaceVariant),
            ),
        ),
        contentAlignment = Alignment.Center,
    ) {
        if (showText) {
            Column(Modifier.padding(8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                    textAlign = TextAlign.Center,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                )
                if (!serial.isNullOrBlank()) {
                    Text(
                        text = serial,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.66f),
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/** Vertical focus zones of the library screen, stacked top→bottom. */
enum class HomeZone { Toolbar, Search, Recents, Grid }

object HomeInputController {
    private var owner: HomeViewModel? = null
    private var openMenu: (() -> Unit)? = null
    private var columns = 3
    val scrollVelocity = mutableFloatStateOf(0f)

    // Three vertical zones stacked above the cover grid. The grid itself is
    // data-driven (owner.selectedIndex); the Recently-Played row and the top toolbar
    // (refresh / sort / layout / 2D-3D / background + the leading menu button) sit
    // above it and can't share that index space, so each is its own zone. From the
    // grid's top row, Up steps into Recents (if shown), then Up again into the
    // Toolbar; Down walks back down. Left/Right move within the focused row. A fires
    // the highlighted item. HomeScreen reads `zone` + `toolbarIndex` / `recentIndex`
    // to draw the highlight and registers the toolbar actions + recents launcher.
    val zone = mutableStateOf(HomeZone.Grid)
    val toolbarIndex = mutableIntStateOf(0)
    val recentIndex = mutableIntStateOf(0)
    private var toolbarActions: List<() -> Unit> = emptyList()
    private var recentCount = 0
    private var recentLauncher: ((Int) -> Unit)? = null
    private var searchAvailable = false
    private var searchConfirm: (() -> Unit)? = null
    private var toolbarAtBottom = false

    /** True once the user has actually moved the selector with the controller. The
     *  grid camera-follow is gated on this so a cold app-open rests at the TOP (toolbar
     *  / search / recently-played) instead of the follow snapping the grid to the
     *  initially-selected cover and hiding the whole header. Reset in bind() so each
     *  time the library (re)opens it starts at the top again. */
    var userNavigated = false

    /** Opens the per-game menu for a cover. Registered by HomeScreen because the menu's
     *  visibility is its own composable state, not something this object can hold. */
    private var openGameMenu: ((GameInfo) -> Unit)? = null

    fun bind(viewModel: HomeViewModel, onOpenMenu: () -> Unit, onOpenGameMenu: (GameInfo) -> Unit) {
        openGameMenu = onOpenGameMenu
        owner = viewModel
        openMenu = onOpenMenu
        userNavigated = false
    }

    fun unbind(viewModel: HomeViewModel) {
        if (owner === viewModel) {
            owner = null
            openMenu = null
            openGameMenu = null
            scrollVelocity.floatValue = 0f
            zone.value = HomeZone.Grid
        }
    }

    fun setColumnCount(value: Int) { columns = value.coerceAtLeast(1) }
    fun active(): Boolean = owner != null

    /** HomeScreen registers the toolbar button actions here, in visual left→right
     *  order (leading menu button first). */
    fun setToolbarActions(actions: List<() -> Unit>) { toolbarActions = actions }

    /** HomeScreen registers the currently-shown Recently-Played games (0 when the
     *  shelf is hidden, e.g. while searching). */
    fun setRecents(count: Int, launcher: (Int) -> Unit) {
        recentCount = count
        recentLauncher = launcher
        if (recentIndex.intValue >= count) recentIndex.intValue = (count - 1).coerceAtLeast(0)
        if (count == 0 && zone.value == HomeZone.Recents) zone.value = HomeZone.Grid
    }

    /** HomeScreen registers whether the search field is shown (only once the library
     *  has loaded) and how to focus it — A on the Search zone opens the keyboard. */
    fun setSearchAction(available: Boolean, action: () -> Unit) {
        searchAvailable = available
        searchConfirm = action
        if (!available && zone.value == HomeZone.Search) zone.value = HomeZone.Grid
    }

    /** HomeScreen registers where the view toolbar is drawn (App setting). At the
     *  bottom it's reached by pressing Down off the grid's last row instead of Up. */
    fun setToolbarAtBottom(value: Boolean) { toolbarAtBottom = value }

    /** The chrome zone directly above the grid, honoring which chrome is shown and
     *  whether the toolbar is at the top. Order top→bottom (toolbar-top): Toolbar,
     *  Search, Recents, Grid. When the toolbar is at the bottom it isn't above. */
    private fun zoneAboveGrid(): HomeZone = when {
        recentCount > 0 -> HomeZone.Recents
        searchAvailable -> HomeZone.Search
        !toolbarAtBottom && toolbarActions.isNotEmpty() -> HomeZone.Toolbar
        else -> HomeZone.Grid
    }

    fun move(dx: Int, dy: Int): Boolean {
        val viewModel = owner ?: return false
        userNavigated = true
        // Snapshot the highlight so we blip the nav sound only when it actually moves (not when a
        // press runs into an edge).
        val beforeZone = zone.value
        val beforeToolbar = toolbarIndex.intValue
        val beforeRecent = recentIndex.intValue
        val beforeSel = viewModel.state.value.selectedIndex
        when (zone.value) {
            HomeZone.Toolbar -> when {
                // Toolbar at top: Down descends into the chrome/grid. At bottom: Up
                // returns to the grid.
                !toolbarAtBottom && dy > 0 -> zone.value = when {
                    searchAvailable -> HomeZone.Search
                    recentCount > 0 -> HomeZone.Recents
                    else -> HomeZone.Grid
                }
                toolbarAtBottom && dy < 0 -> zone.value = HomeZone.Grid
                dx != 0 && toolbarActions.isNotEmpty() ->
                    toolbarIndex.intValue = (toolbarIndex.intValue + dx).coerceIn(0, toolbarActions.lastIndex)
            }
            HomeZone.Search -> when {
                dy < 0 -> if (!toolbarAtBottom && toolbarActions.isNotEmpty()) zone.value = HomeZone.Toolbar
                dy > 0 -> zone.value = if (recentCount > 0) HomeZone.Recents else HomeZone.Grid
            }
            HomeZone.Recents -> when {
                dy < 0 -> zone.value = when {
                    searchAvailable -> HomeZone.Search
                    !toolbarAtBottom && toolbarActions.isNotEmpty() -> HomeZone.Toolbar
                    else -> HomeZone.Recents
                }
                dy > 0 -> zone.value = HomeZone.Grid
                dx != 0 && recentCount > 0 ->
                    recentIndex.intValue = (recentIndex.intValue + dx).coerceIn(0, recentCount - 1)
            }
            HomeZone.Grid -> when {
                dx != 0 -> viewModel.moveSelection(dx)
                // Up: try to climb a row; if the selection didn't move we're on the top
                // row → step into the chrome above. This "move-then-check" is robust to
                // the exact per-row count (shelf/grid), unlike a selectedIndex<columns
                // guess — which is why Recently Played was being skipped in shelf view.
                dy < 0 -> {
                    val before = viewModel.state.value.selectedIndex
                    viewModel.moveSelection(-columns)
                    if (viewModel.state.value.selectedIndex == before && zoneAboveGrid() != HomeZone.Grid) {
                        zone.value = zoneAboveGrid()
                    }
                }
                // Down: climb a row; if it didn't move we're on the last row → step into
                // the bottom toolbar (only when the toolbar is drawn there).
                dy > 0 -> {
                    val before = viewModel.state.value.selectedIndex
                    viewModel.moveSelection(columns)
                    if (viewModel.state.value.selectedIndex == before && toolbarAtBottom && toolbarActions.isNotEmpty()) {
                        zone.value = HomeZone.Toolbar
                    }
                }
            }
        }
        if (zone.value != beforeZone || toolbarIndex.intValue != beforeToolbar ||
            recentIndex.intValue != beforeRecent || viewModel.state.value.selectedIndex != beforeSel
        ) com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
        return true
    }

    fun confirm(): Boolean {
        val viewModel = owner ?: return false
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
        when (zone.value) {
            HomeZone.Toolbar -> toolbarActions.getOrNull(toolbarIndex.intValue)?.invoke()
            HomeZone.Search -> searchConfirm?.invoke()
            HomeZone.Recents -> recentLauncher?.invoke(recentIndex.intValue)
            HomeZone.Grid -> {
                val game = viewModel.selectedGame() ?: return false
                viewModel.launch(game)
            }
        }
        return true
    }

    /** X (Square) on the highlighted cover: open its menu — the controller equivalent of a
     *  long-press, and the same menu touch gets. It replaced a direct jump to that game's
     *  settings, which reached exactly one of the menu's rows and hid the other five (per-game
     *  BIOS, pin to launcher, hide, drop from Recents, play) from anyone without a touchscreen.
     *  Settings is still one press away, as the second row. */
    fun openSelectedGameMenu(): Boolean {
        val game = owner?.selectedGame() ?: return false
        val open = openGameMenu ?: return false
        open(game)
        return true
    }

    fun scroll(velocity: Float): Boolean {
        if (owner == null) return false
        scrollVelocity.floatValue = if (abs(velocity) > 0.08f) velocity.coerceIn(-1f, 1f) else 0f
        return true
    }

    fun back(): Boolean {
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.BACK)
        // B in the Recents / Toolbar zone drops back to the grid; on the grid it
        // opens the nav drawer.
        if (zone.value != HomeZone.Grid) {
            zone.value = HomeZone.Grid
            return true
        }
        openMenu?.invoke() ?: return false
        return true
    }

    /** Cycle the library layout Grid → List → Shelf (bound to R1). Gives the
     *  controller direct access to all three view modes without needing to reach
     *  the touch-only toolbar toggle. */
    fun cycleLayout(): Boolean {
        val viewModel = owner ?: return false
        viewModel.toggleLayout()
        return true
    }

    /** Cycle the library sort order (bound to L1). */
    fun cycleSort(): Boolean {
        val viewModel = owner ?: return false
        val entries = HomeSort.entries
        val next = entries[(viewModel.state.value.sort.ordinal + 1) % entries.size]
        viewModel.setSort(next)
        return true
    }
}

// -- vivi's shelf library layout — covers standing on vivi's plank PNGs (frosted
//    for All Games, navy for Now Playing). Each cover sits seated on the plank's
//    top face (back-of-centre, like a book on a shelf) with shelf surface visible
//    in front of it, and a faint mirror reflection on that surface. --

@Composable
private fun GameShelf(
    games: List<GameInfo>,
    shelfRes: Int,
    coverWidth: Dp,
    scroll: Boolean,
    onLaunch: (GameInfo) -> Unit,
    onDetails: (GameInfo) -> Unit = {},
    modifier: Modifier = Modifier,
    slotsPerRow: Int = games.size,
    // Controller selection highlight: the global visibleGames index that's selected,
    // and this shelf row's first global index. -1 = nothing selected on this shelf.
    selectedIndex: Int = -1,
    startIndex: Int = 0,
) {
    val coverHeight = coverWidth / 0.7f
    // Slimmer plank to match bagas's slimmer frosted-shelf PNG (2903×200).
    val plankHeight = 84.dp
    // How far below the plank's back (top) edge the cover base sits — small, so the
    // cover rests toward the BACK of the top face with plenty of shelf in front of
    // it (not perched on the front edge). Scaled with the slimmer plank.
    val surfaceInset = 16.dp
    val reflectionHeight = coverHeight * 0.18f
    // Covers + reflection are top-anchored; the box is tall enough that the cover
    // base lands on the top face and the reflection lays over the shelf in front.
    // "Name on grid" also labels shelf covers (previously only the flat grid honoured it) — reserve
    // a line under the reflection for the title so it isn't clipped by the plank.
    val nameHeight = if (GridLabels.show.value) 18.dp else 0.dp
    val rowHeight = coverHeight + reflectionHeight + nameHeight
    Box(modifier.fillMaxWidth().height(coverHeight + plankHeight - surfaceInset + nameHeight)) {
        Image(
            painter = painterResource(shelfRes),
            contentDescription = null,
            // The shelf PNG carries a ~1.1% transparent/faded margin on each side, so
            // when stretched full-width its *visible* plank edge sits ~11dp inset while
            // the first/last covers reach the screen edge — making them overhang the
            // shelf's faded end. Scale the plank out ~5% horizontally so its solid
            // surface bleeds to (past) the screen edges and the covers sit on it.
            modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(plankHeight)
                .graphicsLayer { scaleX = 1.05f },
            contentScale = ContentScale.FillBounds,
        )
        if (scroll) {
            // Controller selection must drive the same blue highlight the non-scroll
            // path draws, and keep the selected cover on-screen — without this the
            // Recently-Played shelf (scroll = true) showed NO highlight, so it looked
            // like the controller couldn't select it.
            val rowState = rememberLazyListState()
            LaunchedEffect(selectedIndex) {
                val local = selectedIndex - startIndex
                if (local in games.indices) rowState.animateScrollToItem(local)
            }
            LazyRow(
                state = rowState,
                modifier = Modifier.align(Alignment.TopStart).fillMaxWidth().height(rowHeight),
                // 12dp start so the first cover lines up with the section header.
                contentPadding = PaddingValues(horizontal = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                itemsIndexed(games, key = { _, g -> "shelfcard_${shelfRes}_${g.uri}" }) { index, game ->
                    ShelfGameCard(
                        game, coverWidth, reflectionHeight,
                        selected = startIndex + index == selectedIndex,
                        onLaunch = onLaunch,
                        onDetails = onDetails,
                    )
                }
            }
        } else {
            // Covers laid left-to-right at their full-row positions. SpaceBetween
            // pins the first cover to the left padding (aligned with the section
            // header) and spreads the row across the shelf width. A short trailing
            // row is padded with invisible spacers so its covers stay packed on the
            // left in sequence instead of sprawling across the whole shelf.
            Row(
                modifier = Modifier.align(Alignment.TopStart).fillMaxWidth().height(rowHeight).padding(horizontal = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                games.forEachIndexed { i, game ->
                    ShelfGameCard(game, coverWidth, reflectionHeight, selected = startIndex + i == selectedIndex, onLaunch = onLaunch, onDetails = onDetails)
                }
                repeat((slotsPerRow - games.size).coerceAtLeast(0)) {
                    Spacer(Modifier.width(coverWidth))
                }
            }
        }
    }
}

@Composable
private fun ShelfGameCard(game: GameInfo, width: Dp, reflectionHeight: Dp, selected: Boolean = false, onLaunch: (GameInfo) -> Unit, onDetails: (GameInfo) -> Unit = {}) {
    // Long-press opens the game context menu (per-game settings, hide, etc.) — same as the grid /
    // list / recents cards. Without this the shelf layout had no way to reach per-game settings.
    Column(modifier = Modifier.width(width).combinedClickable(onClick = { onLaunch(game) }, onLongClick = { onDetails(game) })) {
        // Square corners in shelf view — rounding fought the 3D box-art edges. The
        // grid/cover view keeps rounded corners (GameCover's 12.dp default).
        // ContentScale.Fit shows the WHOLE cover — Crop was trimming the top off the
        // large standing covers.
        GameCover(
            game,
            Modifier.fillMaxWidth().aspectRatio(0.7f)
                .then(
                    if (selected)
                        Modifier.border(2.5.dp, Color(0xFF3DA5FF), RoundedCornerShape(4.dp))
                    else Modifier,
                ),
            cornerRadius = 0.dp,
            contentScale = ContentScale.Fit,
        )
        // A faint mirror of the cover on the shelf surface just in front of it.
        // clipToBounds keeps it to reflectionHeight — without it the full flipped
        // cover renders and bleeds down onto the row below.
        Box(Modifier.fillMaxWidth().height(reflectionHeight).clipToBounds()) {
            GameCover(
                game,
                Modifier.fillMaxWidth().aspectRatio(0.7f)
                    .graphicsLayer { scaleY = -1f; alpha = 0.18f },
                cornerRadius = 0.dp,
                contentScale = ContentScale.Fit,
                placeholderText = false,
            )
            // Fade the reflection out toward the front of the shelf.
            Box(Modifier.matchParentSize().background(Brush.verticalGradient(listOf(Color.Transparent, Color(0x55000000)))))
        }
        // Title under the cover when "Name on grid" is on — shelf covers honour it too now.
        if (GridLabels.show.value) {
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                style = MaterialTheme.typography.labelSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 2.dp),
            )
        }
    }
}

