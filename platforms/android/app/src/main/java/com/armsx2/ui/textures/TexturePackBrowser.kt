package com.armsx2.ui.textures

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.compose.SubcomposeAsyncImage
import com.armsx2.TextureCatalog
import com.armsx2.TexturePackInstallState
import com.armsx2.TexturePackInstallState.InstallAction
import com.armsx2.TexturePackLinks
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.common.GameCoverPlaceholder
import com.armsx2.ui.common.PadModal
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable

/**
 * The online texture packs, full screen, a page of cubes at a time: each pack is its game's cover,
 * its name, and its creator with their picture. Shaped after Online Icons: the controller moves
 * between the cubes that are composed, and left or right at the edge of a page turns it, so the
 * pages flip rather than scroll. Sort by game, serial or creator (the creators then are cubes of
 * their own, each opening their packs); show every pack, the ones for the player's games, or the
 * installed ones; search names, games, serials and creators. A pack opens its window (PackWindow),
 * where Download is, and a download goes on when the browser closes ([TexturePackDownloads]).
 */
@Composable
internal fun TexturePackBrowser(
    packs: List<TextureCatalog.Pack>,
    links: Map<String, TexturePackLinks.Links>,
    /** The game in context, if any: the browser opens on My games rather than Popular Today, and its
     *  packs install there. */
    contextSerial: String?,
    /** Serials in the player's library, for My games. */
    librarySerials: Set<String>,
    /** Serials of games the player can boot: a multi-region pack's cover and install folder. */
    ownedSerials: Set<String>,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current
    val installRevision = TexturePackInstallState.revision.value
    val installed = remember(installRevision) { TexturePackInstallState.all() }
    val busyId = TexturePackDownloads.busyPackId.value
    val fraction = TexturePackDownloads.progressFraction.floatValue

    var sortMode by rememberSaveable { mutableIntStateOf(SORT_GAME) }
    // Opens on Popular Today, as Online Icons does; from a game's own texture settings, on its packs.
    var filter by rememberSaveable { mutableIntStateOf(if (contextSerial != null) FILTER_MINE else FILTER_POPULAR) }
    var query by rememberSaveable { mutableStateOf("") }
    // The creator whose packs are showing, in creator mode; null shows the creators.
    var creatorKey by rememberSaveable { mutableStateOf<String?>(null) }
    var page by rememberSaveable { mutableIntStateOf(0) }
    var openPackId by rememberSaveable { mutableStateOf<String?>(null) }

    // Popular Today: asked of the counter every time the browser opens, as Online Icons does.
    LaunchedEffect(Unit) { TexturePackStats.refreshPopular() }
    val popular = TexturePackStats.popular.value
    val popularAsked = TexturePackStats.asked.value

    val shownPacks = remember(packs, links, installed, filter, query, sortMode, popular) {
        val q = query.trim().lowercase()
        val matches = { p: TextureCatalog.Pack ->
            q.isEmpty() || p.name.lowercase().contains(q) || p.gameTitle.lowercase().contains(q) ||
                p.serials.any { it.lowercase().contains(q) } || creatorOf(p, links[p.id]).lowercase().contains(q)
        }
        if (filter == FILTER_POPULAR) {
            // In the counter's order, most downloaded first.
            val byId = packs.associateBy { it.id }
            popular.orEmpty().mapNotNull { byId[it] }.filter(matches)
        } else {
            packs.filter { p ->
                val keep = when (filter) {
                    FILTER_MINE -> p.matchesSerial(contextSerial) || p.serials.any { it.uppercase() in librarySerials }
                    FILTER_INSTALLED -> installed[p.id] != null
                    else -> true
                }
                keep && matches(p)
            }.sortedWith(if (sortMode == SORT_SERIAL) BY_SERIAL else BY_GAME)
        }
    }
    val groups = remember(shownPacks, links) { creatorGroups(shownPacks, links) }
    val creatorTotal = remember(packs, links) { creatorGroups(packs, links).count { it.key.isNotEmpty() } }
    val viewingCreator = sortMode == SORT_CREATOR && creatorKey != null
    val creator = if (viewingCreator) groups.firstOrNull { it.key == creatorKey } else null
    // What the pages hold: packs, or in creator mode the creators until one is picked.
    val packItems: List<TextureCatalog.Pack>? = when {
        sortMode != SORT_CREATOR -> shownPacks
        viewingCreator -> creator?.packs.orEmpty()
        else -> null
    }
    val count = packItems?.size ?: groups.size
    // Shuffle, on All only, as in Online Icons: turning it on leaves the page as it is, and from then
    // on every turn, either way, deals a page of random cubes ([dealt], indices into the list) instead
    // of the next in order. Off, the list is back in order from the page the first cube showing is on.
    var shuffleOn by remember { mutableStateOf(false) }
    var dealt by remember { mutableStateOf<List<Int>?>(null) }
    // A new filter, search or sort starts at the first page. Going into a creator and back out does
    // not: those two set the page themselves (openCreator, backToCreators).
    LaunchedEffect(filter, query, sortMode) {
        page = 0
        dealt = null
    }
    // How many cubes a page holds, as last laid out, for working out which page a creator is on.
    var perPageNow by remember { mutableIntStateOf(1) }
    // A cube for the pad to land on once the page that has it is drawn: going in or out of a creator
    // changes what every slot holds, and the slot wanted may not exist until then.
    var selectNext by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(selectNext) {
        val id = selectNext ?: return@LaunchedEffect
        androidx.compose.runtime.withFrameNanos { }
        SettingsControllerNav.selectById(id)
        selectNext = null
    }
    // Only a controller has a selection to move; a touch leaves none, and gets none.
    val padSelecting = { SettingsControllerNav.currentSelectedId() != null }
    val openCreator = { key: String ->
        creatorKey = key
        page = 0
        dealt = null
        if (padSelecting()) selectNext = "$LAYER.tile.0"
    }
    // Back to the creators, on the page with the one just left and that one selected, not the first.
    val backToCreators = {
        val left = creatorKey
        creatorKey = null
        dealt = null
        val index = groups.indexOfFirst { it.key == left }
        if (index >= 0) {
            val per = perPageNow.coerceAtLeast(1)
            page = index / per
            if (padSelecting()) selectNext = "$LAYER.tile.${index % per}"
        }
    }

    val unknownName = str("textures.creators.unknown")
    val onePack = str("textures.creators.onePack")
    val manyPacks = str("textures.creators.packs")
    val packCount = { n: Int -> if (n == 1) onePack else manyPacks.replace("%d", "%,d".format(n)) }

    PadModal(
        key = LAYER,
        // Back from a creator's packs goes to the creators first; from anywhere else it closes.
        onDismiss = { if (viewingCreator) backToCreators() else onClose() },
        scrimAlpha = 1f,
        initialFocusId = "$LAYER.tile.0",
    ) {
        Box(Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF0E1530), Color(0xFF080B16))))) {
            BoxWithConstraints(Modifier.fillMaxSize().padding(horizontal = 20.dp, vertical = 12.dp)) {
                val narrow = maxWidth < 760.dp
                Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Header(
                        title = if (viewingCreator) creator?.name?.ifEmpty { unknownName } ?: unknownName
                        else str("textures.online.title"),
                        subtitle = if (viewingCreator) packCount(creator?.packs?.size ?: 0)
                        else if (filter == FILTER_POPULAR) str("textures.browser.popularNote")
                        else str("textures.browser.counts")
                            .replace("%1\$s", "%,d".format(packs.size))
                            .replace("%2\$s", "%,d".format(creatorTotal)) +
                            "  ·  " + str("textures.browser.credit"),
                        avatar = if (viewingCreator) creator else null,
                    )
                    Controls(
                        narrow = narrow,
                        viewingCreator = viewingCreator,
                        sortMode = sortMode,
                        filter = filter,
                        query = query,
                        onBack = backToCreators,
                        // Creator again, from inside a creator, is the way back to the creators.
                        onSort = { mode ->
                            if (mode == SORT_CREATOR && viewingCreator) backToCreators() else {
                                sortMode = mode
                                creatorKey = null
                            }
                        },
                        onFilter = { filter = it },
                        onQuery = { query = it },
                        shuffleOn = shuffleOn,
                        onShuffle = {
                            if (shuffleOn) {
                                // Back in order, at the page the first cube showing sits on.
                                dealt?.firstOrNull()?.let { anchor -> page = anchor / perPageNow.coerceAtLeast(1) }
                                dealt = null
                            }
                            shuffleOn = !shuffleOn
                        },
                    )
                    BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                        // Cubes as large as two rows allow on a handheld held sideways, more rows where
                        // the screen is taller; then as many columns as fit, widened to fill the row.
                        val gridH = maxHeight - FOOTER_H
                        val rows = (gridH / ROW_TARGET).toInt().coerceAtLeast(1).let { r ->
                            if (gridH / r > ROW_TARGET * 1.45f) r + 1 else r
                        }
                        val tileH = (gridH - GAP * (rows - 1)) / rows
                        val cols = ((maxWidth + GAP) / (tileH / ASPECT + GAP)).toInt().coerceAtLeast(1)
                        val tileW = minOf((maxWidth - GAP * (cols - 1)) / cols, tileH * 1.3f)
                        val perPage = rows * cols
                        androidx.compose.runtime.SideEffect { perPageNow = perPage }
                        val pages = ((count + perPage - 1) / perPage).coerceAtLeast(1)
                        val current = page.coerceAtMost(pages - 1)
                        val first = current * perPage
                        // Shuffling: Shuffle on, on All, with more than a page to deal from.
                        val shuffling = shuffleOn && filter == FILTER_ALL && count > perPage
                        val onPage: List<Int> = (if (shuffling) dealt?.filter { it < count }?.takeIf { it.isNotEmpty() } else null)
                            ?: (first until minOf(first + perPage, count)).toList()
                        // A turn, either way: while shuffling, a page of random cubes, none of them the
                        // ones showing when there are enough; otherwise the next page in order.
                        val turn = { by: Int ->
                            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.PAGE)
                            if (shuffling) {
                                val showing = onPage.toHashSet()
                                val fresh = (0 until count).filterNot { it in showing }
                                dealt = (if (fresh.size >= perPage) fresh else (0 until count).toList()).shuffled().take(perPage)
                            } else {
                                dealt = null
                                page = current + by
                            }
                        }
                        if (count == 0) {
                            Text(
                                if (filter == FILTER_POPULAR && query.isEmpty()) str(
                                    when {
                                        popular == null && !popularAsked -> "textures.online.loading"
                                        popular == null -> "textures.browser.popularOffline"
                                        else -> "textures.browser.popularNone"
                                    },
                                ) else str("textures.browser.none"),
                                color = Color.White.copy(alpha = 0.75f),
                                textAlign = TextAlign.Center,
                                modifier = Modifier.align(Alignment.Center).padding(24.dp),
                            )
                        } else {
                            Column(verticalArrangement = Arrangement.spacedBy(GAP)) {
                                for (r in 0 until rows) Row(horizontalArrangement = Arrangement.spacedBy(GAP)) {
                                    for (c in 0 until cols) {
                                        val slot = r * cols + c
                                        val index = onPage.getOrNull(slot) ?: break
                                        // At the edge of a page, left and right turn it, and the selection
                                        // lands on the other edge of the new one.
                                        val left: (() -> Unit)? = if (c == 0 && (shuffling || current > 0)) {
                                            {
                                                turn(-1)
                                                SettingsControllerNav.selectById("$LAYER.tile.${r * cols + cols - 1}")
                                            }
                                        } else null
                                        val right: (() -> Unit)? = if (c == cols - 1 && (shuffling || current < pages - 1)) {
                                            {
                                                turn(1)
                                                val last = if (shuffling) perPage - 1
                                                else (count - (current + 1) * perPage).coerceAtMost(perPage) - 1
                                                SettingsControllerNav.selectById("$LAYER.tile.${minOf(r * cols, last)}")
                                            }
                                        } else null
                                        // One composition per slot, kept across page turns and downloads:
                                        // rebuilding a cube would unregister its slot's id, and the
                                        // registry then moved the selection somewhere else entirely.
                                        val id = "$LAYER.tile.$slot"
                                        if (packItems != null) {
                                            val pack = packItems[index]
                                            PackCube(
                                                pack = pack,
                                                links = links[pack.id],
                                                unknownName = unknownName,
                                                action = TexturePackInstallState.actionFor(installed[pack.id], pack),
                                                downloading = busyId == pack.id,
                                                fraction = fraction,
                                                coverSerial = pack.installSerialFor(contextSerial, ownedSerials),
                                                id = id, width = tileW, height = tileH,
                                                onOpen = { openPackId = pack.id },
                                                onLeft = left, onRight = right,
                                            )
                                        } else {
                                            val group = groups[index]
                                            CreatorCube(
                                                group = group,
                                                name = group.name.ifEmpty { unknownName },
                                                subtitle = packCount(group.packs.size),
                                                id = id, width = tileW, height = tileH,
                                                onOpen = { openCreator(group.key) },
                                                onLeft = left, onRight = right,
                                            )
                                        }
                                    }
                                }
                            }
                        }
                        Footer(
                            modifier = Modifier.align(Alignment.BottomCenter),
                            selected = SettingsControllerNav.currentSelectedId()?.removePrefix("$LAYER.tile.")?.toIntOrNull()
                                ?.let { onPage.getOrNull(it) }?.let { i ->
                                    if (packItems != null) {
                                        val p = packItems[i]
                                        listOf(p.name, creatorOf(p, links[p.id]).ifEmpty { unknownName }, packMb(p.sizeBytes))
                                            .joinToString("  ·  ")
                                    } else {
                                        val g = groups[i]
                                        g.name.ifEmpty { unknownName } + "  ·  " + packCount(g.packs.size)
                                    }
                                } ?: "",
                            current = current,
                            pages = pages,
                            shuffling = shuffling,
                            onTurn = turn,
                        )
                    }
                }
            }
            // Close, for touch; Back does the same from a controller.
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .padding(12.dp)
                    .size(40.dp)
                    .clip(CircleShape)
                    .background(Color.Black.copy(alpha = 0.35f))
                    .clickable(onClick = onClose),
                contentAlignment = Alignment.Center,
            ) { Text("×", color = Color.White, fontSize = 24.sp) }
        }
    }

    val openPack = openPackId?.let { id -> packs.firstOrNull { it.id == id } }
    if (openPack != null) {
        val packLinks = links[openPack.id]
        PackWindow(
            pack = openPack,
            links = packLinks,
            creator = creatorOf(openPack, packLinks),
            sizeText = "${packMb(openPack.sizeBytes)} · " +
                str("textures.pack.files").replace("%d", openPack.fileCount.toString()),
            action = TexturePackInstallState.actionFor(installed[openPack.id], openPack),
            anyBusy = busyId != null,
            openUrl = uriHandler::openUri,
            onDownload = {
                openPackId = null
                TexturePackDownloads.start(context, openPack, openPack.installSerialFor(contextSerial, ownedSerials))
            },
            onClose = { openPackId = null },
        )
    }
}

/** What the browser is showing, how many there are, whose they are; and a download in progress. */
@Composable
private fun Header(title: String, subtitle: String, avatar: CreatorGroup?) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        if (avatar != null) {
            CreatorAvatar(avatar.name, avatar.avatar, 40.dp)
            Spacer(Modifier.width(12.dp))
        }
        Column(Modifier.weight(1f)) {
            Text(title, color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            Text(subtitle, color = Color.White.copy(alpha = 0.62f), fontSize = 11.sp, maxLines = 2,
                overflow = TextOverflow.Ellipsis)
        }
        DownloadStatus(Modifier.widthIn(max = 300.dp).padding(start = 12.dp))
        Spacer(Modifier.width(52.dp)) // room for Close
    }
}

/** The download going on, with Cancel until it commits; or how the last one ended. */
@Composable
internal fun DownloadStatus(modifier: Modifier = Modifier, dark: Boolean = true) {
    val busy = TexturePackDownloads.busyPackId.value != null
    val text = if (busy) TexturePackDownloads.progressText.value else TexturePackDownloads.status.value
    if (!busy && text.isEmpty()) return
    val ink = if (dark) Color.White else androidx.compose.material3.MaterialTheme.colorScheme.onSurface
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f, fill = false)) {
            if (busy) {
                LinearProgressIndicator(
                    progress = { TexturePackDownloads.progressFraction.floatValue.coerceIn(0f, 1f) },
                    modifier = Modifier.width(180.dp),
                )
                Spacer(Modifier.height(4.dp))
            }
            Text(text, color = ink.copy(alpha = 0.8f), fontSize = 11.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        if (busy && !TexturePackDownloads.commitStarted.value) {
            Spacer(Modifier.width(8.dp))
            BrowserChip(str("action.cancel"), "$LAYER.cancel", dark = dark) { TexturePackDownloads.cancel() }
        }
    }
}

/** Sort, filters and search: one row where there is room, the search on a row of its own on a phone. */
@Composable
private fun Controls(
    narrow: Boolean,
    viewingCreator: Boolean,
    sortMode: Int,
    filter: Int,
    query: String,
    onBack: () -> Unit,
    onSort: (Int) -> Unit,
    onFilter: (Int) -> Unit,
    onQuery: (String) -> Unit,
    shuffleOn: Boolean,
    onShuffle: () -> Unit,
) {
    val chips = @Composable {
        if (viewingCreator) {
            BrowserChip("‹ " + str("textures.creators.all"), "$LAYER.back", selected = true, onClick = onBack)
            Spacer(Modifier.width(12.dp))
        }
        for ((mode, label) in listOf(
            SORT_GAME to str("textures.online.sort.game"),
            SORT_SERIAL to str("textures.online.sort.serial"),
            SORT_CREATOR to str("textures.online.sort.creator"),
        )) {
            BrowserChip(label, "$LAYER.sort.$mode", selected = sortMode == mode) { onSort(mode) }
            Spacer(Modifier.width(6.dp))
        }
        Spacer(Modifier.width(10.dp))
        for ((f, label) in listOf(
            FILTER_POPULAR to str("textures.browser.popular"),
            FILTER_ALL to str("textures.browser.all"),
            FILTER_MINE to str("textures.browser.mine"),
            FILTER_INSTALLED to str("textures.browser.installed"),
        )) {
            BrowserChip(label, "$LAYER.filter.$f", selected = filter == f, outlined = true) { onFilter(f) }
            Spacer(Modifier.width(6.dp))
        }
        if (filter == FILTER_ALL) {
            BrowserChip(
                str("textures.browser.shuffle"), "$LAYER.shuffle", selected = shuffleOn,
                icon = com.armsx2.R.drawable.ic_shuffle, onClick = onShuffle,
            )
            Spacer(Modifier.width(6.dp))
        }
    }
    val hint = str("textures.online.search")
    val search = @Composable { mod: Modifier ->
        val open = { LibraryKeyboard.open(query, onQuery, I18n.get("textures.online.search")) }
        Box(
            mod
                .height(38.dp)
                .controllerFocusable("$LAYER.search", shape = RoundedCornerShape(19.dp), onConfirm = open)
                .clip(RoundedCornerShape(19.dp))
                .background(Color.White.copy(alpha = 0.1f))
                .clickable(onClick = open)
                .padding(horizontal = 16.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(
                "⌕  " + query.ifEmpty { hint },
                color = Color.White.copy(alpha = if (query.isEmpty()) 0.5f else 0.95f),
                fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
            )
        }
    }
    if (narrow) {
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
            chips()
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            search(Modifier.weight(1f))
            if (query.isNotEmpty()) {
                Spacer(Modifier.width(6.dp))
                BrowserChip("×", "$LAYER.clear") { onQuery("") }
            }
        }
    } else {
        Row(verticalAlignment = Alignment.CenterVertically) {
            chips()
            search(Modifier.weight(1f))
            if (query.isNotEmpty()) {
                Spacer(Modifier.width(6.dp))
                BrowserChip("×", "$LAYER.clear") { onQuery("") }
            }
        }
    }
}

/** The selected cube's name and creator, and the page with its arrows. */
@Composable
private fun Footer(modifier: Modifier, selected: String, current: Int, pages: Int, shuffling: Boolean, onTurn: (Int) -> Unit) {
    Row(modifier.fillMaxWidth().height(FOOTER_H), verticalAlignment = Alignment.CenterVertically) {
        Text(
            selected,
            color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        if (pages > 1) {
            BrowserChip("‹", "$LAYER.prev", enabled = shuffling || current > 0) { onTurn(-1) }
            Text(
                if (shuffling) str("textures.browser.shuffle")
                else str("textures.browser.page").replace("%1\$s", "%,d".format(current + 1)).replace("%2\$s", "%,d".format(pages)),
                color = Color.White.copy(alpha = 0.7f), fontSize = 12.sp, modifier = Modifier.padding(horizontal = 8.dp),
            )
            BrowserChip("›", "$LAYER.next", enabled = shuffling || current < pages - 1) { onTurn(1) }
        }
    }
}

/** One pack: its game's cover, its name, its creator with their picture; whether it is installed or
 *  has an update, and how far its download is. */
@Composable
private fun PackCube(
    pack: TextureCatalog.Pack,
    links: TexturePackLinks.Links?,
    unknownName: String,
    action: InstallAction,
    downloading: Boolean,
    fraction: Float,
    coverSerial: String,
    id: String,
    width: Dp,
    height: Dp,
    onOpen: () -> Unit,
    onLeft: (() -> Unit)?,
    onRight: (() -> Unit)?,
) {
    val shape = RoundedCornerShape(16.dp)
    val ring = when {
        downloading -> Color(0xFF4F8EF7)
        action == InstallAction.INSTALLED -> Color(0xFF5BE49B).copy(alpha = 0.85f)
        action != InstallAction.INSTALL -> Color(0xFFFFC857)
        else -> Color.White.copy(alpha = 0.10f)
    }
    Box(
        Modifier
            .size(width, height)
            .controllerFocusable(id, shape, onConfirm = onOpen, onLeft = onLeft, onRight = onRight)
            .clip(shape)
            .background(Color(0xFF161D30))
            .border(1.5.dp, ring, shape)
            .clickable(onClick = onOpen),
    ) {
        // Per pack, not per slot: after a page turn the slot shows another pack, and must not show
        // the last one's cover while the new one loads.
        key(pack.id) {
            SubcomposeAsyncImage(
                model = packCoverUrl(coverSerial),
                contentDescription = pack.gameTitle,
                contentScale = ContentScale.Crop,
                alignment = Alignment.TopCenter,
                modifier = Modifier.fillMaxSize(),
                loading = { GameCoverPlaceholder(pack.gameTitle.ifEmpty { pack.name }, coverSerial) },
                error = { GameCoverPlaceholder(pack.gameTitle.ifEmpty { pack.name }, coverSerial) },
            )
        }
        when (action) {
            InstallAction.INSTALLED -> Badge("✓", Color(0xFF2E9E63))
            InstallAction.UPDATE, InstallAction.CONFLICT -> Badge("↑", Color(0xFFC98A12))
            InstallAction.INSTALL -> Unit
        }
        Column(
            Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .background(Brush.verticalGradient(0f to Color.Transparent, 0.35f to Color.Black.copy(alpha = 0.6f), 1f to Color.Black.copy(alpha = 0.9f)))
                .padding(start = 10.dp, end = 10.dp, top = 26.dp, bottom = 9.dp),
        ) {
            Text(
                pack.name, color = Color.White, fontSize = 12.5.sp, lineHeight = 15.sp, fontWeight = FontWeight.Bold,
                maxLines = 2, overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(5.dp))
            val creator = creatorOf(pack, links)
            Row(verticalAlignment = Alignment.CenterVertically) {
                CreatorAvatar(creator, links?.avatar, 18.dp)
                Spacer(Modifier.width(6.dp))
                Text(
                    creator.ifEmpty { unknownName }, color = Color(0xFFCFE0FF), fontSize = 11.sp,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (downloading) {
            LinearProgressIndicator(
                progress = { fraction.coerceIn(0f, 1f) },
                modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(4.dp),
            )
        }
    }
}

/** One creator: their picture (their initials where there is none), their name, how many packs. */
@Composable
private fun CreatorCube(
    group: CreatorGroup,
    name: String,
    subtitle: String,
    id: String,
    width: Dp,
    height: Dp,
    onOpen: () -> Unit,
    onLeft: (() -> Unit)?,
    onRight: (() -> Unit)?,
) {
    val shape = RoundedCornerShape(16.dp)
    val color = nameColor(group.name.ifEmpty { "?" })
    Box(
        Modifier
            .size(width, height)
            .controllerFocusable(id, shape, onConfirm = onOpen, onLeft = onLeft, onRight = onRight)
            .clip(shape)
            .background(Brush.linearGradient(listOf(lerp(color, Color.White, 0.18f), lerp(color, Color.Black, 0.5f))))
            .border(1.5.dp, color.copy(alpha = 0.9f), shape)
            .clickable(onClick = onOpen),
    ) {
        val initialsSize = with(LocalDensity.current) { (minOf(width, height) * 0.3f).toSp() }
        Text(
            initials(group.name.ifEmpty { "?" }),
            modifier = Modifier.align(Alignment.Center).padding(bottom = 22.dp),
            color = Color.White.copy(alpha = 0.92f), fontSize = initialsSize, fontWeight = FontWeight.Black, maxLines = 1,
        )
        key(group.key) {
            group.avatar?.let { url ->
                AsyncImage(model = avatarRequest(url), contentDescription = name, contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize())
            }
        }
        TileLabel(title = name, subtitle = subtitle)
    }
}

/** A creator's picture in a circle, their initial on their colour where there is none. */
@Composable
internal fun CreatorAvatar(name: String, avatar: String?, size: Dp) {
    Box(
        Modifier.size(size).clip(CircleShape).background(nameColor(name.ifEmpty { "?" })),
        contentAlignment = Alignment.Center,
    ) {
        val letter = with(LocalDensity.current) { (size * 0.5f).toSp() }
        Text(initials(name.ifEmpty { "?" }).take(1), color = Color.White, fontSize = letter, fontWeight = FontWeight.Bold)
        if (avatar != null) {
            AsyncImage(model = avatarRequest(avatar), contentDescription = null, contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize())
        }
    }
}

@Composable
private fun Badge(mark: String, color: Color) {
    Box(Modifier.fillMaxSize().padding(8.dp), contentAlignment = Alignment.TopEnd) {
        Box(Modifier.size(24.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
            Text(mark, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/** A small rounded button: a sort, a filter, a page arrow, Cancel. */
@Composable
private fun BrowserChip(
    label: String,
    id: String,
    selected: Boolean = false,
    outlined: Boolean = false,
    enabled: Boolean = true,
    dark: Boolean = true,
    icon: Int? = null,
    onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(19.dp)
    val act = { if (enabled) onClick() }
    val ink = if (dark) Color.White else androidx.compose.material3.MaterialTheme.colorScheme.onSurface
    Box(
        Modifier
            .height(38.dp)
            .controllerFocusable(id, shape = shape, onConfirm = act)
            .clip(shape)
            .background(
                when {
                    selected && !outlined -> Color(0xFF3D5AFE)
                    selected -> Color(0xFF3D5AFE).copy(alpha = 0.35f)
                    else -> ink.copy(alpha = if (enabled) 0.12f else 0.05f)
                },
            )
            .then(if (selected && outlined) Modifier.border(1.dp, Color(0xFF8C9EFF), shape) else Modifier)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        if (icon != null) {
            androidx.compose.material3.Icon(
                painter = androidx.compose.ui.res.painterResource(icon),
                contentDescription = label,
                tint = ink.copy(alpha = if (enabled) 1f else 0.4f),
                modifier = Modifier.size(18.dp),
            )
        } else {
            Text(label, color = ink.copy(alpha = if (enabled) 1f else 0.4f), fontSize = 13.sp, maxLines = 1)
        }
    }
}

/** The creators of the packs showing, each with their packs: alphabetical, an unknown creator last. */
internal class CreatorGroup(
    /** The first named creator (a pack credited "A, B" is A's); empty for an unknown creator. */
    val key: String,
    val name: String,
    val avatar: String?,
    val packs: List<TextureCatalog.Pack>,
)

internal fun creatorGroups(packs: List<TextureCatalog.Pack>, links: Map<String, TexturePackLinks.Links>): List<CreatorGroup> {
    val byKey = LinkedHashMap<String, MutableList<TextureCatalog.Pack>>()
    for (p in packs) byKey.getOrPut(creatorOf(p, links[p.id]).substringBefore(", ").trim()) { ArrayList() }.add(p)
    return byKey.map { (key, list) ->
        CreatorGroup(key, key, list.firstNotNullOfOrNull { links[it.id]?.avatar }, list.sortedWith(BY_GAME))
    }.sortedWith(compareBy({ it.key.isEmpty() }, { it.key.lowercase() }))
}

/** Who made the pack: the archive's name for them, else the catalog's; empty when no one knows. */
internal fun creatorOf(pack: TextureCatalog.Pack, links: TexturePackLinks.Links?): String = when {
    links?.creator != null -> links.creator
    links?.unknownCreator == true -> ""
    else -> pack.authors.joinToString(", ")
}

/** The game's flat cover, from the same cover set as the library's covers. */
internal fun packCoverUrl(serial: String) = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/$serial.jpg"

// A game's packs together, then by pack name; keys lower-cased so "Zelda" does not sort before
// "ape escape". By serial, a multi-region pack sits under its lowest serial.
private val BY_GAME = compareBy<TextureCatalog.Pack>(
    { it.gameTitle.lowercase() }, { it.serials.minOrNull().orEmpty().lowercase() }, { it.name.lowercase() },
)
private val BY_SERIAL = compareBy<TextureCatalog.Pack>(
    { it.serials.minOrNull().orEmpty().lowercase() }, { it.gameTitle.lowercase() }, { it.name.lowercase() },
)

private const val LAYER = "texture-browser"
private const val SORT_GAME = 0
private const val SORT_SERIAL = 1
private const val SORT_CREATOR = 2
private const val FILTER_ALL = 0
private const val FILTER_MINE = 1
private const val FILTER_INSTALLED = 2
private const val FILTER_POPULAR = 3
/** A cube's height to aim for; the rows that fit decide the real one. */
private val ROW_TARGET = 196.dp
/** Height over width: a little taller than square, for the cover and the name under it. */
private const val ASPECT = 1.1f
private val GAP = 10.dp
private val FOOTER_H = 44.dp
