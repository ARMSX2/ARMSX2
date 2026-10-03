package com.armsx2.ui.textures

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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import com.armsx2.TextureCatalog
import com.armsx2.TexturePackLinks
import com.armsx2.i18n.str
import com.armsx2.ui.common.GameCoverPlaceholder
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.withContext

/**
 * The texture screen's door to the online packs: what the catalog holds, a strip of covers (packs for
 * the player's games first), a download in progress, and Browse, which opens the packs full screen
 * ([TexturePackBrowser]). The catalog loads here, so the browser opens on it at once.
 *
 * The whole catalog is browsable with no game running. Each pack names the serials it belongs to, so
 * the install target comes from the pack itself rather than from whatever happens to be loaded; for a
 * multi-region pack it is the serial of the copy the player has ([TextureCatalog.Pack.installSerialFor]).
 * New packs reach us over Discord and are uploaded by hand, so that is where the submit link goes.
 */
@Composable
fun TextureOnlineSection(
    /** Serial of the game in context, if any. The browser opens on its packs, and a pack covering it
     *  installs there. */
    serial: String?,
    /** Serials present in the user's library, for the browser's My games. */
    librarySerials: Set<String> = emptySet(),
    /** Serials of games the player can boot (the library scan and the game in context), upper-case.
     *  Picks a multi-region pack's install folder. Unlike [librarySerials] it leaves out installed
     *  pack folders, so a pack installed under the wrong region's serial cannot vouch for itself. */
    ownedSerials: Set<String> = emptySet(),
    modifier: Modifier = Modifier,
    onInstalled: () -> Unit,
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current

    var loading by remember { mutableStateOf(true) }
    var failed by remember { mutableStateOf(false) }
    var fromCache by remember { mutableStateOf(false) }
    var packs by remember { mutableStateOf<List<TextureCatalog.Pack>>(emptyList()) }
    var links by remember { mutableStateOf<Map<String, TexturePackLinks.Links>>(emptyMap()) }
    var browserOpen by rememberSaveable { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        loading = true
        // Two files on the same server, asked for side by side.
        val (result, packLinks) = withContext(Dispatchers.IO) {
            val packLinks = async { TexturePackLinks.fetch(context) }
            TextureCatalog.fetch(context) to packLinks.await()
        }
        links = packLinks
        packs = result?.packs.orEmpty()
        fromCache = result?.fromCache == true
        failed = result == null
        loading = false
    }

    // A pack that finished installing, with the browser open or after it closed, refreshes the
    // installed packs on the texture screen.
    val installedCount = TexturePackDownloads.installedCount.intValue
    var seen by remember { mutableIntStateOf(installedCount) }
    LaunchedEffect(installedCount) {
        if (installedCount != seen) {
            seen = installedCount
            onInstalled()
        }
    }

    val open = { if (packs.isNotEmpty()) browserOpen = true }
    // A touch opens it with nothing selected: closing it then hands the controller back nothing,
    // rather than a selection left somewhere down the screen that the page would scroll to.
    val openByTouch = {
        SettingsControllerNav.clearSelection()
        open()
    }
    GlassPanel(modifier) {
        Column {
            SectionTitle(str("textures.online.title"), str("textures.online.subtitle"))
            Spacer(Modifier.height(12.dp))
            when {
                loading -> Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(10.dp))
                    Text(str("textures.online.loading"), style = MaterialTheme.typography.bodyMedium)
                }

                failed -> Text(
                    str("textures.online.failed"),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )

                else -> {
                    CoverStrip(packs, serial, librarySerials, ownedSerials, onClick = openByTouch)
                    Spacer(Modifier.height(8.dp))
                    val creators = remember(packs, links) { creatorGroups(packs, links).count { it.key.isNotEmpty() } }
                    Text(
                        str("textures.browser.counts")
                            .replace("%1\$s", "%,d".format(packs.size))
                            .replace("%2\$s", "%,d".format(creators)),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            DownloadStatus(Modifier.fillMaxWidth().padding(top = 10.dp), dark = false)
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = openByTouch,
                enabled = packs.isNotEmpty(),
                shape = RoundedCornerShape(14.dp),
                modifier = Modifier
                    .fillMaxWidth()
                    .controllerFocusable("textures.online.browse", RoundedCornerShape(14.dp), onConfirm = open),
            ) { Text(str("textures.browser.open"), fontWeight = FontWeight.SemiBold) }
            Spacer(Modifier.height(4.dp))
            // New packs come to us over Discord and are uploaded to the catalog by hand; there is no
            // public repo to send people to, so the link is the server itself.
            val contact = { uriHandler.openUri(com.armsx2.navigation.DiscordUrl) }
            TextButton(
                onClick = contact,
                modifier = Modifier.controllerFocusable("tex.contribute", onConfirm = contact),
            ) { Text(str("textures.online.submitDiscord")) }
            if (fromCache) {
                Spacer(Modifier.height(6.dp))
                Text(
                    str("textures.online.cached"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }

    if (browserOpen && packs.isNotEmpty()) {
        TexturePackBrowser(
            packs = packs,
            links = links,
            contextSerial = serial,
            librarySerials = librarySerials,
            ownedSerials = ownedSerials,
            onClose = { browserOpen = false },
        )
    }
}

/**
 * A row of covers, as many as fit: the player's games' packs first, then a pick of the rest that
 * changes from day to day, one cover per game. A look at what is in there; tapping opens it.
 */
@Composable
private fun CoverStrip(
    packs: List<TextureCatalog.Pack>,
    contextSerial: String?,
    librarySerials: Set<String>,
    ownedSerials: Set<String>,
    onClick: () -> Unit,
) {
    val picks = remember(packs, contextSerial, librarySerials) {
        val mine = packs.filter { p -> p.matchesSerial(contextSerial) || p.serials.any { it.uppercase() in librarySerials } }
        val day = java.time.LocalDate.now().toEpochDay()
        (mine.sortedBy { it.gameTitle.lowercase() } + (packs - mine.toSet()).shuffled(kotlin.random.Random(day)))
            .distinctBy { it.gameTitle.lowercase() }
    }
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val width = 58.dp
        val gap = 8.dp
        val n = ((maxWidth + gap) / (width + gap)).toInt().coerceIn(1, 14)
        Row(horizontalArrangement = Arrangement.spacedBy(gap)) {
            picks.take(n).forEach { pack ->
                val coverSerial = pack.installSerialFor(contextSerial, ownedSerials)
                Box(
                    Modifier
                        .size(width, width * 1.4f)
                        .clip(RoundedCornerShape(10.dp))
                        .clickable(onClick = onClick),
                ) {
                    SubcomposeAsyncImage(
                        model = packCoverUrl(coverSerial),
                        contentDescription = pack.gameTitle,
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize(),
                        loading = { GameCoverPlaceholder(pack.gameTitle.ifEmpty { pack.name }, coverSerial) },
                        error = { GameCoverPlaceholder(pack.gameTitle.ifEmpty { pack.name }, coverSerial) },
                    )
                }
            }
        }
    }
}
