package com.armsx2.memcard

import java.io.File
import java.io.InputStream
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/**
 * The online icons on the device, in [dir]: the set's index.txt.zst ("SERIAL HASH" lines) and
 * catalog.txt.zst ("HASH<tab>title<tab>which save<tab>contributors" for every icon), and one
 * icons/HASH.zst per icon the player has downloaded, kept compressed. HASH is the start of the
 * SHA-1 of the icon's bytes (icon.sys, then the icon), so every icon is checked as it arrives
 * and nothing half-written or wrong is ever kept. Files are written beside their place and
 * renamed into it.
 *
 * [decode] turns one zstd frame into its bytes: the app's JNI decoder, a plain copy in tests.
 */
internal class OnlineIconStore(val dir: File, private val decode: (InputStream, Long) -> ByteArray) {
    private val iconDir = File(dir, "icons")

    @Volatile private var index: Map<String, String>? = null
    @Volatile private var catalog: List<OnlineIcons.Entry>? = null

    /** The icon for [serial], by hash, whether it is downloaded or not. */
    fun hashFor(serial: String): String? = index()[serial.uppercase()]

    /** Every icon the set has, downloaded or not. Empty until the catalog has been fetched. */
    fun catalog(): List<OnlineIcons.Entry> = catalog ?: readCatalog().also { catalog = it }

    fun isInstalled(hash: String): Boolean = File(iconDir, "$hash.zst").isFile

    fun installedHashes(): Set<String> =
        iconDir.listFiles()?.mapNotNullTo(HashSet()) { f -> f.name.removeSuffix(".zst").takeIf { f.name.endsWith(".zst") } }.orEmpty()

    /** Bytes the downloaded icons take on the device. */
    fun installedBytes(): Long = iconDir.listFiles()?.sumOf { it.length() } ?: 0L

    /** A downloaded icon's bytes, or null. */
    fun read(hash: String): ByteArray? {
        val f = File(iconDir, "$hash.zst")
        if (!f.isFile) return null
        return runCatching { decode(f.inputStream(), MAX_ICON_BYTES) }.getOrNull()
    }

    /** Keeps an icon's compressed bytes if they are what [hash] says they are. */
    fun install(hash: String, compressed: ByteArray): Boolean {
        if (decodeChecked(hash, compressed) == null) return false
        iconDir.mkdirs()
        return put(File(iconDir, "$hash.zst"), compressed)
    }

    /** An icon's bytes from its compressed ones, or null unless they are what [hash] says: the
     *  start of their SHA-1, and an icon.sys to begin with. */
    fun decodeChecked(hash: String, compressed: ByteArray): ByteArray? {
        if (!HASH.matches(hash)) return null
        val bytes = runCatching { decode(compressed.inputStream(), MAX_ICON_BYTES) }.getOrNull() ?: return null
        val sha = MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
        return bytes.takeIf { sha.startsWith(hash) && it.size > Ps2IconSys.SIZE && Ps2IconSys.parse(it) != null }
    }

    fun uninstall(hash: String) {
        File(iconDir, "$hash.zst").delete()
    }

    fun uninstallAll() {
        iconDir.listFiles()?.forEach { it.delete() }
    }

    /** Replaces the index or the catalog ("index.txt.zst", "catalog.txt.zst") with [compressed],
     *  if it decodes. */
    fun putList(name: String, compressed: ByteArray): Boolean {
        if (name != INDEX && name != CATALOG) return false
        if (runCatching { decode(compressed.inputStream(), MAX_TEXT_BYTES) }.isFailure) return false
        dir.mkdirs()
        if (!put(File(dir, name), compressed)) return false
        if (name == INDEX) index = null else catalog = null
        return true
    }

    /**
     * Installs everything in the published zip as it streams in: the index, the catalog and every
     * icon, each checked. Returns how many icons it kept; stops between entries if [keepGoing]
     * says so, keeping what it has.
     */
    fun installFromZip(zip: InputStream, keepGoing: () -> Boolean): Int {
        var icons = 0
        ZipInputStream(zip).use { z ->
            while (keepGoing()) {
                val entry = z.nextEntry ?: break
                val name = entry.name.removePrefix(ROOT)
                if (entry.isDirectory) continue
                when {
                    name == INDEX || name == CATALOG -> putList(name, z.readBounded(MAX_TEXT_BYTES))
                    name.startsWith("icons/") && name.endsWith(".zst") -> {
                        if (install(name.removePrefix("icons/").removeSuffix(".zst"), z.readBounded(MAX_ICON_BYTES))) icons++
                    }
                }
            }
        }
        return icons
    }

    private fun put(target: File, bytes: ByteArray): Boolean {
        val tmp = File(target.parentFile, target.name + ".part")
        return runCatching {
            tmp.writeBytes(bytes)
            if (!tmp.renameTo(target)) { tmp.delete(); false } else true
        }.getOrDefault(false)
    }

    private fun text(name: String): String? {
        val f = File(dir, name)
        if (!f.isFile) return null
        return runCatching { String(decode(f.inputStream(), MAX_TEXT_BYTES), Charsets.UTF_8) }.getOrNull()
    }

    private fun index(): Map<String, String> = index ?: HashMap<String, String>().also { map ->
        text(INDEX)?.lineSequence()?.forEach { line ->
            val space = line.indexOf(' ')
            if (space > 0 && !line.startsWith("#")) map[line.substring(0, space).uppercase()] = line.substring(space + 1).trim()
        }
        index = map
    }

    private fun readCatalog(): List<OnlineIcons.Entry> = text(CATALOG)?.lineSequence()?.mapNotNull { line ->
        val p = line.split('\t')
        if (p.size >= 2 && HASH.matches(p[0])) OnlineIcons.Entry(p[0], p[1], p.getOrElse(2) { "" }, p.getOrElse(3) { "" }) else null
    }?.sortedBy { it.title.lowercase() }?.toList().orEmpty()

    private fun InputStream.readBounded(max: Long): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val n = read(buf)
            if (n < 0) break
            total += n
            if (total > max) throw java.io.IOException("entry larger than $max bytes")
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }

    companion object {
        const val INDEX = "index.txt.zst"
        const val CATALOG = "catalog.txt.zst"
        private const val ROOT = "memcard-icons/"
        private const val MAX_TEXT_BYTES = 16L shl 20
        private const val MAX_ICON_BYTES = 4L shl 20
        private val HASH = Regex("[0-9a-f]{16}")
    }
}
