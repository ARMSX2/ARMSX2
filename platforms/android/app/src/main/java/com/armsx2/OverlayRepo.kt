package com.armsx2

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import java.util.zip.ZipInputStream

/**
 * RetroArch OVERLAY packs — the `.cfg` + image kind people already own for other systems
 * (requested by Slogik). Deliberately separate from the shader chain: Mega Bezel and koko-aio give
 * bezels as SHADERS, which is a different (and much heavier) pipeline. An RA overlay is just
 * artwork drawn over the frame, so it composites for free and stacks with a shader preset — which
 * is exactly the "both together" the request asks for.
 *
 * Only the IMAGE half of the format is used. RA overlays can also carry input hitboxes
 * (`overlay0_desc0 = ...`), but ARMSX2 has its own touch layout with its own editor, so honouring
 * those would fight it — the artwork is the part that doesn't already exist here.
 */
object OverlayRepo {

    /** Folder under the data root that overlay packs live in. */
    const val OVERLAY_DIR = "overlays"

    private const val KEY_ACTIVE = "overlay.active"
    private const val KEY_OPACITY = "overlay.opacity"

    /** Path of the active overlay IMAGE (empty = none). */
    val activePath = mutableStateOf("")

    /** 0..1 draw alpha for the overlay art. */
    val opacity = mutableFloatStateOf(1.0f)

    /** Decoded active image, cached so the composable doesn't decode per frame. */
    @Volatile private var cachedPath: String? = null
    @Volatile private var cachedBitmap: Bitmap? = null

    fun root(context: Context): File =
        File(MainActivityRuntime.assetCopyRoot(context), OVERLAY_DIR).apply { mkdirs() }

    fun load() {
        runCatching {
            activePath.value = MainActivityRuntime.prefs.getString(KEY_ACTIVE, "").orEmpty()
            opacity.floatValue = MainActivityRuntime.prefs.getFloat(KEY_OPACITY, 1.0f).coerceIn(0.05f, 1f)
        }
    }

    fun setActive(path: String) {
        activePath.value = path
        cachedPath = null
        cachedBitmap = null
        runCatching { MainActivityRuntime.prefs.edit().putString(KEY_ACTIVE, path).apply() }
    }

    fun setOpacity(v: Float) {
        val c = v.coerceIn(0.05f, 1f)
        opacity.floatValue = c
        runCatching { MainActivityRuntime.prefs.edit().putFloat(KEY_OPACITY, c).apply() }
    }

    /** One selectable overlay: the display name and the image to draw. */
    data class Entry(val name: String, val imagePath: String)

    /**
     * Every overlay we can draw, found under [root].
     *
     * Prefers a `.cfg` (the real RA format) and resolves its `overlay0_overlay` image relative to
     * the cfg. Falls back to listing loose images, because plenty of "overlay packs" in the wild
     * are just a folder of PNGs — refusing those would reject files that work perfectly.
     */
    fun list(context: Context): List<Entry> {
        val base = root(context)
        if (!base.isDirectory) return emptyList()
        val out = LinkedHashMap<String, Entry>()
        base.walkTopDown().filter { it.isFile }.forEach { f ->
            when {
                f.extension.equals("cfg", true) -> {
                    val img = imageFromCfg(f)
                    if (img != null && img.isFile) out[img.absolutePath] = Entry(f.nameWithoutExtension, img.absolutePath)
                }
                f.extension.lowercase() in IMAGE_EXTS -> {
                    // Keyed by path so a cfg-declared image already added wins its nicer name.
                    out.putIfAbsent(f.absolutePath, Entry(f.nameWithoutExtension, f.absolutePath))
                }
            }
        }
        return out.values.sortedBy { it.name.lowercase() }
    }

    /** Resolve `overlay0_overlay = foo.png` (quoted or not) against the cfg's own folder. */
    private fun imageFromCfg(cfg: File): File? = runCatching {
        val line = cfg.readLines().firstOrNull { it.trimStart().startsWith("overlay0_overlay") }
            ?: return@runCatching null
        val raw = line.substringAfter('=', "").trim().trim('"')
        if (raw.isBlank()) return@runCatching null
        File(cfg.parentFile, raw).takeIf { it.isFile } ?: File(raw).takeIf { it.isFile }
    }.getOrNull()

    /** Decoded bitmap for the active overlay, or null when none/unreadable. */
    fun activeBitmap(): Bitmap? {
        val path = activePath.value
        if (path.isBlank()) return null
        cachedBitmap?.let { if (cachedPath == path && !it.isRecycled) return it }
        val bmp = runCatching {
            BitmapFactory.decodeFile(path, BitmapFactory.Options().apply {
                inPreferredConfig = Bitmap.Config.ARGB_8888
            })
        }.getOrNull()
        cachedPath = path
        cachedBitmap = bmp
        return bmp
    }

    /**
     * Import a whole overlay FOLDER (SAF tree) — the shape RetroArch packs actually come in.
     *
     * ★ This is the one that matters for a `.cfg`. A single-document import can only ever copy the
     * one file the picker returned, so importing a lone .cfg brought the config across and left the
     * PNG it points at behind; [list] then resolved `overlay0_overlay` to a file that wasn't there,
     * dropped the entry, and the import looked like it did nothing at all. A tree URI can read the
     * siblings, so the cfg and its images arrive together.
     *
     * Returns how many files landed.
     */
    fun importTree(context: Context, treeUri: android.net.Uri): Int {
        val doc = androidx.documentfile.provider.DocumentFile.fromTreeUri(context, treeUri) ?: return 0
        val dest = File(root(context), (doc.name ?: "overlay").replace(Regex("[^A-Za-z0-9._-]"), "_"))
            .apply { mkdirs() }
        var n = 0
        fun copyInto(dir: androidx.documentfile.provider.DocumentFile, into: File) {
            dir.listFiles().forEach { child ->
                val name = child.name ?: return@forEach
                if (child.isDirectory) {
                    // Packs are often one folder deep (an "img" subfolder next to the cfg).
                    copyInto(child, File(into, name.replace(Regex("[^A-Za-z0-9._-]"), "_")).apply { mkdirs() })
                    return@forEach
                }
                val ext = name.substringAfterLast('.', "").lowercase()
                if (ext != "cfg" && ext !in IMAGE_EXTS) return@forEach
                runCatching {
                    val target = File(into, name)
                    context.contentResolver.openInputStream(child.uri)?.use { input ->
                        target.outputStream().use { input.copyTo(it) }
                    }
                    n++
                }
            }
        }
        copyInto(doc, dest)
        return n
    }

    /** Import a single file (.zip, or a bare image) into [root]. Returns how many files landed. */
    fun importFrom(context: Context, uri: android.net.Uri, displayName: String?): Int {
        val base = root(context)
        val isZip = displayName?.endsWith(".zip", true) == true
        return runCatching {
            if (isZip) {
                val dest = File(base, displayName!!.removeSuffix(".zip").removeSuffix(".ZIP")).apply { mkdirs() }
                var n = 0
                context.contentResolver.openInputStream(uri)?.use { input ->
                    ZipInputStream(input.buffered()).use { zip ->
                        while (true) {
                            val e = zip.nextEntry ?: break
                            if (e.isDirectory) continue
                            // Reject path traversal: a crafted zip could otherwise write outside base.
                            val target = File(dest, e.name).canonicalFile
                            if (!target.path.startsWith(dest.canonicalFile.path)) continue
                            target.parentFile?.mkdirs()
                            target.outputStream().use { zip.copyTo(it) }
                            n++
                        }
                    }
                }
                n
            } else {
                val name = displayName ?: "overlay"
                val target = File(base, name)
                target.parentFile?.mkdirs()
                context.contentResolver.openInputStream(uri)?.use { input ->
                    target.outputStream().use { input.copyTo(it) }
                }
                1
            }
        }.getOrDefault(0)
    }

    private val IMAGE_EXTS = setOf("png", "jpg", "jpeg", "webp")

    // ---- In-app downloader -------------------------------------------------------------------
    // Importing by hand is fiddly (a .cfg is useless without the image it points at), so overlays
    // can be pulled straight from libretro's own collection instead. Same shape as the shader
    // downloader: browse a list, tap one, it lands ready to use.

    /** libretro's overlay collection — the canonical source RetroArch itself ships. */
    private const val OVERLAY_REPO_RAW = "https://raw.githubusercontent.com/libretro/common-overlays/master"
    private const val OVERLAY_REPO_TREE =
        "https://api.github.com/repos/libretro/common-overlays/git/trees/master?recursive=1"

    /** Folders worth offering for a PS2: full-screen borders and CRT/scanline effects. The
     *  console-specific gamepad/keyboard overlays in the same repo are for on-screen controls,
     *  which ARMSX2 has its own editor for, so they are deliberately not listed. */
    private val CATALOG_DIRS = listOf("borders/", "effects/")

    data class CatalogEntry(val name: String, val path: String)

    @Volatile private var catalogCache: List<CatalogEntry>? = null

    /** List the downloadable overlays. BLOCKING — call from a worker. Cached for the session. */
    fun fetchCatalog(): List<CatalogEntry> {
        catalogCache?.let { return it }
        val body = httpText(OVERLAY_REPO_TREE) ?: return emptyList()
        // The tree JSON is one flat "path" list; a regex beats pulling in a JSON parse for this.
        val out = Regex("\"path\"\\s*:\\s*\"([^\"]+\\.cfg)\"").findAll(body)
            .map { it.groupValues[1] }
            .filter { p -> CATALOG_DIRS.any { p.startsWith(it) } }
            .map { p ->
                // "effects/crt-bezels/foo.cfg" -> "crt-bezels / foo"
                val pretty = p.removeSuffix(".cfg").split('/').drop(1).joinToString(" / ")
                CatalogEntry(pretty.ifBlank { p.removeSuffix(".cfg") }, p)
            }
            .distinctBy { it.path }
            .sortedBy { it.name.lowercase() }
            .toList()
        if (out.isNotEmpty()) catalogCache = out
        return out
    }

    /**
     * Download one catalog overlay: its .cfg AND the image the cfg points at.
     *
     * The image reference is RELATIVE to the cfg ("img/tv-integer.png"), so the same relative
     * layout is recreated on disk — that is exactly what [list]'s resolver expects, which is why
     * a downloaded overlay works immediately while a hand-imported lone cfg could not.
     *
     * Returns the number of files written (0 = failed).
     */
    fun downloadFromCatalog(context: Context, entry: CatalogEntry): Int {
        val cfgText = httpText("$OVERLAY_REPO_RAW/${entry.path}") ?: return 0
        val folder = File(root(context), entry.name.replace(Regex("[^A-Za-z0-9._-]"), "_")).apply { mkdirs() }
        var n = 0
        runCatching {
            File(folder, entry.path.substringAfterLast('/')).writeText(cfgText)
            n++
        }
        // Pull every image the cfg references (overlay0_overlay, overlay1_overlay, ...).
        Regex("(?m)^\\s*overlay\\d+_overlay\\s*=\\s*\"?([^\"\\r\\n#]+)\"?").findAll(cfgText).forEach { m ->
            val rel = m.groupValues[1].trim()
            if (rel.isEmpty()) return@forEach
            val remoteDir = entry.path.substringBeforeLast('/', "")
            val target = File(folder, rel)
            target.parentFile?.mkdirs()
            if (httpFile("$OVERLAY_REPO_RAW/$remoteDir/$rel", target)) n++
        }
        return n
    }

    private fun httpText(url: String): String? = runCatching {
        val conn = (java.net.URL(url).openConnection() as java.net.HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 20_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "ARMSX2")
        }
        if (conn.responseCode != java.net.HttpURLConnection.HTTP_OK) return@runCatching null
        conn.inputStream.bufferedReader().use { it.readText() }
    }.getOrNull()

    private fun httpFile(url: String, dest: File): Boolean = runCatching {
        val conn = (java.net.URL(url).openConnection() as java.net.HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 20_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "ARMSX2")
        }
        if (conn.responseCode != java.net.HttpURLConnection.HTTP_OK) return@runCatching false
        conn.inputStream.use { input -> dest.outputStream().use { input.copyTo(it) } }
        dest.length() > 0
    }.getOrDefault(false)
}
