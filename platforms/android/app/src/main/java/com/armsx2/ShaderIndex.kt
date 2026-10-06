package com.armsx2

import android.util.Log
import java.io.File

/**
 * On-disk index of every `.slangp` under the shaders root, with each preset's resolved pass
 * count.
 *
 * Without it, every open of the preset picker and of the Shader Packs list walked the whole
 * tree — ~5.6k files for the stock pack — and READ every one of its ~2.5k presets (plus the
 * `#reference` chains between them) to count passes. On Android's shared storage that is many
 * seconds, during which the picker showed nothing but its "None" row. Now the walk runs once,
 * while the user is already waiting on a download or import, and every later open reads one
 * file.
 *
 * Staleness is handled two ways:
 *  1. Every write under the shaders root that this app makes goes through [change], which
 *     deletes the index before and after the write. [change] and [load] share one lock, so a
 *     scan can never record a half-written tree.
 *  2. Changes made outside the app (a file manager) are caught by [fingerprint]: the
 *     modified times of the root, each pack folder, and each folder one level below that. A
 *     directory's modified time moves when an entry is added to, removed from, or renamed in
 *     it, so adding or removing a pack or a category forces a rescan. An edit deeper than that
 *     is not seen until the next write through [change].
 *
 * The index file lives BESIDE the root, not inside it: writing it inside would move the
 * root's own modified time and invalidate the fingerprint it was just written with.
 *
 * Every function is blocking IO. Call from Dispatchers.IO.
 */
object ShaderIndex {

    private const val TAG = "ShaderIndex"

    /** Bump when the scan's output changes meaning (e.g. how passes are counted), so an old
     *  index is rebuilt rather than trusted. */
    private const val HEADER = "armsx2-shader-index 1"

    /** One preset. [relPath] is relative to the shaders root with `/` separators; [passes]
     *  is null when the preset never yields a count. */
    data class Entry(val relPath: String, val passes: Int?)

    private val lock = Any()

    fun indexFile(root: File): File = File(root.absoluteFile.parentFile, ".${root.name}-preset-index")

    /** The presets under [root]: from the index when it is still valid, otherwise from a fresh
     *  scan, which is then written as the new index. */
    fun load(root: File): List<Entry> = synchronized(lock) {
        read(root)?.let { return it }
        val fp = fingerprint(root)
        val entries = try {
            scan(root)
        } catch (e: Exception) {
            // A tree being changed from outside the app can throw mid-walk. Don't record it.
            Log.w(TAG, "scan of $root failed: ${e.message}")
            return emptyList()
        }
        write(root, fp, entries)
        entries
    }

    /** Run [block], which changes files under [root], with the index removed for its whole
     *  duration. Removing it before as well as after means a crash part-way leaves no index
     *  rather than a stale one. */
    fun <T> change(root: File, block: () -> T): T = synchronized(lock) {
        indexFile(root).delete()
        try {
            block()
        } finally {
            indexFile(root).delete()
        }
    }

    /** [change] for a write that touches only [files] (saving or deleting one user preset).
     *  When the index was valid beforehand, it is patched for those files instead of being
     *  thrown away, which saves a full rescan for a one-file edit. */
    fun <T> changeFiles(root: File, files: List<File>, block: () -> T): T = synchronized(lock) {
        val before = read(root)
        val result = change(root, block)
        if (before != null) {
            val rootAbs = root.absoluteFile
            val touched = files.mapNotNull { relPathOrNull(rootAbs, it) }.toSet()
            val cache = HashMap<String, Int?>()
            val added = files.filter { it.isFile && isPreset(it) }.mapNotNull { f ->
                relPathOrNull(rootAbs, f)?.let { Entry(it, resolvePasses(f, cache, HashSet(), 0)) }
            }
            write(root, fingerprint(root), before.filterNot { it.relPath in touched } + added)
        }
        result
    }

    /** The parsed index, or null when there is none or it no longer matches the tree. */
    private fun read(root: File): List<Entry>? {
        val file = indexFile(root)
        if (!file.isFile) return null
        return try {
            file.bufferedReader().use { r ->
                if (r.readLine() != HEADER) return null
                if (r.readLine() != "root\t${root.absolutePath}") return null
                if (r.readLine() != "fp\t${fingerprint(root)}") return null
                val out = ArrayList<Entry>()
                while (true) {
                    val line = r.readLine() ?: break
                    val tab = line.indexOf('\t')
                    if (tab <= 0) return null
                    out += Entry(line.substring(tab + 1), line.substring(0, tab).toIntOrNull())
                }
                out
            }
        } catch (e: Exception) {
            Log.w(TAG, "unreadable index $file: ${e.message}")
            null
        }
    }

    /** Written to a temp file and renamed into place, so a reader never sees half an index. */
    private fun write(root: File, fp: String, entries: List<Entry>) {
        val file = indexFile(root)
        val tmp = File(file.path + ".tmp")
        try {
            tmp.bufferedWriter().use { w ->
                w.write(HEADER); w.write("\n")
                w.write("root\t${root.absolutePath}\n")
                w.write("fp\t$fp\n")
                for (e in entries) {
                    w.write(e.passes?.toString() ?: "-")
                    w.write("\t")
                    w.write(e.relPath)
                    w.write("\n")
                }
            }
            if (!tmp.renameTo(file)) {
                tmp.delete()
                Log.w(TAG, "could not move index into place at $file")
            }
        } catch (e: Exception) {
            tmp.delete()
            Log.w(TAG, "could not write index $file: ${e.message}")
        }
    }

    /** Modified times of the root, its folders, and their folders — see the class comment for
     *  what this catches and what it doesn't. */
    internal fun fingerprint(root: File): String {
        val sb = StringBuilder()
        sb.append(root.lastModified())
        fun folders(dir: File) = dir.listFiles { f -> f.isDirectory }.orEmpty().sortedBy { it.name }
        for (pack in folders(root)) {
            sb.append('|').append(pack.name).append(':').append(pack.lastModified())
            for (sub in folders(pack)) {
                sb.append('|').append(pack.name).append('/').append(sub.name).append(':').append(sub.lastModified())
            }
        }
        return sb.toString()
    }

    internal fun scan(root: File): List<Entry> {
        val rootAbs = root.absoluteFile
        if (!rootAbs.isDirectory) return emptyList()
        val cache = HashMap<String, Int?>()
        return rootAbs.walkTopDown()
            .filter { it.isFile && isPreset(it) }
            .mapNotNull { f ->
                relPathOrNull(rootAbs, f)?.let { Entry(it, resolvePasses(f, cache, HashSet(), 0)) }
            }
            .toList()
    }

    private fun isPreset(f: File) = f.extension.equals("slangp", ignoreCase = true)

    /** Null for a file outside [rootAbs], or one whose name would break the line format (one
     *  entry per line, so a newline in a file name can't be stored). */
    private fun relPathOrNull(rootAbs: File, f: File): String? =
        f.absoluteFile.relativeToOrNull(rootAbs)?.invariantSeparatorsPath
            ?.takeIf { it.isNotEmpty() && it != ".." && !it.startsWith("../") && '\n' !in it && '\r' !in it }

    // ---- Pass counting ------------------------------------------------------------------

    /** Depth cap for #reference chains. The deepest real chain in the stock pack is 7 hops
     *  (bezel/koko-aio/Presets-4.1/FXAA-bloom-immersive.slangp), so this is pure headroom for
     *  a future pack; the actual loop guard is the visited set in [resolvePasses]. */
    private const val MAX_REFERENCE_DEPTH = 16

    /** `shaders = 12` or `shaders = "12"` — 423 presets in the stock pack quote the value, so
     *  the quotes are not optional to handle. */
    private val SHADERS_RE = Regex("""^\s*shaders\s*=\s*"?(\d+)"?""", RegexOption.IGNORE_CASE)

    /** `#reference "../../Root_Presets/MBZ__3__STD__GDV.slangp"` — a preset that inherits its
     *  whole chain from another file and only overrides parameters. */
    private val REFERENCE_RE = Regex("""^\s*#reference\s+(.+?)\s*$""", RegexOption.IGNORE_CASE)

    /** What one `.slangp` says about its own cost: its pass count, or the presets it inherits
     *  one from. Never both in the stock pack — a `#reference` preset overrides parameters
     *  only — but [resolvePasses] prefers a local count anyway, which is the RetroArch rule. */
    private class PresetFacts(val shaders: Int?, val references: List<String>)

    private fun readPreset(file: File): PresetFacts {
        var shaders: Int? = null
        val references = ArrayList<String>(2)
        try {
            file.bufferedReader().useLines { lines ->
                for (line in lines) {
                    val hit = SHADERS_RE.find(line)
                    if (hit != null) {
                        shaders = hit.groupValues[1].toIntOrNull()
                        // A local count wins outright, so the references can't change the
                        // answer — stop reading. Bails on line 1 for most of the pack.
                        if (shaders != null) return@useLines
                    }
                    REFERENCE_RE.find(line)?.let {
                        references.add(it.groupValues[1].trim().trim('"', '\''))
                    }
                }
            }
        } catch (_: Exception) {
            // Unreadable / vanished mid-scan: the row says "cost unknown" rather than
            // inventing a number.
        }
        return PresetFacts(shaders, references)
    }

    /**
     * The preset's pass count, following `#reference` chains. null = undeterminable, which the
     * row reports honestly as "cost unknown".
     *
     * The reference hop is the whole reason this function exists rather than a one-line regex.
     * 691 of the stock pack's 2542 presets — essentially all of Mega Bezel and koko-aio — carry
     * no `shaders` key at all; they are thin files whose entire body is `#reference
     * "../some/root.slangp"` plus parameter overrides. Reading `shaders` naively finds nothing
     * for exactly those files, so without this the headline number would be blank on precisely
     * the biggest chains in the pack.
     *
     * Targets resolve against the REFERENCING file's directory (they are written `../../…`).
     * [seen] is the loop guard and [cache] memoises across the whole scan, both keyed on the
     * normalised absolute path. Not the canonical path: that costs a lookup per directory
     * level on every preset, and the depth cap already bounds a symlink loop.
     */
    private fun resolvePasses(
        file: File,
        cache: MutableMap<String, Int?>,
        seen: MutableSet<String>,
        depth: Int,
    ): Int? {
        val key = file.absoluteFile.normalize().path
        if (depth > MAX_REFERENCE_DEPTH || !seen.add(key)) return null
        if (cache.containsKey(key)) return cache[key]
        val facts = readPreset(file)
        val result = facts.shaders ?: facts.references.firstNotNullOfOrNull { ref ->
            val target = File(file.parentFile, ref)
            // Multi-reference presets (324 of them) list the .slangp first and .params after;
            // taking the first target that actually yields a count skips the parameter files
            // without having to sniff extensions.
            if (target.isFile) resolvePasses(target, cache, seen, depth + 1) else null
        }
        cache[key] = result
        return result
    }
}
