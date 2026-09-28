package com.armsx2.memcard

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.util.Calendar
import java.util.TimeZone

/**
 * One save folder on a PS2 memory card, as Memory Card Covers needs it: the folder name (which
 * carries the game's serial), when it was last written, and its files, read on demand from the
 * card it came from (so only while that card is open).
 */
class Ps2Save internal constructor(
    val folder: String,
    val modifiedMillis: Long,
    private val files: Map<String, () -> ByteArray?>,
) {
    val fileNames: Set<String> get() = files.keys

    /** A file's bytes, or null if it is missing or unreadable. Exact name first, then any case,
     *  since icon.sys names its icon files by hand and not every game got the case right. */
    fun read(name: String): ByteArray? {
        val reader = files[name] ?: files.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value
        return runCatching { reader?.invoke() }.getOrNull()
    }

    /** The disc serial the folder names, "BASLUS-20776spidermn" -> "SLUS-20776", or null. */
    val serial: String? get() = serialOf(folder)

    companion object {
        // A save folder is a two-letter region prefix (BA America, BE Europe, BI Japan...), then
        // the product code; whatever follows is the game's own.
        private val folderSerial = Regex("""^B[A-Z]([A-Z]{4})[-_]?(\d{3})\.?(\d{2})""")

        fun serialOf(folder: String): String? =
            folderSerial.find(folder.uppercase())?.let { "${it.groupValues[1]}-${it.groupValues[2]}${it.groupValues[3]}" }
    }
}

/**
 * Reads the saves on a PS2 memory card: a card image (.ps2, of any size, with or without the 16
 * bytes of ECC that follow each 512-byte page) or a PCSX2 folder card. Read-only and defensive: a
 * card that is unformatted, truncated or corrupt yields what it can, or nothing, and never throws.
 *
 * Images are read a page at a time rather than whole: a 128 MB card is a real thing people have,
 * and loading one into a phone's heap to find a few icons would be absurd.
 */
object Ps2MemoryCard {
    private const val MAGIC = "Sony PS2 Memory Card Format "
    private const val PAGE = 512
    private const val ENTRY = 512
    private const val MODE_EXISTS = 0x8000
    private const val MODE_DIR = 0x0020
    private const val MODE_FILE = 0x0010

    /** An open card. Its saves read their files from it, so use them before closing it. */
    interface Card : Closeable {
        fun saves(): List<Ps2Save>
    }

    /** Opens the card at [file], or null when it is not a card that can be read. */
    fun open(file: File): Card? = runCatching {
        when {
            file.isDirectory -> FolderCard(file)
            file.isFile -> ImageCard.open(file)
            else -> null
        }
    }.getOrNull()

    /** The saves on [file], read in one go; convenient when nothing else from the card is needed. */
    fun saveNames(file: File): List<String> = open(file)?.use { card -> card.saves().map { it.folder } }.orEmpty()

    /** A PCSX2 folder card: one directory per save, plus the core's own bookkeeping files. */
    private class FolderCard(private val dir: File) : Card {
        override fun saves(): List<Ps2Save> =
            (dir.listFiles() ?: emptyArray()).filter { it.isDirectory && !it.name.startsWith("_pcsx2") }.map { save ->
                val files = (save.listFiles() ?: emptyArray()).filter { it.isFile && !it.name.startsWith("_pcsx2") }
                Ps2Save(
                    folder = save.name,
                    modifiedMillis = files.maxOfOrNull { it.lastModified() } ?: save.lastModified(),
                    files = files.associate { f -> f.name to { runCatching { f.readBytes() }.getOrNull() } },
                )
            }

        override fun close() = Unit
    }

    private class ImageCard(
        private val raf: RandomAccessFile,
        private val stride: Int,
        private val pagesPerCluster: Int,
        private val clustersPerCard: Int,
        private val allocOffset: Int,
        private val rootCluster: Int,
        private val ifcList: IntArray,
    ) : Card {
        private val clusterSize = PAGE * pagesPerCluster
        private val perCluster = clusterSize / 4
        private val pages = raf.length() / stride

        // The FAT's own clusters are read over and over while following chains; keep them.
        private val fatCache = HashMap<Int, ByteArray>()

        fun cluster(abs: Int): ByteArray? {
            if (abs < 0 || abs >= clustersPerCard) return null
            val out = ByteArray(clusterSize)
            for (i in 0 until pagesPerCluster) {
                val page = abs.toLong() * pagesPerCluster + i
                if (page >= pages) return null
                raf.seek(page * stride)
                raf.readFully(out, i * PAGE, PAGE)
            }
            return out
        }

        private fun cachedCluster(abs: Int): ByteArray? =
            fatCache[abs] ?: cluster(abs)?.also { if (fatCache.size < 4096) fatCache[abs] = it }

        /** The FAT entry for cluster [rel] (relative to the allocatable area), through the double
         *  indirection the format uses: ifc_list -> indirect FAT cluster -> FAT cluster. */
        fun fat(rel: Int): Int? {
            if (rel < 0) return null
            val fatIndex = rel / perCluster
            val indirect = ifcList.getOrNull(fatIndex / perCluster) ?: return null
            val fatCluster = cachedCluster(indirect)?.let { u32(it, (fatIndex % perCluster) * 4) } ?: return null
            return cachedCluster(fatCluster)?.let { u32(it, (rel % perCluster) * 4) }
        }

        /** [length] bytes of the chain that starts at [rel]. Bounded, so a looping FAT ends. */
        fun read(rel: Int, length: Int): ByteArray? {
            if (length < 0 || length.toLong() > clustersPerCard.toLong() * clusterSize) return null
            val out = ByteArray(length)
            var at = 0
            var c = rel
            var steps = 0
            while (at < length) {
                if (steps++ > clustersPerCard) return null
                val bytes = cluster(c + allocOffset) ?: return null
                val n = minOf(clusterSize, length - at)
                System.arraycopy(bytes, 0, out, at, n)
                at += n
                if (at >= length) break
                val e = fat(c) ?: return null
                // Bit 31 marks an allocated cluster; 0xFFFFFFFF ends the chain.
                if (e == -1 || e and Int.MIN_VALUE == 0) return null
                c = e and 0x7FFFFFFF
            }
            return out
        }

        private class Entry(val mode: Int, val length: Int, val cluster: Int, val name: String, val modified: Long)

        private fun entries(rel: Int, count: Int): List<Entry> {
            if (count <= 0 || count > 4096) return emptyList()
            val raw = read(rel, count * ENTRY) ?: return emptyList()
            return (0 until count).map { i ->
                val o = i * ENTRY
                var end = o + 0x40
                while (end < o + 0x60 && raw[end].toInt() != 0) end++
                Entry(
                    mode = u16(raw, o),
                    length = u32(raw, o + 4),
                    cluster = u32(raw, o + 0x10),
                    name = String(raw, o + 0x40, end - (o + 0x40), Charsets.US_ASCII),
                    modified = timestamp(raw, o + 0x18),
                )
            }
        }

        override fun saves(): List<Ps2Save> = runCatching {
            // The root's own "." entry holds how many entries the root has.
            val rootCount = entries(rootCluster, 1).firstOrNull()?.length ?: return emptyList()
            entries(rootCluster, rootCount).drop(2)
                .filter { it.mode and MODE_EXISTS != 0 && it.mode and MODE_DIR != 0 }
                .map { dir ->
                    val files = entries(dir.cluster, dir.length).drop(2)
                        .filter { it.mode and MODE_EXISTS != 0 && it.mode and MODE_FILE != 0 }
                    Ps2Save(
                        folder = dir.name,
                        modifiedMillis = dir.modified,
                        files = files.associate { f -> f.name to { runCatching { read(f.cluster, f.length) }.getOrNull() } },
                    )
                }
        }.getOrDefault(emptyList())

        override fun close() = raf.close()

        companion object {
            fun open(file: File): ImageCard? {
                val raf = RandomAccessFile(file, "r")
                val ok = runCatching {
                    val size = raf.length()
                    // 528-byte pages carry ECC after each 512 bytes of data; an image without it is
                    // a plain multiple of 512. Decided by size, as PCSX2 does.
                    val stride = when {
                        size % 528 == 0L && size / 528 >= 1024 -> 528
                        size % PAGE == 0L && size >= PAGE -> PAGE
                        else -> return@runCatching null
                    }
                    val sb = ByteArray(PAGE)
                    raf.seek(0)
                    raf.readFully(sb)
                    if (String(sb, 0, MAGIC.length, Charsets.US_ASCII) != MAGIC || u16(sb, 0x28) != PAGE) return@runCatching null
                    val ppc = u16(sb, 0x2A)
                    val clusters = u32(sb, 0x30)
                    if (ppc !in 1..16 || clusters !in 1..(1 shl 22)) return@runCatching null
                    ImageCard(
                        raf = raf,
                        stride = stride,
                        pagesPerCluster = ppc,
                        clustersPerCard = clusters,
                        allocOffset = u32(sb, 0x34),
                        rootCluster = u32(sb, 0x3C),
                        ifcList = IntArray(32) { u32(sb, 0x50 + it * 4) },
                    )
                }.getOrNull()
                if (ok == null) raf.close()
                return ok
            }
        }
    }

    internal fun u16(b: ByteArray, o: Int): Int = (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

    internal fun u32(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8) or
            ((b[o + 2].toInt() and 0xFF) shl 16) or ((b[o + 3].toInt() and 0xFF) shl 24)

    /** A card timestamp: unused, sec, min, hour, day, month, then a 16-bit year, in Japan time. */
    private fun timestamp(b: ByteArray, o: Int): Long = runCatching {
        Calendar.getInstance(TimeZone.getTimeZone("Asia/Tokyo")).apply {
            clear()
            set(u16(b, o + 6), (b[o + 5].toInt() and 0xFF) - 1, b[o + 4].toInt() and 0xFF,
                b[o + 3].toInt() and 0xFF, b[o + 2].toInt() and 0xFF, b[o + 1].toInt() and 0xFF)
        }.timeInMillis
    }.getOrDefault(0L)
}
