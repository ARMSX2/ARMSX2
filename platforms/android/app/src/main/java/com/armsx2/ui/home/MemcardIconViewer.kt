package com.armsx2.ui.home

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.Ps2IconRenderer
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Whether the Icon Museum is open; set from the library's overflow menu. */
internal object MemcardIconViewerState {
    val open = mutableStateOf(false)
    /** The "MC Icon Info" note. */
    val info = mutableStateOf(false)
    /** The screensaver's settings. */
    val screensaver = mutableStateOf(false)
    /** The Online Icons download prompt. */
    val online = mutableStateOf(false)
}

/** The Icon Museum's bookmark: the icon it opens at, by [MemcardCovers.ShowIcon.key]. With the
 *  online set there are thousands, and nobody should have to page back to where they were. */
internal object MuseumBookmark {
    private const val KEY = "library.iconMuseum.bookmark"
    val key = mutableStateOf<String?>(null)
    private var loaded = false

    fun load() {
        if (loaded) return
        loaded = true
        key.value = runCatching { MainActivityRuntime.prefs.getString(KEY, null) }.getOrNull()
    }

    /** Bookmarks [k], or takes the bookmark off it when it is the one bookmarked. */
    fun toggle(k: String) {
        val next = if (key.value == k) null else k
        runCatching {
            MainActivityRuntime.prefs.edit().apply { if (next == null) remove(KEY) else putString(KEY, next) }.apply()
        }
        key.value = next
    }
}

/** How Memory Card Covers works, from the library menu's "MC Icon Info". */
@Composable
internal fun MemcardCoversInfo(onClose: () -> Unit) {
    com.armsx2.ui.common.PadModal(key = "memcard-covers-info", onDismiss = onClose, initialFocusId = "memcard-covers-info.ok") {
        androidx.compose.material3.Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 460.dp),
            shape = RoundedCornerShape(20.dp),
            color = androidx.compose.material3.MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier.padding(22.dp).heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.8f).dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    str("memcard.info.title"),
                    style = androidx.compose.material3.MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    str("memcard.info.body"),
                    style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
                    color = androidx.compose.material3.MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterEnd) {
                    androidx.compose.material3.TextButton(
                        onClick = onClose,
                        modifier = Modifier.controllerFocusable(controllerId = "memcard-covers-info.ok", onConfirm = onClose),
                    ) { Text(str("action.ok")) }
                }
            }
        }
    }
}

/**
 * Icon Museum: every save icon on the player's memory cards, then the ones found on discs and,
 * once downloaded, every icon of the online set, one at a time, full screen and moving, each on
 * its own icon.sys background with its title. Like ARMSX3's theme preview, it is for looking at
 * them without the library in the way. Swipe or press left and right to go through them; the
 * bookmark button (A on a controller) marks one to open at next time; Back closes it.
 */
@Composable
internal fun MemcardIconViewer(onClose: () -> Unit, titles: () -> Map<String, String> = { emptyMap() }) {
    val context = LocalContext.current
    val saves by produceState<List<MemcardCovers.ShowIcon>?>(null) {
        value = withContext(Dispatchers.IO) { MemcardCovers.viewerIcons(context, titles()) }
    }
    com.armsx2.ui.common.PadModal(
        key = "memcard-icon-viewer",
        onDismiss = onClose,
        scrimAlpha = 1f,
        initialFocusId = "memcard-icon-viewer",
    ) {
        Box(Modifier.fillMaxSize().background(Color(0xFF0A0F1E))) {
            val list = saves
            when {
                list == null -> Text(
                    str("memcard.viewer.loading"),
                    color = Color.White.copy(alpha = 0.8f),
                    modifier = Modifier.align(Alignment.Center),
                )
                list.isEmpty() -> Text(
                    str("memcard.viewer.empty"),
                    color = Color.White.copy(alpha = 0.8f),
                    textAlign = TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center).padding(32.dp),
                )
                else -> ViewerPages(list)
            }
            // Close, for touch; Back does the same from a controller.
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .padding(16.dp)
                    .size(44.dp)
                    .clip(CircleShape)
                    .background(Color.Black.copy(alpha = 0.35f))
                    .clickable(onClick = onClose),
                contentAlignment = Alignment.Center,
            ) {
                Text("×", color = Color.White, fontSize = 26.sp)
            }
        }
    }
}

@Composable
private fun ViewerPages(saves: List<MemcardCovers.ShowIcon>) {
    remember { MuseumBookmark.load() }
    // Opens at the bookmark, when it is still in the list.
    val start = remember(saves) { saves.indexOfFirst { it.key == MuseumBookmark.key.value }.coerceAtLeast(0) }
    val pager = rememberPagerState(initialPage = start, pageCount = { saves.size })
    val fromDisc = str("memcard.viewer.fromDisc")
    val onlineBy = str("memcard.viewer.onlineBy")
    val online = str("memcard.viewer.online")
    val bookmarked = str("memcard.viewer.bookmarked")
    val scope = rememberCoroutineScope()
    Box(Modifier.fillMaxSize()) {
        HorizontalPager(state = pager, modifier = Modifier.fillMaxSize()) { page ->
            val ref = saves[page]
            ViewerPage(ref, animate = page == pager.settledPage)
        }
        // Title, serial and card, and where in the list this is. Also the controller's handle on
        // the viewer: left and right move through the saves, and A bookmarks the one shown.
        val ref = saves[pager.currentPage.coerceIn(saves.indices)]
        val isBookmarked = MuseumBookmark.key.value == ref.key
        val origin = when (val o = ref.origin) {
            is MemcardCovers.FromCard -> o.card
            MemcardCovers.FromDisc -> fromDisc
            is MemcardCovers.FromOnline -> listOfNotNull(
                o.label.ifBlank { null },
                if (o.contributors.isBlank()) online else onlineBy.replace("%s", o.contributors),
            ).joinToString("  ·  ")
        }
        // Bookmark, for touch, across from Close; A on the panel does the same from a controller.
        Box(
            Modifier
                .align(Alignment.TopStart)
                .padding(16.dp)
                .size(44.dp)
                .clip(CircleShape)
                .background(Color.Black.copy(alpha = 0.35f))
                .clickable { MuseumBookmark.toggle(ref.key) },
            contentAlignment = Alignment.Center,
        ) {
            androidx.compose.material3.Icon(
                painter = androidx.compose.ui.res.painterResource(
                    if (isBookmarked) com.armsx2.R.drawable.ic_bookmark_filled else com.armsx2.R.drawable.ic_bookmark,
                ),
                contentDescription = str("memcard.viewer.bookmark"),
                tint = Color.White,
                modifier = Modifier.size(22.dp),
            )
        }
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 28.dp, start = 24.dp, end = 24.dp)
                .clip(RoundedCornerShape(18.dp))
                .background(Color.Black.copy(alpha = 0.38f))
                .controllerFocusable(
                    controllerId = "memcard-icon-viewer",
                    shape = RoundedCornerShape(18.dp),
                    onLeft = { scope.launch { pager.animateScrollToPage((pager.currentPage - 1).coerceAtLeast(0)) } },
                    onRight = { scope.launch { pager.animateScrollToPage((pager.currentPage + 1).coerceAtMost(saves.size - 1)) } },
                    onConfirm = { MuseumBookmark.toggle(saves[pager.currentPage.coerceIn(saves.indices)].key) },
                )
                .padding(horizontal = 22.dp, vertical = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                ref.title,
                color = Color.White,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
            Text(
                listOfNotNull(ref.serial, origin).joinToString("  ·  "),
                color = Color.White.copy(alpha = 0.72f),
                fontSize = 13.sp,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
            Text(
                listOfNotNull("%,d / %,d".format(pager.currentPage + 1, saves.size), bookmarked.takeIf { isBookmarked })
                    .joinToString("  ·  "),
                color = Color.White.copy(alpha = 0.55f),
                fontSize = 12.sp,
            )
        }
    }
}

@Composable
private fun ViewerPage(ref: MemcardCovers.ShowIcon, animate: Boolean) {
    val loaded by produceState<MemcardCovers.Loaded?>(null, ref) {
        value = withContext(Dispatchers.IO) { runCatching { ref.load() }.getOrNull() }
    }
    // The save's own background gradient, drawn once small and stretched: it is a smooth blend of
    // four corners, so nothing is lost.
    val background by produceState<ImageBitmap?>(null, loaded) {
        val sys = loaded?.sys ?: return@produceState
        value = withContext(Dispatchers.Default) {
            val px = Ps2IconRenderer.backgroundPixels(sys, 32, 32)
            Bitmap.createBitmap(px, 32, 32, Bitmap.Config.ARGB_8888).asImageBitmap()
        }
    }
    Box(Modifier.fillMaxSize()) {
        background?.let {
            Image(it, contentDescription = null, contentScale = ContentScale.FillBounds, modifier = Modifier.fillMaxSize())
        }
        if (loaded != null) {
            AnimatedPs2Icon(
                key = ref.key,
                load = { loaded },
                options = Ps2IconRenderer.Options(background = false, fill = 0.9f),
                aspect = 1f,
                maxWidth = 560,
                animate = animate,
                modifier = Modifier
                    .align(Alignment.Center)
                    .fillMaxWidth(0.8f)
                    .fillMaxSize(0.72f)
                    .padding(bottom = 40.dp),
            )
        }
    }
}
