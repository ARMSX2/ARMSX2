package com.armsx2.ui.textures

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.armsx2.TextureCatalog
import com.armsx2.TexturePackInstallState
import com.armsx2.TexturePackInstaller
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.SearchField
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.controllerFocusable

/**
 * Browse and install texture packs from the shared online catalog (hosted by sashkinbro, used with
 * his approval).
 *
 * The whole catalog is browsable with no game running. Each pack names the serials it belongs to, so
 * the install target comes from the pack itself rather than from whatever happens to be loaded —
 * requiring a running game was a restriction the data never justified. Packs matching something in
 * your library are listed first; the rest stay visible so you can grab them before you own the disc.
 */
/** The catalog repo: where the packs live and where contributions go. */
private const val CATALOG_REPO_URL = "https://github.com/sashkinbro/EmuCoreX-Textures"

@Composable
fun TextureOnlineSection(
    /** Serial of the game in context, if any. Only affects ordering — never what gets installed. */
    serial: String?,
    /** Serials present in the user's library, so owned games float to the top. */
    librarySerials: Set<String> = emptySet(),
    modifier: Modifier = Modifier,
    onInstalled: () -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = androidx.compose.runtime.rememberCoroutineScope()
    val uriHandler = LocalUriHandler.current

    var loading by remember { mutableStateOf(true) }
    var failed by remember { mutableStateOf(false) }
    var fromCache by remember { mutableStateOf(false) }
    var packs by remember { mutableStateOf<List<TextureCatalog.Pack>>(emptyList()) }
    var busyPackId by remember { mutableStateOf<String?>(null) }
    var progressText by remember { mutableStateOf("") }
    var progressFraction by remember { mutableStateOf(0f) }
    var cancelRequested by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }
    // The catalog is 113 packs and growing, so it folds away once someone has what they came for.
    // Saveable, so it survives rotation and does not spring back open.
    var listOpen by androidx.compose.runtime.saveable.rememberSaveable { mutableStateOf(true) }
    var query by androidx.compose.runtime.saveable.rememberSaveable { mutableStateOf("") }

    val installRevision = TexturePackInstallState.revision.value
    val installed = remember(installRevision) { TexturePackInstallState.all() }

    LaunchedEffect(Unit) {
        loading = true
        val result = withContext(Dispatchers.IO) {
            TextureCatalog.fetch(context)
        }
        packs = result?.packs.orEmpty()
        fromCache = result?.fromCache == true
        failed = result == null
        loading = false
    }

    GlassPanel(modifier) {
        Column {
            val toggle = { listOpen = !listOpen }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .controllerFocusable("textures.online.toggle", onConfirm = toggle)
                    .clickable(onClick = toggle),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                SectionTitle(
                    str("textures.online.title"),
                    str("textures.online.subtitle"),
                    Modifier.weight(1f),
                )
                if (packs.isNotEmpty()) {
                    Text(
                        packs.size.toString(),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(8.dp))
                }
                Text(
                    if (listOpen) "\u25be" else "\u25b8",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            Spacer(Modifier.height(8.dp))

            if (!listOpen) {
                // collapsed: header only
            } else when {
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
                    // Search across everything a person might type: pack name, game title, serial
                    // and author. Uses the in-app keyboard like the library search, so it works on a
                    // handheld with no touchscreen keyboard and honours the system-IME preference.
                    val open = { LibraryKeyboard.open(query, { query = it }, I18n.get("textures.online.search")) }
                    SearchField(
                        value = query,
                        onClick = open,
                        placeholder = str("textures.online.search"),
                        modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
                    )
                    val needle = query.trim().lowercase()
                    val packs = if (needle.isEmpty()) packs else packs.filter { p ->
                        p.name.lowercase().contains(needle) ||
                            p.gameTitle.lowercase().contains(needle) ||
                            p.serials.any { it.lowercase().contains(needle) } ||
                            p.authors.any { it.lowercase().contains(needle) }
                    }

                    // Ordering only: the game in context first, then anything in the library, then
                    // the rest of the catalog. Nothing is hidden — a pack you cannot use today is
                    // still a pack you may want tomorrow.
                    val (owned, others) = packs.partition { p ->
                        p.matchesSerial(serial) || p.serials.any { librarySerials.contains(it.uppercase()) }
                    }
                    val ordered = owned.sortedByDescending { it.matchesSerial(serial) }

                    val startInstall: (TextureCatalog.Pack, String) -> Unit = { pack, targetSerial ->
                        busyPackId = pack.id
                        cancelRequested = false
                        status = ""
                        progressFraction = 0f
                        progressText = I18n.get("textures.online.starting")
                        scope.launch {
                            val outcome = withContext(Dispatchers.IO) {
                                TexturePackInstaller.install(
                                    context, pack, targetSerial,
                                    onProgress = { p ->
                                        when (p) {
                                            is TexturePackInstaller.Progress.Downloading -> {
                                                progressFraction =
                                                    if (p.total > 0) (p.read.toFloat() / p.total) else 0f
                                                progressText = I18n.get("textures.online.downloading")
                                                    .replace("%1s", mb(p.read)).replace("%2s", mb(p.total))
                                            }
                                            TexturePackInstaller.Progress.Verifying ->
                                                progressText = I18n.get("textures.online.verifying")
                                            is TexturePackInstaller.Progress.Extracting -> {
                                                progressFraction =
                                                    if (p.total > 0) (p.done.toFloat() / p.total) else 0f
                                                progressText = I18n.get("textures.online.extracting")
                                                    .replace("%1s", p.done.toString())
                                                    .replace("%2s", p.total.toString())
                                            }
                                            TexturePackInstaller.Progress.Installing ->
                                                progressText = I18n.get("textures.online.installing")
                                        }
                                    },
                                    isCancelled = { cancelRequested },
                                )
                            }
                            busyPackId = null
                            progressText = ""
                            status = when {
                                outcome.ok -> I18n.get("textures.online.done")
                                outcome.error == null -> I18n.get("textures.online.cancelled")
                                else -> outcome.error
                            }
                            if (outcome.ok) onInstalled()
                        }
                    }

                    if (ordered.isEmpty() && others.isEmpty()) {
                        Text(
                            str("textures.online.empty"),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }

                    ordered.forEach { pack ->
                        PackRow(pack, installed[pack.id], busyPackId, progressText,
                            progressFraction, uriHandler::openUri,
                            // Install target is the pack's own serial, so this works with no game
                            // running and cannot drop a pack into the wrong game's folder.
                            onGet = { startInstall(pack, pack.serials.first()) },
                            onCancel = { cancelRequested = true })
                    }

                    if (others.isNotEmpty()) {
                        Spacer(Modifier.height(10.dp))
                        Text(
                            str("textures.online.otherGames"),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Spacer(Modifier.height(4.dp))
                        others.forEach { pack ->
                            PackRow(pack, installed[pack.id], busyPackId, progressText,
                                progressFraction, uriHandler::openUri,
                                onGet = { startInstall(pack, pack.serials.first()) },
                                onCancel = { cancelRequested = true })
                        }
                    }

                    if (status.isNotEmpty()) {
                        Spacer(Modifier.height(8.dp))
                        Text(status, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Spacer(Modifier.height(8.dp))
                    // Where packs come from, and where to send new ones. People kept asking; the
                    // catalog is a public repo with a CONTRIBUTING guide, so point straight at it.
                    val contribute = { uriHandler.openUri(CATALOG_REPO_URL) }
                    TextButton(
                        onClick = contribute,
                        modifier = Modifier.controllerFocusable("tex.contribute", onConfirm = contribute),
                    ) { Text(str("textures.online.contribute")) }

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
        }
    }
}

@Composable
private fun PackRow(
    pack: TextureCatalog.Pack,
    installed: TexturePackInstallState.Installed?,
    busyPackId: String?,
    progressText: String,
    progressFraction: Float,
    openUrl: (String) -> Unit,
    onGet: () -> Unit,
    onCancel: () -> Unit,
) {
    val busy = busyPackId == pack.id
    val anyBusy = busyPackId != null
    val upToDate = installed != null && installed.version == pack.version

    Surface(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.40f)),
    ) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(pack.name, style = MaterialTheme.typography.titleSmall,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(
                        "${pack.serials.joinToString(", ")} · ${mb(pack.sizeBytes)} · ${pack.fileCount} files",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1, overflow = TextOverflow.Ellipsis,
                    )
                    if (pack.authors.isNotEmpty()) {
                        Text(
                            str("textures.online.by").replace("%s", pack.authors.joinToString(", ")),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1, overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Spacer(Modifier.width(8.dp))
                if (busy) {
                    val cancel = onCancel
                    TextButton(
                        onClick = cancel,
                        modifier = Modifier.controllerFocusable("tex.cancel.${pack.id}", onConfirm = cancel),
                    ) { Text(str("action.cancel")) }
                } else {
                    val label = when {
                        upToDate -> str("textures.online.installed")
                        installed != null -> str("textures.online.update")
                        else -> str("textures.online.get")
                    }
                    Button(
                        onClick = onGet,
                        enabled = !anyBusy && !upToDate,
                        shape = RoundedCornerShape(14.dp),
                        modifier = Modifier.controllerFocusable(
                            "tex.get.${pack.id}", RoundedCornerShape(14.dp), onConfirm = onGet,
                        ),
                    ) { Text(label) }
                }
            }
            if (busy) {
                Spacer(Modifier.height(6.dp))
                LinearProgressIndicator(
                    progress = { progressFraction.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(4.dp))
                Text(progressText, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (pack.sourceUrl.isNotEmpty() && !busy) {
                val open = { openUrl(pack.sourceUrl) }
                TextButton(
                    onClick = open,
                    modifier = Modifier.controllerFocusable("tex.src.${pack.id}", onConfirm = open),
                ) {
                    Text(str("textures.online.source"), fontSize = 12.sp, fontWeight = FontWeight.Normal)
                }
            }
        }
    }
}

private fun mb(bytes: Long): String = when {
    bytes >= 1024L * 1024 * 1024 -> String.format(java.util.Locale.US, "%.1f GB", bytes / 1024.0 / 1024 / 1024)
    else -> "${bytes / 1024 / 1024} MB"
}
