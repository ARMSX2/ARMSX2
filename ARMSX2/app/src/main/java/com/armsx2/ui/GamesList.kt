package com.armsx2.ui

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.animateScrollBy
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.itemsIndexed as lazyItemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.R
import com.armsx2.EmuState
import com.armsx2.FilenameParser
import com.armsx2.CoverArtStyle
import com.armsx2.GameInfo
import com.armsx2.GamePlatform
import com.armsx2.Main
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin

private val GAME_EXTENSIONS = setOf(
    "iso", "chd", "cso", "zso", "gz", "bin", "mdf", "img", "nrg", "dump"
)

object GamesList {
    private const val KEY_LIBRARY_BACKGROUND = "library.background.path"
    /**
     * Cached list of games for the configured ROMs folder. Backed by
     * SharedPreferences (`gamesCache` JSON + `gamesCacheDir` URI) so a
     * cold start doesn't re-walk every file. The list is only refreshed
     * when:
     *   - the user changes their ROMs folder (auto-detected via the
     *     romsDir state — cached dir mismatch triggers a rescan), or
     *   - they explicitly tap the Refresh card in the grid.
     */
    private val games = mutableStateListOf<GameInfo>()
    private val scanning = mutableStateOf(false)
    private val scanError = mutableStateOf<String?>(null)
    private val lastScannedRoms = mutableStateOf<String?>(null)
    private val cacheLoaded = mutableStateOf(false)
    private val recentUris = mutableStateListOf<String>()
    private val recentLoaded = mutableStateOf(false)
    private val customBackgroundPath = mutableStateOf<String?>(null)
    private val customBackgroundLoaded = mutableStateOf(false)
    private val controllerSelectedUri = mutableStateOf<String?>(null)
    private data class ControllerGameRow(
        val rowId: Int,
        val games: List<GameInfo>,
        val listItemIndex: Int,
    )
    private data class ControllerGamePosition(
        val rowIndex: Int,
        val columnIndex: Int,
        val game: GameInfo,
    )
    private var controllerRows: List<ControllerGameRow> = emptyList()
    private var controllerRowIndex = 0
    private var controllerColumnIndex = 0
    private val controllerScrollVelocity = mutableStateOf(0f)

    // Controller focus zones: the top toolbar chips, the game grid, and the
    // bottom-left gear (global settings). The grid is the manual game model;
    // TOOLBAR/RAIL are the non-game buttons wired in here so the controller can
    // reach them. UP from the top grid row enters TOOLBAR; DOWN from the last
    // row enters RAIL.
    private enum class Zone { TOOLBAR, GRID, RAIL }
    private val controllerZone = mutableStateOf(Zone.GRID)
    private val controllerToolbarIndex = mutableStateOf(0)
    // Toolbar actions, published from HeaderRow composition so they capture the
    // live ActivityResult launchers / context. Label + click action, in order.
    private var controllerToolbarActions: List<Pair<String, () -> Unit>> = emptyList()

    fun controllerActive(): Boolean =
        WindowImpl.showLibrary.value || Main.eState.value == EmuState.STOPPED

    /** Reset focus to the game grid (call when the library (re)appears). */
    fun resetControllerZone() {
        controllerZone.value = Zone.GRID
        controllerToolbarIndex.value = 0
    }

    fun handleControllerMove(dx: Int, dy: Int): Boolean {
        if (!controllerActive()) return false

        when (controllerZone.value) {
            Zone.TOOLBAR -> {
                if (dy > 0) {
                    controllerZone.value = Zone.GRID
                } else if (dx != 0) {
                    val last = (controllerToolbarActions.size - 1).coerceAtLeast(0)
                    controllerToolbarIndex.value =
                        (controllerToolbarIndex.value + dx).coerceIn(0, last)
                }
                return true
            }
            Zone.RAIL -> {
                // Gear sits on the left rail — right returns to the grid.
                if (dx > 0) controllerZone.value = Zone.GRID
                return true
            }
            Zone.GRID -> { /* handled below */ }
        }

        val rows = controllerRows.filter { it.games.isNotEmpty() }
        if (rows.isEmpty()) {
            // No games yet — up reaches the toolbar, left reaches the gear, so
            // the controller can scan / open setup on a fresh install.
            if (dy < 0) controllerZone.value = Zone.TOOLBAR
            else if (dx < 0) controllerZone.value = Zone.RAIL
            return true
        }

        val current = controllerSelectedPosition(rows)
        if (dy != 0) {
            if (dy < 0 && current.rowIndex == 0) {
                controllerZone.value = Zone.TOOLBAR
                return true
            }
            val rowIndex = (current.rowIndex + dy).coerceIn(0, rows.lastIndex)
            val columnIndex = current.columnIndex.coerceIn(0, rows[rowIndex].games.lastIndex)
            selectControllerPosition(rows, rowIndex, columnIndex)
        } else if (dx != 0) {
            if (dx < 0 && current.columnIndex == 0) {
                // Left off the first cover of any shelf → the gear on the rail.
                controllerZone.value = Zone.RAIL
                return true
            }
            val row = rows[current.rowIndex]
            val columnIndex = (current.columnIndex + dx).coerceIn(0, row.games.lastIndex)
            selectControllerPosition(rows, current.rowIndex, columnIndex)
        }
        return true
    }

    fun handleControllerConfirm(): Boolean {
        if (!controllerActive()) return false
        when (controllerZone.value) {
            Zone.TOOLBAR ->
                controllerToolbarActions.getOrNull(controllerToolbarIndex.value)?.second?.invoke()
            Zone.RAIL -> InGameOverlay.openGlobalSettings()
            Zone.GRID -> {
                val rows = controllerRows.filter { it.games.isNotEmpty() }
                if (rows.isNotEmpty()) launchGame(controllerSelectedPosition(rows).game)
            }
        }
        return true
    }

    /** Open per-game settings for the currently-highlighted cover (controller
     *  equivalent of long-pressing a game). Used by the Menu hotkey / Y button
     *  while browsing the library. */
    fun openSelectedGameSettings(): Boolean {
        if (!controllerActive() || controllerZone.value != Zone.GRID) return false
        val rows = controllerRows.filter { it.games.isNotEmpty() }
        if (rows.isEmpty()) return false
        InGameOverlay.openGameSettings(controllerSelectedPosition(rows).game)
        return true
    }

    fun handleControllerBack(): Boolean {
        if (!controllerActive()) return false
        // From the toolbar/gear, back returns to the game grid first.
        if (controllerZone.value != Zone.GRID) {
            controllerZone.value = Zone.GRID
            return true
        }
        if (WindowImpl.showLibrary.value)
            WindowImpl.showLibrary.value = false
        return true
    }

    fun handleControllerScroll(velocity: Float): Boolean {
        if (!controllerActive()) {
            controllerScrollVelocity.value = 0f
            return false
        }
        controllerScrollVelocity.value = if (abs(velocity) > 0.08f) velocity.coerceIn(-1f, 1f) else 0f
        return true
    }

    @Composable
    fun GamesRow() {
        val context = LocalContext.current
        val romsDirs = Main.romsDirs.value

        LaunchedEffect(Unit) {
            if (!customBackgroundLoaded.value) {
                customBackgroundLoaded.value = true
                loadCustomBackground(context)
            }
        }

        // Stable cache key — order-independent join of all configured dirs.
        // Two-dir configs in either order hit the same cache, single-dir
        // matches the old format. Used for both "is the cache stale?"
        // checks and the cache write below.
        val romsKey = remember(romsDirs) { cacheKeyForDirs(romsDirs) }

        LaunchedEffect(romsKey) {
            if (romsDirs.isEmpty()) return@LaunchedEffect

            if (!recentLoaded.value) {
                recentLoaded.value = true
                loadRecents()
            }

            if (!cacheLoaded.value) {
                cacheLoaded.value = true
                val (cachedKey, cachedGames) = loadCache(context)
                if (cachedKey == romsKey) {
                    games.clear()
                    games.addAll(cachedGames)
                    lastScannedRoms.value = romsKey
                    return@LaunchedEffect
                }
            }

            if (lastScannedRoms.value != romsKey && !scanning.value) {
                scanRoms(context, romsDirs, romsKey)
            }
        }

        Box(
            Modifier
                .fillMaxSize()
                .let { if (WindowImpl.overlayVisible.value) it.blur(18.dp) else it }
                .background(
                    Brush.verticalGradient(
                        listOf(
                            Color(0xFF001124),
                            Color(0xFF002647),
                            Color(0xFF000711),
                        ),
                    ),
                )
                .padding(16.dp),
        ) {
            when {
                romsDirs.isEmpty() -> EmptyMessage(
                    "No ROMs folders configured",
                    "Use the Settings cog to add one or more.",
                )
                scanning.value && games.isEmpty() -> ScanningSpinner()
                scanError.value != null -> Text(
                    scanError.value!!,
                    color = Color(0xFFFF6B6B),
                )
                else -> {
                    LibraryScreen(context, romsDirs, romsKey)
                }
            }
        }
    }

    /** Sorted "|" join so ["A","B"] and ["B","A"] produce the same key.
     *  We dedupe within the user's selection in case they accidentally
     *  pick the same folder twice. The "|v3" suffix invalidates legacy
     *  caches that were built before .img/.mdf/.nrg/.dump were probed for
     *  serials and before DMC2 Dante/Lucia filename fallback landed — bump
     *  again any time the probe coverage changes. */
    private fun cacheKeyForDirs(dirs: List<String>): String =
        dirs.toSet().sorted().joinToString("|") + "|v3"

    @Composable
    private fun LibraryScreen(context: Context, romsDirs: List<String>, romsKey: String) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val landscape = maxWidth >= maxHeight
            if (landscape) {
                Row(
                    Modifier
                        .fillMaxSize()
                        .clip(RoundedCornerShape(28.dp))
                        .background(Color(0xFF00101F)),
                ) {
                    LibraryNavRail(landscape = true)
                    LibraryContent(
                        context = context,
                        romsDirs = romsDirs,
                        romsKey = romsKey,
                        landscape = true,
                        modifier = Modifier.weight(1f),
                    )
                }
            } else {
                Box(
                    Modifier
                        .fillMaxSize()
                        .clip(RoundedCornerShape(28.dp))
                        .background(Color(0xFF00101F)),
                ) {
                    LibraryContent(
                        context = context,
                        romsDirs = romsDirs,
                        romsKey = romsKey,
                        landscape = false,
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(bottom = 86.dp),
                    )
                    LibraryNavRail(
                        landscape = false,
                        modifier = Modifier.align(Alignment.BottomCenter),
                    )
                }
            }
        }
    }

    @Composable
    private fun LibraryContent(
        context: Context,
        romsDirs: List<String>,
        romsKey: String,
        landscape: Boolean,
        modifier: Modifier = Modifier,
    ) {
        val currentShelfGames = recentUris
            .mapNotNull { uri -> games.firstOrNull { it.uri.toString() == uri } }
            .take(5)
            .ifEmpty { games.take(if (landscape) 8 else 6) }
        val libraryRows = games.chunked(if (landscape) 8 else 5)
        val controllerLayoutRows = buildList {
            if (currentShelfGames.isNotEmpty()) add(currentShelfGames)
            addAll(libraryRows)
        }
        val firstLibraryRowItem = if (currentShelfGames.isNotEmpty()) 3 else 2
        val controllerRowsForUi = buildList {
            if (currentShelfGames.isNotEmpty()) add(ControllerGameRow(0, currentShelfGames, 1))
            libraryRows.forEachIndexed { index, row ->
                add(ControllerGameRow(index + 1, row, firstLibraryRowItem + index))
            }
        }
        val controllerVisibleKey = controllerLayoutRows
            .joinToString("||") { row -> row.joinToString("|") { it.uri.toString() } }
        val listState = rememberLazyListState()
        val density = LocalDensity.current

        LaunchedEffect(controllerVisibleKey, landscape) {
            updateControllerLayout(controllerRowsForUi)
        }

        LaunchedEffect(controllerSelectedUri.value, controllerVisibleKey) {
            controllerSelectedListItemIndex()?.let { index ->
                val visible = listState.layoutInfo.visibleItemsInfo
                val first = visible.firstOrNull()?.index
                val last = visible.lastOrNull()?.index
                when {
                    first == null || last == null -> listState.scrollToItem(index)
                    index < first -> listState.scrollToItem(index)
                    index > last -> {
                        val visibleSpan = (last - first).coerceAtLeast(0)
                        listState.scrollToItem((index - visibleSpan).coerceAtLeast(0))
                    }
                }
            }
        }

        // Bring the toolbar into view when the controller moves up into it; the
        // gear rail is always on-screen so it needs no scroll.
        LaunchedEffect(controllerZone.value) {
            if (controllerZone.value == Zone.TOOLBAR) listState.scrollToItem(0)
        }
        // Start library browsing on the game grid each time it's explicitly shown.
        LaunchedEffect(WindowImpl.showLibrary.value) {
            if (WindowImpl.showLibrary.value) resetControllerZone()
        }
        LaunchedEffect(listState) {
            var lastFrame = withFrameNanos { it }
            while (true) {
                val frame = withFrameNanos { it }
                val dt = ((frame - lastFrame).coerceAtMost(50_000_000L)).toFloat() / 1_000_000_000f
                lastFrame = frame
                val velocity = controllerScrollVelocity.value
                if (abs(velocity) > 0.08f) {
                    val pxPerSecond = with(density) { 1700.dp.toPx() }
                    listState.scrollBy(velocity * pxPerSecond * dt)
                }
            }
        }

        Box(modifier = modifier.fillMaxSize()) {
            val customBackground = customBackgroundPath.value
                ?.let { File(it) }
                ?.takeIf { it.exists() && it.length() > 0L }
            if (customBackground != null) {
                SubcomposeAsyncImage(
                    model = ImageRequest.Builder(context)
                        .data(customBackground)
                        .crossfade(true)
                        .build(),
                    contentDescription = null,
                    modifier = Modifier.matchParentSize(),
                    contentScale = ContentScale.Crop,
                    alignment = Alignment.Center,
                )
                Box(
                    Modifier
                        .matchParentSize()
                        .background(Color.Black.copy(alpha = 0.38f)),
                )
                Box(
                    Modifier
                        .matchParentSize()
                        .background(
                            Brush.verticalGradient(
                                listOf(Color.Black.copy(alpha = 0.10f), Color.Black.copy(alpha = 0.42f)),
                            ),
                        ),
                )
            } else {
                // Base reactor-blue wash (unchanged palette).
                Box(
                    Modifier
                        .matchParentSize()
                        .background(
                            Brush.radialGradient(
                                listOf(
                                    Color(0xFF07355E),
                                    Color(0xFF001C35),
                                    Color(0xFF000814),
                                ),
                            ),
                        ),
                )
                // "Inside the core reactor": the app's glowing monolith stands
                // tall behind the shelves, scaled past the screen height and
                // dimmed so it reads as ambient light rather than a foreground object.
                Image(
                    painter = painterResource(id = R.drawable.savetowerforeground),
                    contentDescription = null,
                    modifier = Modifier
                        .align(Alignment.Center)
                        .fillMaxHeight(1.25f)
                        .graphicsLayer { alpha = 0.30f },
                    contentScale = ContentScale.Fit,
                )
                // Vertical vignette so the tower glow falls off toward the bottom and
                // the shelf rows keep their contrast.
                Box(
                    Modifier
                        .matchParentSize()
                        .background(
                            Brush.verticalGradient(
                                listOf(Color.Transparent, Color(0x66000814)),
                            ),
                        ),
                )
            }
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(
                        horizontal = if (landscape) 26.dp else 18.dp,
                        vertical = if (landscape) 22.dp else 20.dp,
                    ),
                verticalArrangement = Arrangement.spacedBy(if (landscape) 18.dp else 16.dp),
                contentPadding = PaddingValues(bottom = 18.dp),
            ) {
            if (currentShelfGames.isNotEmpty()) {
                item(key = "__current_title__") {
                    HeaderRow(
                        title = "Recently Played",
                        context = context,
                        romsDirs = romsDirs,
                        romsKey = romsKey,
                    )
                }
                item(key = "__current_shelf__") {
                        GameShelf(
                            games = currentShelfGames,
                            label = null,
                            rowId = 0,
                            listItemIndex = 1,
                            coverWidth = if (landscape) 92.dp else 98.dp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(if (landscape) 220.dp else 194.dp),
                    )
                }
            } else {
                item(key = "__empty_actions__") {
                    HeaderRow(
                        title = "Library",
                        context = context,
                        romsDirs = romsDirs,
                        romsKey = romsKey,
                    )
                }
            }

            if (games.isNotEmpty()) {
                item(key = "__library_title__") {
                    SectionHeader("Library")
                }
                lazyItemsIndexed(libraryRows, key = { _, row ->
                    row.joinToString("|") { it.uri.toString() }
                }) { rowIndex, row ->
                    val label = shelfLabelFor(row)
                    GameShelf(
                        games = row,
                        label = label,
                        rowId = rowIndex + 1,
                        listItemIndex = firstLibraryRowItem + rowIndex,
                        coverWidth = if (landscape) 86.dp else 98.dp,
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(if (landscape) 204.dp else 230.dp),
                    )
                }
            }
            }
        }
    }

    @Composable
    private fun HeaderRow(
        title: String,
        context: Context,
        romsDirs: List<String>,
        romsKey: String,
    ) {
        // Boot ELF picker. Copies the chosen .elf into a real file under the
        // app data dir and boots it via the normal launch path — emucore's
        // AutoDetectSource recognizes the .elf extension (s_elf_override,
        // NoDisc), so homebrew / HDD-loader ELFs boot through the BIOS. We copy
        // (rather than pass the content URI) so the .elf suffix and a plain
        // file path are guaranteed regardless of the document provider.
        val bootElfLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult
            val name = DocumentFile.fromSingleUri(context, uri)?.name
                ?.takeIf { it.isNotEmpty() } ?: "boot.elf"
            val outName = if (name.endsWith(".elf", ignoreCase = true)) name else "$name.elf"
            val outFile = File(File(Main.assetCopyRoot(context), "elf").apply { mkdirs() }, outName)
            val copied = runCatching {
                context.contentResolver.openInputStream(uri)?.use { ins ->
                    outFile.outputStream().use { outs -> ins.copyTo(outs) }
                } != null
            }.getOrDefault(false)
            if (copied && outFile.length() > 0L) {
                WindowImpl.showLibrary.value = false
                Main.launchGame(outFile.absolutePath, null)
            }
        }
        val wallLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri != null)
                importCustomBackground(context, uri)
        }

        // Single source of truth for the toolbar so the rendered chips, the
        // controller highlight and the confirm action all stay in sync. Built
        // here in composition so the closures capture the live launchers/context;
        // published to the nav model via SideEffect.
        val toolbarActions: List<Pair<String, () -> Unit>> = buildList {
            add("Scan" to {
                when {
                    romsDirs.isEmpty() ->
                        Toast.makeText(context, "Choose a game folder first", Toast.LENGTH_SHORT).show()
                    scanning.value ->
                        Toast.makeText(context, "Library scan already running", Toast.LENGTH_SHORT).show()
                    else -> {
                        Toast.makeText(context, "Scanning library...", Toast.LENGTH_SHORT).show()
                        lastScannedRoms.value = null
                        scanRoms(context, romsDirs, romsKey)
                    }
                }
            })
            add("BIOS" to {
                WindowImpl.showLibrary.value = false
                Main.startBios()
            })
            add("ELF" to { bootElfLauncher.launch(arrayOf("*/*")) })
            add("Wall" to { wallLauncher.launch(arrayOf("image/*")) })
            add((if (CoverArtStyle.use3d.value) "Art: 3D" else "Art: 2D") to {
                CoverArtStyle.set(!CoverArtStyle.use3d.value)
            })
            if (customBackgroundPath.value != null) {
                add("Reset" to { resetCustomBackground(context) })
            }
            add("Cards" to { MemoryCardManager.visible.value = true })
            add("Setup" to {
                SetupImpl.resetForReentry()
                Main.reopenSetup()
            })
        }
        SideEffect { controllerToolbarActions = toolbarActions }
        val toolbarFocusIndex =
            if (controllerZone.value == Zone.TOOLBAR) controllerToolbarIndex.value else -1

        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            SectionHeader(
                text = title,
                modifier = Modifier.weight(1f),
            )
            toolbarActions.forEachIndexed { idx, (label, action) ->
                ActionChip(label = label, highlighted = idx == toolbarFocusIndex, onClick = action)
            }
        }
    }

    @Composable
    private fun ActionChip(label: String, highlighted: Boolean = false, onClick: () -> Unit) {
        val glow = Color(0xFF3DA5FF)
        Box(
            Modifier
                .height(34.dp)
                .then(
                    if (highlighted)
                        Modifier.shadow(10.dp, RoundedCornerShape(17.dp), ambientColor = glow, spotColor = glow)
                    else Modifier
                )
                .clip(RoundedCornerShape(17.dp))
                .background(Color.White.copy(alpha = if (highlighted) 0.20f else 0.10f))
                .border(
                    1.dp,
                    if (highlighted) glow else Color.White.copy(alpha = 0.18f),
                    RoundedCornerShape(17.dp),
                )
                .clickable(onClick = onClick)
                .padding(horizontal = 12.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                label,
                color = Color.White,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    @Composable
    private fun LibraryNavRail(landscape: Boolean, modifier: Modifier = Modifier) {
        if (landscape) {
            Column(
                modifier
                    .fillMaxHeight()
                    .width(82.dp)
                    .background(
                        Brush.verticalGradient(
                            listOf(Color(0xFF06416E), Color(0xFF002C50)),
                        ),
                    )
                    .padding(vertical = 28.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.SpaceBetween,
            ) {
                NavButton(NavKind.Library, "LIBRARY", active = true) {}
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                    modifier = Modifier.offset(y = 10.dp),
                ) {
                    ShelfScrollHint()
                    ControllerHint()
                }
                Box(Modifier.offset(y = 12.dp)) {
                    NavButton(
                        NavKind.Settings,
                        null,
                        active = false,
                        highlighted = controllerZone.value == Zone.RAIL,
                    ) { InGameOverlay.openGlobalSettings() }
                }
            }
        } else {
            Row(
                modifier
                    .fillMaxWidth()
                    .height(82.dp)
                    .background(
                        Brush.verticalGradient(
                            listOf(Color(0xCC0E4B79), Color(0xEE002E52)),
                        ),
                    )
                    .padding(horizontal = 28.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                NavButton(NavKind.Library, "LIBRARY", active = true) {}
                ShelfScrollHint()
                NavButton(
                    NavKind.Settings,
                    null,
                    active = false,
                    highlighted = controllerZone.value == Zone.RAIL,
                ) { InGameOverlay.openGlobalSettings() }
            }
        }
    }

    @Composable
    private fun NavButton(
        kind: NavKind,
        label: String?,
        active: Boolean,
        highlighted: Boolean = false,
        onClick: () -> Unit,
    ) {
        val glow = Color(0xFF3DA5FF)
        Column(
            Modifier
                .width(70.dp)
                .clip(RoundedCornerShape(16.dp))
                .clickable(onClick = onClick)
                .padding(vertical = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Box(
                Modifier
                    .size(42.dp)
                    .then(
                        if (highlighted)
                            Modifier.shadow(10.dp, CircleShape, ambientColor = glow, spotColor = glow)
                        else Modifier
                    )
                    .clip(if (active) RoundedCornerShape(12.dp) else CircleShape)
                    .background(
                        when {
                            highlighted -> glow.copy(alpha = 0.30f)
                            active -> Color.White.copy(alpha = 0.10f)
                            else -> Color.Transparent
                        }
                    )
                    .then(
                        if (highlighted) Modifier.border(1.dp, glow, CircleShape) else Modifier
                    ),
                contentAlignment = Alignment.Center,
            ) {
                NavGlyph(kind, active)
            }
            if (label != null) {
                Text(
                    label,
                    color = Color.White,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
            }
        }
    }

    private enum class NavKind { Library, Settings }

    @Composable
    private fun ShelfScrollHint() {
        Text(
            "Scroll up and down to navigate shelves, scroll left and right to reveal more games per shelf",
            color = Color.White.copy(alpha = 0.58f),
            fontSize = 9.sp,
            lineHeight = 10.sp,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center,
            modifier = Modifier.width(80.dp),
        )
    }

    /** Controller + touch help shown on the library rail. */
    @Composable
    private fun ControllerHint() {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                "On controller, press the X button to open per-game settings",
                color = Color.White.copy(alpha = 0.52f),
                fontSize = 8.sp,
                lineHeight = 10.sp,
                fontWeight = FontWeight.SemiBold,
                textAlign = TextAlign.Center,
                modifier = Modifier.width(80.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "On mobile, long press a game cover to open per-game settings",
                color = Color.White.copy(alpha = 0.52f),
                fontSize = 8.sp,
                lineHeight = 10.sp,
                fontWeight = FontWeight.SemiBold,
                textAlign = TextAlign.Center,
                modifier = Modifier.width(80.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "When in game, you can click the top right of the screen to make " +
                    "the gear icon pop up, clicking it will open the in-game pause " +
                    "overlay. On controller, you can set hotkeys for the menu and " +
                    "many other toggles.",
                color = Color.White.copy(alpha = 0.52f),
                fontSize = 8.sp,
                lineHeight = 10.sp,
                fontWeight = FontWeight.SemiBold,
                textAlign = TextAlign.Center,
                modifier = Modifier.width(80.dp),
            )
        }
    }

    @Composable
    private fun NavGlyph(kind: NavKind, active: Boolean) {
        val color = Color.White.copy(alpha = if (active) 1f else 0.78f)
        Canvas(Modifier.size(36.dp)) {
            val min = size.minDimension
            val stroke = Stroke(width = min * 0.075f, cap = StrokeCap.Round)
            when (kind) {
                NavKind.Library -> {
                    val cell = min * 0.24f
                    val gap = min * 0.16f
                    val total = cell * 2f + gap
                    val left = (size.width - total) / 2f
                    val top = (size.height - total) / 2f
                    repeat(2) { row ->
                        repeat(2) { col ->
                            drawRoundRect(
                                color = color,
                                topLeft = Offset(left + col * (cell + gap), top + row * (cell + gap)),
                                size = Size(cell, cell),
                                cornerRadius = CornerRadius(cell * 0.18f, cell * 0.18f),
                                style = stroke,
                            )
                        }
                    }
                }
                NavKind.Settings -> {
                    val center = Offset(size.width / 2f, size.height / 2f)
                    val base = min * 0.28f
                    val toothInner = min * 0.36f
                    val toothOuter = min * 0.46f
                    repeat(8) { index ->
                        val angle = (index * 45.0 - 90.0) * PI / 180.0
                        val dx = cos(angle).toFloat()
                        val dy = sin(angle).toFloat()
                        drawLine(
                            color = color,
                            start = Offset(center.x + dx * toothInner, center.y + dy * toothInner),
                            end = Offset(center.x + dx * toothOuter, center.y + dy * toothOuter),
                            strokeWidth = min * 0.075f,
                            cap = StrokeCap.Round,
                        )
                    }
                    drawCircle(color = color, radius = base, center = center, style = stroke)
                    drawCircle(color = color, radius = min * 0.095f, center = center, style = stroke)
                }
            }
        }
    }

    @Composable
    private fun SectionHeader(text: String, modifier: Modifier = Modifier) {
        Text(
            text,
            color = Color.White,
            fontSize = 24.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = modifier,
        )
    }

    @Composable
    private fun GameShelf(
        games: List<GameInfo>,
        label: String?,
        rowId: Int,
        listItemIndex: Int,
        coverWidth: Dp,
        modifier: Modifier = Modifier,
    ) {
        val rowState = rememberLazyListState()
        val selectedUri = controllerSelectedUri.value
        val density = LocalDensity.current

        LaunchedEffect(selectedUri, games) {
            val index = controllerSelectedColumnIndex(rowId) ?: return@LaunchedEffect
            val info = rowState.layoutInfo
            val itemInfo = info.visibleItemsInfo.firstOrNull { it.index == index }
            if (itemInfo == null) {
                // The cover isn't rendered at all — bring it to the leading edge.
                rowState.animateScrollToItem(index)
                return@LaunchedEffect
            }
            // Visible but possibly clipped at an edge. Nudge by exactly the
            // overflow (plus a small margin for the selection glow) so the whole
            // cover lands inside the viewport. The old code only scrolled when the
            // cover was fully past the last visible item, so the rightmost covers
            // — which sit half-visible as the trailing item — never slid in.
            val margin = with(density) { 18.dp.toPx() }
            val itemStart = itemInfo.offset.toFloat()
            val itemEnd = (itemInfo.offset + itemInfo.size).toFloat()
            val delta = when {
                itemStart - margin < info.viewportStartOffset ->
                    itemStart - margin - info.viewportStartOffset
                itemEnd + margin > info.viewportEndOffset ->
                    itemEnd + margin - info.viewportEndOffset
                else -> 0f
            }
            if (delta != 0f) rowState.animateScrollBy(delta)
        }

        Box(modifier) {
            ShelfGlass(
                label = label,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(76.dp),
            )
            LazyRow(
                state = rowState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(horizontal = 26.dp),
                horizontalArrangement = Arrangement.spacedBy(28.dp),
                verticalAlignment = Alignment.Bottom,
            ) {
                lazyItemsIndexed(games, key = { index, game -> "shelf_${listItemIndex}_${index}_${game.uri}" }) { columnIndex, game ->
                    ShelfGameCard(
                        game = game,
                        rowId = rowId,
                        columnIndex = columnIndex,
                        width = coverWidth,
                        modifier = Modifier.padding(bottom = 22.dp),
                    )
                }
            }
        }
    }

    @Composable
    private fun ShelfGlass(label: String?, modifier: Modifier = Modifier) {
        Canvas(modifier) {
            val w = size.width
            val h = size.height
            val stroke = Stroke(width = 1.dp.toPx())
            val top = Path().apply {
                moveTo(w * 0.10f, h * 0.04f)
                lineTo(w * 0.92f, h * 0.04f)
                lineTo(w * 0.995f, h * 0.46f)
                lineTo(w * 0.005f, h * 0.46f)
                close()
            }
            val face = Path().apply {
                moveTo(w * 0.005f, h * 0.46f)
                lineTo(w * 0.995f, h * 0.46f)
                lineTo(w * 0.975f, h * 0.82f)
                lineTo(w * 0.025f, h * 0.82f)
                close()
            }
            drawPath(
                top,
                brush = Brush.linearGradient(
                    colors = listOf(
                        Color(0x88D8F5FF),
                        Color(0x442A76B4),
                        Color(0x77E9FBFF),
                    ),
                ),
            )
            drawPath(
                face,
                brush = Brush.verticalGradient(
                    colors = listOf(
                        Color(0x55C9F4FF),
                        Color(0x303E607D),
                        Color(0x1AFFFFFF),
                    ),
                ),
            )
            drawPath(top, Color.White.copy(alpha = 0.18f), style = stroke)
            drawPath(face, Color.White.copy(alpha = 0.14f), style = stroke)
        }
        if (label != null) {
            Text(
                label,
                color = Color.White.copy(alpha = 0.35f),
                fontSize = 18.sp,
                modifier = modifier.padding(top = 42.dp),
                textAlign = TextAlign.Center,
            )
        }
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    private fun ShelfGameCard(
        game: GameInfo,
        rowId: Int,
        columnIndex: Int,
        width: Dp,
        modifier: Modifier = Modifier,
    ) {
        var cardFocused by remember { mutableStateOf(false) }
        val glowBlue = Color(0xFF3DA5FF)
        // Read the selection STATE (controllerSelectedUri) here so the card
        // recomposes when the controller moves the selection. The row/col
        // indices behind controllerCellSelected are plain vars, so without a
        // state read the glow stays frozen even though the model updates.
        val selectedUri = controllerSelectedUri.value
        val gridFocused = controllerZone.value == Zone.GRID
        val highlighted = cardFocused ||
            (gridFocused && selectedUri == game.uri.toString() &&
                controllerCellSelected(rowId, columnIndex))
        Column(
            modifier = modifier
                .width(width)
                .onFocusChanged {
                    cardFocused = it.isFocused
                    if (it.isFocused) selectControllerCell(rowId, columnIndex, game.uri.toString())
                }
                .combinedClickable(
                    onClick = { launchGame(game) },
                    onLongClick = { InGameOverlay.openGameSettings(game) },
                ),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            CoverArt(
                game = game,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.7f)
                    // Controller selection highlight: blue glow + outline on
                    // the cover when this card has D-pad focus.
                    .then(
                        if (highlighted)
                            Modifier.shadow(14.dp, RoundedCornerShape(6.dp), ambientColor = glowBlue, spotColor = glowBlue)
                        else Modifier
                    )
                    .clip(RoundedCornerShape(5.dp))
                    .then(
                        if (highlighted)
                            Modifier.border(3.dp, glowBlue, RoundedCornerShape(5.dp))
                        else Modifier
                    ),
            )
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(34.dp)
                    .clip(RoundedCornerShape(3.dp)),
            ) {
                CoverArt(
                    game = game,
                    modifier = Modifier
                        .fillMaxWidth()
                        .aspectRatio(0.7f)
                        .graphicsLayer {
                            scaleY = -1f
                            alpha = 0.22f
                        },
                )
                Box(
                    Modifier
                        .matchParentSize()
                        .background(
                            Brush.verticalGradient(
                                listOf(
                                    Color(0x0000172D),
                                    Color(0xAA00172D),
                                ),
                            ),
                        ),
                )
            }
            // Cover art already carries the title, so the only label is the
            // region flag — lets users tell regional versions apart without the
            // redundant (and cramped) text title under every card.
            game.regionFlag?.let { flag ->
                Text(
                    flag,
                    fontSize = 15.sp,
                    maxLines = 1,
                    textAlign = TextAlign.Center,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 6.dp),
                )
            }
        }
    }

    private fun shelfLabelFor(row: List<GameInfo>): String {
        fun labelFor(game: GameInfo): String =
            game.title.firstOrNull { it.isLetterOrDigit() }?.uppercaseChar()?.toString() ?: "#"
        if (row.isEmpty()) return ""
        val first = labelFor(row.first())
        val last = labelFor(row.last())
        return if (first == last) first else "$first - $last"
    }

    private fun launchGame(game: GameInfo) {
        controllerSelectedUri.value = game.uri.toString()
        WindowImpl.showLibrary.value = false
        markRecentlyPlayed(game)
        Main.launchGame(game.uri.toString(), game)
    }

    private fun updateControllerLayout(controllerRowsForUi: List<ControllerGameRow>) {
        controllerRows = controllerRowsForUi.filter { it.games.isNotEmpty() }
        if (controllerRows.isEmpty()) {
            controllerRowIndex = 0
            controllerColumnIndex = 0
            controllerSelectedUri.value = null
            return
        }

        val selected = controllerSelectedUri.value
        val rowIndex = controllerRowIndex.coerceIn(0, controllerRows.lastIndex)
        val columnIndex = controllerColumnIndex.coerceIn(0, controllerRows[rowIndex].games.lastIndex)
        if (selected != null && controllerRows[rowIndex].games[columnIndex].uri.toString() == selected) {
            selectControllerPosition(controllerRows, rowIndex, columnIndex)
            return
        }

        val found = selected?.let { findControllerPosition(it) }
        if (found != null)
            selectControllerPosition(controllerRows, found.rowIndex, found.columnIndex)
        else
            selectControllerPosition(controllerRows, 0, 0)
    }

    private fun controllerSelectedPosition(rows: List<ControllerGameRow> = controllerRows): ControllerGamePosition {
        val rowIndex = controllerRowIndex.coerceIn(0, rows.lastIndex)
        val columnIndex = controllerColumnIndex.coerceIn(0, rows[rowIndex].games.lastIndex)
        return ControllerGamePosition(rowIndex, columnIndex, rows[rowIndex].games[columnIndex])
    }

    private fun selectControllerPosition(rows: List<ControllerGameRow>, rowIndex: Int, columnIndex: Int) {
        controllerRowIndex = rowIndex.coerceIn(0, rows.lastIndex)
        controllerColumnIndex = columnIndex.coerceIn(0, rows[controllerRowIndex].games.lastIndex)
        controllerSelectedUri.value = rows[controllerRowIndex].games[controllerColumnIndex].uri.toString()
    }

    private fun findControllerPosition(uri: String): ControllerGamePosition? {
        controllerRows.forEachIndexed { rowIndex, row ->
            val columnIndex = row.games.indexOfFirst { it.uri.toString() == uri }
            if (columnIndex >= 0)
                return ControllerGamePosition(rowIndex, columnIndex, row.games[columnIndex])
        }
        return null
    }

    private fun selectControllerCell(rowId: Int, columnIndex: Int, uri: String) {
        val rowIndex = controllerRows.indexOfFirst { it.rowId == rowId }
        if (rowIndex >= 0)
            selectControllerPosition(controllerRows, rowIndex, columnIndex)
        else
            controllerSelectedUri.value = uri
    }

    private fun controllerCellSelected(rowId: Int, columnIndex: Int): Boolean {
        if (controllerRows.isEmpty()) return false
        val selected = controllerSelectedPosition()
        return controllerRows[selected.rowIndex].rowId == rowId &&
            selected.columnIndex == columnIndex
    }

    private fun controllerSelectedListItemIndex(): Int? {
        if (controllerRows.isEmpty()) return null
        return controllerRows[controllerSelectedPosition().rowIndex].listItemIndex
    }

    private fun controllerSelectedColumnIndex(rowId: Int): Int? {
        if (controllerRows.isEmpty()) return null
        val selected = controllerSelectedPosition()
        return if (controllerRows[selected.rowIndex].rowId == rowId)
            selected.columnIndex
        else
            null
    }

    private fun loadCustomBackground(context: Context) {
        val saved = Main.prefs.getString(KEY_LIBRARY_BACKGROUND, null)
            ?.takeIf { File(it).exists() }
        customBackgroundPath.value = saved
    }

    private fun importCustomBackground(context: Context, uri: Uri) {
        val name = DocumentFile.fromSingleUri(context, uri)?.name.orEmpty()
        val ext = name.substringAfterLast('.', missingDelimiterValue = "jpg")
            .lowercase()
            .takeIf { it in setOf("jpg", "jpeg", "png", "webp") }
            ?: "jpg"
        val outDir = File(Main.assetCopyRoot(context), "backgrounds").apply { mkdirs() }
        val outFile = File(outDir, "library_background.$ext")
        val ok = runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                outFile.outputStream().use { output -> input.copyTo(output) }
            } != null && outFile.length() > 0L
        }.getOrDefault(false)
        if (ok) {
            Main.prefs.edit().putString(KEY_LIBRARY_BACKGROUND, outFile.absolutePath).apply()
            customBackgroundPath.value = outFile.absolutePath
            Toast.makeText(context, "Library background imported", Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(context, "Could not import background", Toast.LENGTH_LONG).show()
        }
    }

    private fun resetCustomBackground(context: Context) {
        customBackgroundPath.value?.let { runCatching { File(it).delete() } }
        customBackgroundPath.value = null
        Main.prefs.edit().remove(KEY_LIBRARY_BACKGROUND).apply()
        Toast.makeText(context, "Library background reset", Toast.LENGTH_SHORT).show()
    }

    @Composable
    private fun EmptyMessage(title: String, body: String) {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
            Text(title, color = Color.White, fontSize = 18.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally))
            Spacer(Modifier.height(8.dp))
            Text(body, color = Color.LightGray, fontSize = 14.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally))
        }
    }

    @Composable
    private fun ScanningSpinner() {
        Row(Modifier.fillMaxSize(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center) {
            CircularProgressIndicator(
                modifier = Modifier.size(24.dp),
                strokeWidth = 2.dp,
                color = Colors.pasx2_blue,
            )
            Spacer(Modifier.width(12.dp))
            Text("Scanning ROMs…", color = Color.White)
        }
    }

    /** First grid slot — a refresh button styled like a game card.
     *  Caller passes Modifier.weight(1f) so this card and the BIOS card
     *  split the slot evenly. The icon Box uses weight(1f) too so the
     *  icon centers in whatever vertical space is left after the label. */
    @Composable
    private fun RefreshCard(
        modifier: Modifier = Modifier,
        isScanning: Boolean,
        onRefresh: () -> Unit,
    ) {
        Column(
            modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                // Already partial-alpha at 0.20, leave as-is; matches the
                // see-through aesthetic of the regular GameCard chrome.
                .background(Colors.pasx2_blue.copy(alpha = 0.20f))
                .border(2.dp, Colors.pasx2_blue.copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                .clickable(enabled = !isScanning, onClick = onRefresh),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .weight(1f),
                contentAlignment = Alignment.Center,
            ) {
                if (isScanning) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(36.dp),
                        strokeWidth = 3.dp,
                        color = Color.White,
                    )
                } else {
                    Text("⟳", color = Color.White, fontSize = 56.sp)
                }
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    if (isScanning) "Scanning…" else "Refresh",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    "Re-scan ROMs folder",
                    color = Color.LightGray,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }

    /** Stacked under RefreshCard in the first grid slot — boots the
     *  PS2 BIOS without a disc, leaving the library/toolbar visible.
     *  The caller passes Modifier.weight(1f) so this card stretches to
     *  match the height of adjacent GameCards. The icon is centered in
     *  the flexible region; the label sits at the bottom. */
    @Composable
    private fun BiosCard(modifier: Modifier = Modifier, onLaunch: () -> Unit) {
        Column(
            modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Colors.pasx2_blue.copy(alpha = 0.20f))
                .border(2.dp, Colors.pasx2_blue.copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                .clickable(onClick = onLaunch),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .weight(1f),
                contentAlignment = Alignment.Center,
            ) {
                Text("▶", color = Color.White, fontSize = 56.sp)
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    "Start BIOS",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    "Boot without a disc",
                    color = Color.LightGray,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }

    @Composable
    private fun UtilityCard(
        modifier: Modifier = Modifier,
        title: String,
        subtitle: String,
        onClick: () -> Unit,
    ) {
        Column(
            modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525).copy(alpha = 0.35f))
                .border(1.dp, Colors.pasx2_blue.copy(alpha = 0.25f), RoundedCornerShape(8.dp))
                .clickable(onClick = onClick)
                .padding(10.dp),
            verticalArrangement = Arrangement.Center,
        ) {
            Text(
                title,
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(3.dp))
            Text(
                subtitle,
                color = Color.LightGray,
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    private fun GameCard(game: GameInfo) {
        val selected = controllerSelectedUri.value == game.uri.toString()
        val glowBlue = Color(0xFF3DA5FF)
        Column(
            Modifier
                .fillMaxWidth()
                .then(
                    if (selected)
                        Modifier.shadow(14.dp, RoundedCornerShape(8.dp), ambientColor = glowBlue, spotColor = glowBlue)
                    else Modifier
                )
                .clip(RoundedCornerShape(8.dp))
                // Card chrome (background + border) is partial-alpha so the
                // live game surface shows through when the library is
                // overlaid mid-play. The cover image painted into the inner
                // Box and the title/serial Text below stay fully opaque
                // because they're separate paint layers — alpha here only
                // affects the chrome, not the children.
                .background(Color(0xFF272525).copy(alpha = 0.3f))
                .border(
                    if (selected) 2.dp else 1.dp,
                    if (selected) glowBlue else Color(0xFF3A3A3A).copy(alpha = 0.3f),
                    RoundedCornerShape(8.dp),
                )
                .combinedClickable(
                    onClick = {
                        // Clear the library overlay (set by LoadGameButton while
                        // a game was running) so the surface for the new game
                        // comes up uncovered. No-op when starting from idle.
                        // Pass GameInfo so the in-game overlay has cover art /
                        // extension badge / pre-resolved compat stars ready.
                        launchGame(game)
                    },
                    onLongClick = { InGameOverlay.openGameSettings(game) },
                ),
        ) {
            CoverArt(
                game = game,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.7f),
            )
            Column(Modifier.padding(8.dp)) {
                Text(
                    game.title,
                    color = Color.White,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    // Always reserve 2 lines so 1-line titles don't make
                    // their cards shorter than 2-line ones — keeps the
                    // grid rows uniform.
                    minLines = 2,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    game.serial?.let { s -> game.region?.let { "$s · $it" } ?: s } ?: "Unknown serial",
                    color = if (game.serial != null) Color(0xFFAACCFF) else Color(0xFF6F6F6F),
                    fontSize = 10.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(4.dp))
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CompatibilityStars(game.compatibility)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (game.extension.isNotEmpty()) ExtensionBadge(game.extension)
                        // Hardcore mode is a global RetroAchievements flag —
                        // when it's on, ANY game launched from here will
                        // apply, so render the HC badge on every card. The
                        // overlay's polling loop owns the flag.
                        if (InGameOverlay.hardcoreOn.value) {
                            Spacer(Modifier.width(4.dp))
                            HardcoreBadge()
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun CoverArt(game: GameInfo, modifier: Modifier = Modifier) {
        val context = LocalContext.current
        Box(
            modifier
                .background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
            contentAlignment = Alignment.Center,
        ) {
            val coverUrl = game.coverUrl
            if (coverUrl != null) {
                val use3d = CoverArtStyle.use3d.value // re-key cache on style change
                val coverFile = remember(game.serial, game.platform, use3d) { coverFileFor(context, game) }
                val localCoverReady = remember(coverFile?.absolutePath) {
                    mutableStateOf(coverFile?.let { it.exists() && it.length() > 0L } == true)
                }
                LaunchedEffect(coverUrl, coverFile?.absolutePath) {
                    if (!localCoverReady.value && coverFile != null) {
                        localCoverReady.value = mirrorCoverToFile(coverUrl, coverFile)
                    }
                }
                // SubcomposeAsyncImage so we can render a real fallback
                // composable when the cover URL 404s — happens for any
                // game the xlenore covers repo doesn't have (obscure
                // releases, regional dumps whose serial isn't covered).
                //
                // Per-platform contentScale: PS2 boxart is taller than
                // wide (matches the cell's 0.7 aspect ratio cleanly), so
                // ContentScale.Crop fills the cell with no margin. PS1
                // jewel-case covers are squarer/wider — cropping would
                // chop the sides; ContentScale.Fit + Center letterboxes
                // the cover inside the taller cell so the full art is
                // visible. Cell aspect stays uniform so the grid layout
                // doesn't shift between platforms.
                val scale = when (game.platform) {
                    GamePlatform.PS2 -> ContentScale.Crop
                    GamePlatform.PS1 -> ContentScale.Fit
                }
                val coverModel: Any = if (localCoverReady.value && coverFile != null) coverFile else coverUrl
                SubcomposeAsyncImage(
                    model = ImageRequest.Builder(context)
                        .data(coverModel)
                        .crossfade(true)
                        .build(),
                    contentDescription = "${game.title} cover",
                    contentScale = scale,
                    alignment = Alignment.Center,
                    modifier = Modifier.fillMaxSize(),
                    loading = { CoverLoadingTile() },
                    error = { NoCoverTile(missingSerial = false) },
                )
            } else {
                NoCoverTile(missingSerial = true)
            }
        }
    }

    private fun coverFileFor(context: Context, game: GameInfo): File? {
        val serial = game.serial ?: return null
        val coversDir = File(Main.assetCopyRoot(context), "covers")
        // Cache each style separately so toggling 2D/3D doesn't reuse the
        // wrong cached image.
        return if (CoverArtStyle.use3d.value)
            File(coversDir, "${serial}_3d.png")
        else
            File(coversDir, "$serial.jpg")
    }

    private suspend fun mirrorCoverToFile(url: String, target: File): Boolean = withContext(Dispatchers.IO) {
        if (target.exists() && target.length() > 0L)
            return@withContext true

        val parent = target.parentFile ?: return@withContext false
        if (!parent.exists() && !parent.mkdirs())
            return@withContext false

        val tmp = File(parent, ".${target.name}.${System.nanoTime()}.tmp")
        var conn: HttpURLConnection? = null
        try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 10_000
                readTimeout = 15_000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", "ARMSX2 Android")
            }
            if (conn.responseCode !in 200..299)
                return@withContext false

            conn.inputStream.use { input ->
                tmp.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            if (tmp.length() <= 0L)
                return@withContext false

            if (target.exists() && !target.delete())
                return@withContext false
            if (!tmp.renameTo(target)) {
                tmp.copyTo(target, overwrite = true)
                tmp.delete()
            }
            target.exists() && target.length() > 0L
        } catch (_: Exception) {
            false
        } finally {
            tmp.delete()
            conn?.disconnect()
        }
    }

    private fun markRecentlyPlayed(game: GameInfo) {
        val uri = game.uri.toString()
        recentUris.remove(uri)
        recentUris.add(0, uri)
        while (recentUris.size > 10) recentUris.removeAt(recentUris.lastIndex)
        saveRecents()
    }

    private fun loadRecents() {
        val raw = Main.prefs.getString("recentGameUris", null) ?: return
        try {
            val arr = JSONArray(raw)
            recentUris.clear()
            for (i in 0 until arr.length()) {
                val uri = arr.optString(i)
                if (uri.isNotEmpty() && uri !in recentUris) recentUris.add(uri)
            }
        } catch (_: Exception) {
            recentUris.clear()
        }
    }

    private fun saveRecents() {
        val arr = JSONArray()
        recentUris.forEach { arr.put(it) }
        Main.prefs.edit().putString("recentGameUris", arr.toString()).apply()
    }

    /** Small PS2-blue rounded chip showing the container format (ISO /
     *  CHD / BIN / etc.). Sits to the right of the compatibility stars. */
    @Composable
    private fun ExtensionBadge(ext: String) {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Colors.pasx2_blue)
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                ext,
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    /** RetroAchievements Hardcore badge. Shown on library cards while the
     *  global hardcore flag is set (so the user knows whichever game they
     *  launch will run in HC). Same shape as ExtensionBadge for visual
     *  consistency. */
    @Composable
    private fun HardcoreBadge() {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Color(0xFFB22222))
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                "HC",
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    /** Subtle spinner shown while the network fetch for a cover is in
     *  flight. Same dim background as the empty cover tile so the tile
     *  doesn't flash bright while loading. */
    @Composable
    private fun CoverLoadingTile() {
        Box(
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
            contentAlignment = Alignment.Center,
        ) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
                color = Color(0xFF555555),
            )
        }
    }

    /** Fallback cover tile rendered when:
     *    - missingSerial = true  (we couldn't extract a serial at all
     *                             → no URL to even try) → "?" in red-ish tint
     *    - missingSerial = false (URL was tried and 404'd / network err)
     *                            → disc icon + "No cover" subtitle
     *  In both cases the title + serial are still visible below the tile.
     */
    @Composable
    private fun NoCoverTile(missingSerial: Boolean) {
        Box(
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                if (missingSerial) {
                    Text("?", color = Color(0xFF8A4A4A), fontSize = 56.sp)
                    Text("No serial", color = Color(0xFF8A4A4A), fontSize = 10.sp)
                } else {
                    Text("📀", color = Color(0xFF3F3F3F), fontSize = 56.sp)
                    Text("No cover", color = Color(0xFF6F6F6F), fontSize = 10.sp)
                }
            }
        }
    }

    @Composable
    private fun CompatibilityStars(filled: Int) {
        Row {
            repeat(5) { i ->
                val on = i < filled
                Text(
                    if (on) "★" else "☆",
                    color = if (on) Color(0xFFFFD33A) else Color(0xFF555555),
                    fontSize = 12.sp,
                )
            }
        }
    }

    /**
     * Walk every configured ROM folder via DocumentFile, probe each disc
     * image's SYSTEM.CNF for its real serial + platform when possible,
     * and persist merged results to prefs. Falls back to filename-based
     * serial extraction for compressed formats (CHD/CSO/GZ) or non-ISO9660
     * raw images.
     *
     * De-duplication: same URI in two configured folders (rare — would
     * require nested SAF mounts) is merged by URI string. Title sort is
     * case-insensitive across the union.
     */
    private fun scanRoms(context: Context, romsUriStrings: List<String>, romsKey: String) {
        scanning.value = true
        scanError.value = null
        lastScannedRoms.value = romsKey
        Main.invoke {
            try {
                val collected = linkedMapOf<String, GameInfo>() // URI → info, preserves first-seen order
                for (dirUri in romsUriStrings) {
                    val uri = try { Uri.parse(dirUri) } catch (_: Exception) { null } ?: continue
                    val tree = DocumentFile.fromTreeUri(context, uri) ?: continue
                    val files = tree.listFiles()
                    for (f in files) {
                        if (!f.isFile) continue
                        val name = f.name ?: continue
                        val ext = name.substringAfterLast('.', "").lowercase()
                        if (ext !in GAME_EXTENSIONS) continue

                        // ISO9660 probe (PS2 BOOT2 + PS1 BOOT). Native
                        // returns "<platform>:<serial>" tag — e.g.
                        // "ps1:SLUS-00713" — when SYSTEM.CNF is parseable.
                        // Compressed formats (cso/zso/gz) fall through to
                        // filename. Raw-sector formats (.img/.mdf/.nrg/
                        // .dump) are handled by the same plain-ISO path in
                        // native — they fall through to the 2352/16 and
                        // 2352/24 fallbacks if 2048/0 fails.
                        val rawProbe = when (ext) {
                            "iso", "bin", "chd", "img", "mdf", "nrg", "dump" ->
                                probeDiscSerial(context, f.uri)
                            else -> null
                        }
                        val (probeSerial, probePlatform) = parseProbeResult(rawProbe)
                        val (titleFromName, serialFromName) = FilenameParser.parse(name)
                        val finalSerial = probeSerial ?: serialFromName
                        val finalPlatform = probePlatform ?: GamePlatform.PS2
                        // PCSX2 gamedb returns 0..6 (Unknown / Nothing /
                        // Intro / Menu / InGame / Playable / Perfect).
                        // Map 0,1 → 0 stars; 2..6 → 1..5 stars. PS1 will
                        // typically miss the PS2-only gamedb and stay 0.
                        val compatRaw = if (finalSerial != null)
                            NativeApp.getCompatibilityForSerial(finalSerial) else 0
                        val compatStars = (compatRaw - 1).coerceIn(0, 5)
                        val info = GameInfo(
                            uri = f.uri,
                            title = titleFromName,
                            serial = finalSerial,
                            compatibility = compatStars,
                            extension = ext.uppercase(),
                            platform = finalPlatform,
                        )
                        collected.putIfAbsent(f.uri.toString(), info)
                    }
                }
                val sorted = collected.values.sortedBy { it.title.lowercase() }
                games.clear()
                games.addAll(sorted)
                saveCache(context, romsKey, sorted)
            } catch (e: Exception) {
                scanError.value = "Scan failed: ${e.message}"
            } finally {
                scanning.value = false
            }
        }
    }

    /** Split a "ps1:SLUS-00713" / "ps2:SLUS-20312" probe return into
     *  (serial, platform). Untagged input (legacy native or filename-only)
     *  falls back to (input, null). null/empty → (null, null). */
    private fun parseProbeResult(raw: String?): Pair<String?, GamePlatform?> {
        if (raw.isNullOrEmpty()) return null to null
        val colon = raw.indexOf(':')
        if (colon <= 0) return raw to null
        val tag = raw.substring(0, colon)
        val serial = raw.substring(colon + 1)
        return serial to GamePlatform.fromKey(tag)
    }

    /** Open `uri` for read, hand the fd to native, get the
     *  "<platform>:<serial>" back. */
    private fun probeDiscSerial(context: Context, uri: Uri): String? {
        return try {
            val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: return null
            val fd = pfd.detachFd()
            NativeApp.getGameSerialFromFd(fd) // consumes fd
        } catch (_: Exception) {
            null
        }
    }

    // ---------------- Prefs cache ----------------

    private fun saveCache(context: Context, romsKey: String, list: List<GameInfo>) {
        val arr = JSONArray()
        for (g in list) {
            arr.put(JSONObject().apply {
                put("uri", g.uri.toString())
                put("title", g.title)
                put("serial", g.serial ?: JSONObject.NULL)
                put("compat", g.compatibility)
                put("ext", g.extension)
                put("platform", g.platform.key)
            })
        }
        Main.prefs.edit()
            .putString("gamesCacheKey", romsKey)
            .putString("gamesCache", arr.toString())
            .apply()
    }

    private fun loadCache(context: Context): Pair<String?, List<GameInfo>> {
        val prefs = Main.prefs
        // New key format ("|"-joined sorted dir set) under "gamesCacheKey".
        // Legacy key (single dir) was under "gamesCacheDir" — read either,
        // prefer the new one. Caller compares against the current
        // cacheKeyForDirs() so a legacy single-dir cache still matches an
        // unchanged single-dir config.
        val cachedKey = prefs.getString("gamesCacheKey", null)
            ?: prefs.getString("gamesCacheDir", null)
        val cachedJson = prefs.getString("gamesCache", null) ?: return cachedKey to emptyList()
        return try {
            val arr = JSONArray(cachedJson)
            val list = mutableListOf<GameInfo>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val rawSerial = if (obj.isNull("serial")) null else obj.optString("serial").takeIf { it.isNotEmpty() }
                list.add(GameInfo(
                    uri = Uri.parse(obj.getString("uri")),
                    title = obj.getString("title"),
                    serial = rawSerial,
                    compatibility = obj.optInt("compat", 0),
                    extension = obj.optString("ext", "").ifEmpty {
                        obj.getString("uri").substringAfterLast('.', "").uppercase()
                    },
                    platform = GamePlatform.fromKey(
                        if (obj.isNull("platform")) null else obj.optString("platform")
                    ),
                ))
            }
            cachedKey to list
        } catch (_: Exception) {
            cachedKey to emptyList()
        }
    }
}
