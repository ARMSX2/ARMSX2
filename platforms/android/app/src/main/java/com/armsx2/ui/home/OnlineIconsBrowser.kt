package com.armsx2.ui.home

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.OnlineIcons
import com.armsx2.memcard.Ps2Icon
import com.armsx2.memcard.Ps2IconRenderer
import com.armsx2.memcard.Ps2IconSys
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Online Icons, from the library menu: the PS2 Icon Open Database's save icons, to look through
 * with previews and download one at a time, the ones for the player's games, or all of them, and
 * to remove any again. A paged grid rather than a lazy list: the controller moves between the
 * tiles that are composed, and left or right at the edge of a page turns it. Search matches
 * titles, which save it is, contributors and disc serials. With Shuffle on, All and Downloaded
 * deal a page of random icons at every turn. Downloads go on if it is closed.
 */
@Composable
internal fun OnlineIconsBrowser(onClose: () -> Unit, librarySerials: () -> Collection<String>) {
    val context = androidx.compose.ui.platform.LocalContext.current
    // Store music while it is open, in the library music's place, and friends see "Browsing
    // Online Icons".
    androidx.compose.runtime.DisposableEffect(Unit) {
        com.armsx2.LibraryMusic.playTheme(context, com.armsx2.R.raw.online_icons_music)
        com.armsx2.DiscordPresence.enter(com.armsx2.DiscordPresence.PLACE_ONLINE_ICONS)
        onDispose {
            com.armsx2.LibraryMusic.endTheme(context, com.armsx2.R.raw.online_icons_music)
            com.armsx2.DiscordPresence.leave(com.armsx2.DiscordPresence.PLACE_ONLINE_ICONS)
        }
    }
    val generation = OnlineIcons.generation.intValue
    val status = OnlineIcons.status.value
    // The catalog: fetched when it changed (a quick check when it didn't); null while asking.
    var lists by remember { mutableStateOf<Boolean?>(null) }
    LaunchedEffect(Unit) { lists = OnlineIcons.syncLists() }
    var setSize by remember { mutableStateOf<Long?>(null) }
    LaunchedEffect(Unit) { setSize = OnlineIcons.setSize() }
    val catalog = remember(generation, lists) { OnlineIcons.catalog() }
    val installed = remember(generation) { OnlineIcons.installedHashes() }
    val onDevice = remember(generation) { OnlineIcons.installedBytes() }
    val mine = remember(generation, lists) { librarySerials().mapNotNullTo(HashSet()) { OnlineIcons.hashFor(it) } }
    val missingMine = mine - installed

    // Popular Today, the tab it opens on: asked of the counter every time the browser opens.
    var popularAsked by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        OnlineIcons.refreshPopular()
        popularAsked = true
    }
    val popular = OnlineIcons.popular.value

    var filter by remember { mutableIntStateOf(FILTER_POPULAR) }
    var query by remember { mutableStateOf("") }
    // Shuffle, on All and Downloaded, as in the Icon Museum: turning it on leaves the page as it
    // is, and from then on every turn, either way, deals a page of random icons from the tab
    // ([dealt]) instead of the next in order. Off, the tab is back in order from there.
    var shuffleOn by remember { mutableStateOf(false) }
    var dealt by remember { mutableStateOf<List<OnlineIcons.Entry>?>(null) }
    var perPageNow by remember { mutableIntStateOf(1) }
    val shown = remember(catalog, installed, mine, filter, query, popular) {
        val bySerial = OnlineIcons.hashFor(query.trim().uppercase().replace('_', '-').replace(".", ""))
        val q = query.trim().lowercase()
        val matches = { e: OnlineIcons.Entry ->
            q.isEmpty() || e.hash == bySerial || e.title.lowercase().contains(q) || e.label.lowercase().contains(q) ||
                e.contributors.lowercase().contains(q)
        }
        if (filter == FILTER_POPULAR) {
            // In the counter's order, most downloaded first; one entry per icon, the catalog's first.
            val byHash = catalog.groupBy { it.hash }
            popular.orEmpty().mapNotNull { byHash[it]?.first() }.filter(matches)
        } else {
            catalog.filter { e ->
                (filter == FILTER_ALL || (filter == FILTER_MINE && e.hash in mine) || (filter == FILTER_DOWNLOADED && e.hash in installed)) &&
                    matches(e)
            }
        }
    }
    var page by remember { mutableIntStateOf(0) }
    val shownSet = remember(shown) { shown.toHashSet() }
    LaunchedEffect(filter, query) {
        page = 0
        dealt = null
    }
    // A second press removes: one icon (its hash) or everything (ALL).
    var removing by remember { mutableStateOf<String?>(null) }
    var tapped by remember { mutableStateOf<String?>(null) }
    val previews = remember { PreviewCache() }
    val busy = status is OnlineIcons.Status.Working

    val credit = str("onlineicons.credit")
    val onDeviceFormat = str("onlineicons.onDevice")
    val pageFormat = str("onlineicons.page")
    val progressFormat = str("onlineicons.progress")
    val progressIconsFormat = str("onlineicons.progressIcons")
    val failedFormat = str("onlineicons.failed")
    val searchHint = str("onlineicons.search")
    val byFormat = str("onlineicons.by")
    val shuffleLabel = str("memcard.viewer.shuffle")

    com.armsx2.ui.common.PadModal(key = "online-icons", onDismiss = onClose, scrimAlpha = 1f, initialFocusId = "online-icons.tile.0") {
        Box(Modifier.fillMaxSize().background(Color(0xFF0A0F1E))) {
            Column(Modifier.fillMaxSize().padding(horizontal = 20.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                // What this is, whose icons they are, and how many are here.
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(str("onlineicons.title"), color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)
                        Text(
                            onDeviceFormat.replace("%1\$s", "%,d".format(installed.size)).replace("%2\$s", "%,d".format(catalog.size))
                                .replace("%3\$s", megabytes(onDevice)) + "  ·  " + credit,
                            color = Color.White.copy(alpha = 0.62f), fontSize = 11.sp, maxLines = 2, overflow = TextOverflow.Ellipsis,
                        )
                    }
                    Spacer(Modifier.width(52.dp)) // room for Close
                }

                // What can be downloaded or removed, or how far a download is.
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    when {
                        status is OnlineIcons.Status.Working -> {
                            val fraction = if (status.total > 0) (status.done.toFloat() / status.total).coerceIn(0f, 1f) else null
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                if (fraction != null) {
                                    LinearProgressIndicator(progress = { fraction }, modifier = Modifier.fillMaxWidth())
                                } else {
                                    LinearProgressIndicator(Modifier.fillMaxWidth())
                                }
                                Row {
                                    Text(fraction?.let { "${(it * 100).toInt()}%" } ?: "", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold)
                                    Spacer(Modifier.weight(1f))
                                    Text(
                                        if (status.total <= 0) megabytes(status.done)
                                        else if (status.bytes) progressFormat.replace("%1\$s", megabytes(status.done)).replace("%2\$s", megabytes(status.total))
                                        else progressIconsFormat.replace("%1\$s", "%,d".format(status.done)).replace("%2\$s", "%,d".format(status.total)),
                                        color = Color.White.copy(alpha = 0.7f), fontSize = 12.sp,
                                    )
                                }
                            }
                            Chip(str("action.cancel"), "online-icons.cancel") { OnlineIcons.cancel() }
                        }
                        else -> {
                            Chip(
                                str("onlineicons.downloadAll") + "  ·  " + (setSize?.let { megabytes(it) } ?: "%,d".format(catalog.size)),
                                "online-icons.all",
                                enabled = catalog.isNotEmpty() && installed.size < catalog.mapTo(HashSet()) { it.hash }.size,
                            ) { removing = null; OnlineIcons.installAll() }
                            if (missingMine.isNotEmpty()) {
                                Chip(str("onlineicons.getMine").replace("%s", "%,d".format(missingMine.size)), "online-icons.mine") {
                                    removing = null
                                    OnlineIcons.install(missingMine)
                                }
                            }
                            if (installed.isNotEmpty()) {
                                Chip(
                                    str(if (removing == ALL) "onlineicons.removeAllConfirm" else "onlineicons.removeAll"),
                                    "online-icons.removeAll", warn = removing == ALL,
                                ) {
                                    if (removing == ALL) { removing = null; OnlineIcons.uninstallAll() } else removing = ALL
                                }
                            }
                            Spacer(Modifier.weight(1f))
                            if (status is OnlineIcons.Status.Failed) {
                                Text(failedFormat.replace("%s", status.why), color = Color(0xFFFF8A80), fontSize = 12.sp, maxLines = 2)
                            }
                        }
                    }
                }

                // Which icons: all, the player's games', the downloaded ones; and search.
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for ((f, label) in listOf(
                        FILTER_POPULAR to str("onlineicons.popular"), FILTER_ALL to str("onlineicons.all"),
                        FILTER_MINE to str("onlineicons.myGames"), FILTER_DOWNLOADED to str("onlineicons.installed"),
                    )) {
                        Chip(label, "online-icons.filter.$f", selected = filter == f) { filter = f }
                    }
                    if (filter == FILTER_ALL || filter == FILTER_DOWNLOADED) {
                        Chip(shuffleLabel, "online-icons.shuffle", selected = shuffleOn, icon = com.armsx2.R.drawable.ic_shuffle) {
                            if (shuffleOn) {
                                // Back in order, at the page the first icon showing sits on.
                                dealt?.firstOrNull()?.let { anchor ->
                                    page = shown.indexOf(anchor).coerceAtLeast(0) / perPageNow.coerceAtLeast(1)
                                }
                                dealt = null
                            }
                            shuffleOn = !shuffleOn
                        }
                    }
                    val openKeyboard = { LibraryKeyboard.open(query, { query = it }, searchHint) }
                    Box(
                        Modifier
                            .weight(1f)
                            .height(38.dp)
                            .clip(RoundedCornerShape(19.dp))
                            .background(Color.White.copy(alpha = 0.1f))
                            .clickable(onClick = openKeyboard)
                            .controllerFocusable("online-icons.search", shape = RoundedCornerShape(19.dp), onConfirm = openKeyboard)
                            .padding(horizontal = 16.dp),
                        contentAlignment = Alignment.CenterStart,
                    ) {
                        Text(
                            query.ifEmpty { searchHint },
                            color = Color.White.copy(alpha = if (query.isEmpty()) 0.5f else 0.95f),
                            fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        )
                    }
                    if (query.isNotEmpty()) Chip("×", "online-icons.clear") { query = "" }
                }

                // The icons, a page at a time.
                BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                    val cols = (maxWidth / TILE_W).toInt().coerceAtLeast(1)
                    val rows = ((maxHeight - FOOTER_H) / TILE_H).toInt().coerceAtLeast(1)
                    val perPage = cols * rows
                    SideEffect { perPageNow = perPage }
                    val pages = ((shown.size + perPage - 1) / perPage).coerceAtLeast(1)
                    // The list can shrink under the page (a filter, a search, a removal).
                    val current = page.coerceAtMost(pages - 1)
                    val first = current * perPage
                    // Shuffling: Shuffle on, on a tab of its own, with more than a page to deal from.
                    val shuffling = shuffleOn && (filter == FILTER_ALL || filter == FILTER_DOWNLOADED) && shown.size > perPage
                    val onPage = (if (shuffling) dealt?.filter { it in shownSet }?.takeIf { it.isNotEmpty() } else null)
                        ?: shown.subList(first.coerceAtMost(shown.size), (first + perPage).coerceAtMost(shown.size))
                    // A turn, either way: while shuffling, a page of random icons from the tab, none of
                    // them the ones showing when there are enough; otherwise the next page in order.
                    val turn = { by: Int ->
                        turnPage()
                        if (shuffling) {
                            val showing = onPage.toHashSet()
                            val fresh = shown.filterNot { it in showing }
                            dealt = (if (fresh.size >= perPage) fresh else shown).shuffled().take(perPage)
                        } else {
                            dealt = null
                            page = current + by
                        }
                    }
                    when {
                        lists == null && catalog.isEmpty() -> Message(str("onlineicons.loading"))
                        catalog.isEmpty() -> Message(str("onlineicons.offline"))
                        shown.isEmpty() && filter == FILTER_POPULAR && query.isEmpty() -> Message(
                            str(
                                when {
                                    popular == null && !popularAsked -> "onlineicons.loading"
                                    popular == null -> "onlineicons.popularOffline"
                                    else -> "onlineicons.popularNone"
                                },
                            ),
                        )
                        shown.isEmpty() -> Message(str("onlineicons.none"))
                        else -> Column(verticalArrangement = Arrangement.spacedBy(TILE_GAP)) {
                            for (r in 0 until rows) Row(horizontalArrangement = Arrangement.spacedBy(TILE_GAP)) {
                                for (c in 0 until cols) {
                                    val slot = r * cols + c
                                    val entry = onPage.getOrNull(slot) ?: break
                                    val have = entry.hash in installed
                                    val press = {
                                        tapped = entry.hash
                                        when {
                                            !have -> { removing = null; OnlineIcons.install(listOf(entry.hash)) }
                                            removing == entry.hash -> { removing = null; OnlineIcons.uninstall(entry.hash) }
                                            else -> removing = entry.hash
                                        }
                                    }
                                    // At the edge of a page, left and right turn it, and the
                                    // selection lands on the other edge of the new one.
                                    val left: (() -> Unit)? = if (c == 0 && (shuffling || current > 0)) {
                                        {
                                            turn(-1)
                                            SettingsControllerNav.selectById("online-icons.tile.${r * cols + cols - 1}")
                                        }
                                    } else null
                                    val right: (() -> Unit)? = if (c == cols - 1 && (shuffling || current < pages - 1)) {
                                        {
                                            turn(1)
                                            val last = if (shuffling) perPage - 1 else (shown.size - (current + 1) * perPage).coerceAtMost(perPage) - 1
                                            SettingsControllerNav.selectById("online-icons.tile.${minOf(r * cols, last)}")
                                        }
                                    } else null
                                    // One composition per slot, kept across page turns: rebuilding a
                                    // tile unregisters its slot's id, and the registry then moved the
                                    // selection to the first control, Download all. What depends on
                                    // the icon inside it is keyed by the icon instead.
                                    Tile(
                                        entry, "online-icons.tile.$slot", have, removing == entry.hash, previews,
                                        selectedByTouch = tapped == entry.hash, onPress = press, onLeft = left, onRight = right,
                                    )
                                }
                            }
                        }
                    }

                    // Page, and the one selected: what it is, which save, who contributed it.
                    val selectedSlot = SettingsControllerNav.currentSelectedId()?.removePrefix("online-icons.tile.")?.toIntOrNull()
                    val focus = selectedSlot?.let { onPage.getOrNull(it) } ?: onPage.firstOrNull { it.hash == tapped }
                    Row(
                        Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(FOOTER_H),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            focus?.let { e ->
                                listOfNotNull(e.title, e.label.ifBlank { null }, e.contributors.ifBlank { null }?.let { byFormat.replace("%s", it) })
                                    .joinToString("  ·  ")
                            } ?: "",
                            color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f),
                        )
                        if (pages > 1) {
                            Chip("‹", "online-icons.prev", enabled = shuffling || current > 0) { turn(-1) }
                            Text(
                                if (shuffling) shuffleLabel
                                else pageFormat.replace("%1\$s", "%,d".format(current + 1)).replace("%2\$s", "%,d".format(pages)),
                                color = Color.White.copy(alpha = 0.7f), fontSize = 12.sp, modifier = Modifier.padding(horizontal = 8.dp),
                            )
                            Chip("›", "online-icons.next", enabled = shuffling || current < pages - 1) { turn(1) }
                        }
                    }
                }
            }
            // Close, for touch; Back does the same from a controller.
            Box(
                Modifier.align(Alignment.TopEnd).padding(12.dp).size(40.dp).clip(CircleShape)
                    .background(Color.Black.copy(alpha = 0.35f)).clickable(onClick = onClose),
                contentAlignment = Alignment.Center,
            ) { Text("×", color = Color.White, fontSize = 24.sp) }
        }
    }
}

/** One icon: its preview, moving while selected, its name, and whether it is downloaded. */
@Composable
private fun Tile(
    entry: OnlineIcons.Entry,
    id: String,
    have: Boolean,
    removing: Boolean,
    previews: PreviewCache,
    selectedByTouch: Boolean,
    onPress: () -> Unit,
    onLeft: (() -> Unit)?,
    onRight: (() -> Unit)?,
) {
    // Per icon, not per slot: after a page turn the slot shows another icon, and must not show the
    // last one's preview while the new one loads.
    var preview by remember(entry.hash) { mutableStateOf(previews.get(entry.hash)) }
    LaunchedEffect(entry.hash) { if (preview == null) preview = previews.load(entry.hash) }
    val selected = SettingsControllerNav.isSelected(id) || selectedByTouch
    val shape = RoundedCornerShape(14.dp)
    Box(
        Modifier
            .size(TILE_W - TILE_GAP, TILE_H - TILE_GAP)
            .clip(shape)
            .background(Color.White.copy(alpha = if (selected) 0.16f else 0.07f))
            .border(2.dp, if (removing) Color(0xFFFF8A80) else if (have) Color(0xFF7CD992).copy(alpha = 0.7f) else Color.Transparent, shape)
            .clickable(onClick = onPress)
            .controllerFocusable(id, shape = shape, onConfirm = onPress, onLeft = onLeft, onRight = onRight),
    ) {
        val p = preview
        Box(Modifier.align(Alignment.TopCenter).padding(top = 6.dp).size(TILE_W - 34.dp)) {
            androidx.compose.runtime.key(entry.hash) {
                when {
                    p == null -> Unit
                    selected -> AnimatedPs2Icon(
                        key = "online-browser:${entry.hash}",
                        load = { p.loaded },
                        options = Ps2IconRenderer.Options(background = false, fill = 0.9f),
                        aspect = 1f,
                        maxWidth = 320,
                        modifier = Modifier.fillMaxSize(),
                    )
                    else -> Image(p.still, contentDescription = null, modifier = Modifier.fillMaxSize())
                }
            }
        }
        Text(
            if (removing) str("onlineicons.removeConfirm") else entry.title,
            color = if (removing) Color(0xFFFF8A80) else Color.White,
            fontSize = 11.sp, lineHeight = 13.sp, maxLines = 2, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center,
            modifier = Modifier.align(Alignment.BottomCenter).padding(horizontal = 6.dp, vertical = 6.dp),
        )
        if (have) {
            Box(
                Modifier.align(Alignment.TopEnd).padding(6.dp).size(18.dp).clip(CircleShape).background(Color(0xFF2E7D4F)),
                contentAlignment = Alignment.Center,
            ) { Text("✓", color = Color.White, fontSize = 11.sp) }
        }
    }
}

/** A tile's preview: the icon, to move while selected, and a still of it drawn once. */
private class Preview(val loaded: MemcardCovers.Loaded, val still: ImageBitmap)

/** Previews already drawn, a few pages' worth, so paging back and forth doesn't draw them again. */
private class PreviewCache {
    private val map = object : LinkedHashMap<String, Preview>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Preview>?) = size > 72
    }

    fun get(hash: String): Preview? = synchronized(map) { map[hash] }

    suspend fun load(hash: String): Preview? {
        val bytes = OnlineIcons.preview(hash) ?: return null
        return withContext(Dispatchers.Default) {
            val sys = Ps2IconSys.parse(bytes) ?: return@withContext null
            val icon = Ps2Icon.parse(bytes.copyOfRange(Ps2IconSys.SIZE, bytes.size)) ?: return@withContext null
            val options = Ps2IconRenderer.Options(background = false, fill = 0.9f)
            val px = Ps2IconRenderer.render(icon, sys, STILL_PX, STILL_PX, options)
            val still = Bitmap.createBitmap(px, STILL_PX, STILL_PX, Bitmap.Config.ARGB_8888).asImageBitmap()
            Preview(MemcardCovers.Loaded(icon, sys, Ps2IconRenderer.choosePose(icon, sys, options)), still)
        }?.also { synchronized(map) { map[hash] = it } }
    }
}

/** A small rounded button: an action, a filter, or a page arrow. With [icon], the picture stands
 *  in for [label], which then only names it for accessibility (Shuffle). */
@Composable
private fun Chip(
    label: String,
    id: String,
    selected: Boolean = false,
    enabled: Boolean = true,
    warn: Boolean = false,
    icon: Int? = null,
    onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(19.dp)
    val act = { if (enabled) onClick() }
    Box(
        Modifier
            .height(38.dp)
            .clip(shape)
            .background(
                when {
                    warn -> Color(0xFF8C2F2F)
                    selected -> Color(0xFF3D5AFE)
                    else -> Color.White.copy(alpha = if (enabled) 0.12f else 0.05f)
                },
            )
            .clickable(enabled = enabled, onClick = onClick)
            .controllerFocusable(id, shape = shape, onConfirm = act)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        if (icon != null) {
            androidx.compose.material3.Icon(
                painter = androidx.compose.ui.res.painterResource(icon),
                contentDescription = label,
                tint = Color.White.copy(alpha = if (enabled) 1f else 0.4f),
                modifier = Modifier.size(18.dp),
            )
        } else {
            Text(label, color = Color.White.copy(alpha = if (enabled) 1f else 0.4f), fontSize = 13.sp, maxLines = 1)
        }
    }
}

@Composable
private fun Message(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, color = Color.White.copy(alpha = 0.75f), textAlign = TextAlign.Center, modifier = Modifier.padding(24.dp))
    }
}

/** Every page turn clicks, from the arrows or from a page's edge, and once: the controller's
 *  confirm adds its own select sound only after actions that made none. */
private fun turnPage() = com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.PAGE)

/** A size the way people say it: "63 MB", "3.2 MB", "180 KB". */
private fun megabytes(bytes: Long): String {
    val mb = bytes / 1_000_000.0
    return when {
        bytes < 1_000_000 -> "%,d KB".format((bytes + 999) / 1000)
        mb < 10 -> "%.1f MB".format(mb)
        else -> "%.0f MB".format(mb)
    }
}

private const val FILTER_POPULAR = 0
private const val FILTER_ALL = 1
private const val FILTER_MINE = 2
private const val FILTER_DOWNLOADED = 3
private const val ALL = "*"
private const val STILL_PX = 192
private val TILE_W = 128.dp
private val TILE_H = 150.dp
private val TILE_GAP = 8.dp
private val FOOTER_H = 40.dp
