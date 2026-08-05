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

    /** Import a folder of overlay files, or a .zip, into [root]. Returns how many files landed. */
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
}
