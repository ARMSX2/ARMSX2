package com.armsx2.memcard

import java.util.zip.CRC32

/**
 * Finds a game's save icon on its own disc, so a game with no save yet can still have a Memory
 * Card Cover. The game has to keep the icon somewhere it can copy it to a memory card from, and
 * it is nearly always one of three places, tried cheapest first:
 *
 * 1. The main executable (named in SYSTEM.CNF) or a small file of its own (ICON.ICO, VIEW.ICO,
 *    SAVEGAME.ICO...): a few MB read, well under a second.
 * 2. An entry in the game's data archive. Entries start on a sector, and archives list where
 *    their entries start in a table near the front (or back) of the file, or in a small index
 *    file beside it. So rather than read a 4 GB disc, read those tables and look at the first
 *    bytes of each entry. Jackie Chan's DATA1.AIF lists its icons at 480 MB in; Chaos Legion's
 *    sit 16 KB into LEGION.DAT.
 * 3. Nowhere readable: packed in a compressed archive (Crash Twinsanity). Only a save helps then.
 *
 * CVM files (CRI's ROFS, used by Persona, Tales and Sonic) are an ISO volume inside a file, so
 * their contents join the file list as if they were on the disc itself.
 *
 * Every candidate is confirmed by parsing it whole as an icon, and the PS2's network-settings icon
 * (SYS_NET.ICO, shipped on online games' discs) is never taken for the game's own. No icon.sys is
 * taken from a disc: the "PS2D" in an executable is nearly always code that writes one, not a
 * template, and the real templates found are the network settings' or light everything fully.
 */
object DiscIcons {
    class Found(val icon: ByteArray, val where: String)

    /** How a search went, for measuring. */
    class Stats {
        var files = 0
        var probes = 0
        var step = 0
    }

    private class DiscFile(val path: String, val lba: Long, val size: Long, val container: Boolean = false) {
        val name get() = path.substringAfterLast('\\')
        val ext get() = name.substringAfterLast('.', "")
        val base get() = name.substringBeforeLast('.')
    }

    private class Candidate(val bytes: ByteArray, val where: String, val score: Long)

    // Media never holds an icon; its streams are most of a disc, so skipping them is most of the win.
    private val MEDIA = setOf(
        "PSS", "PSM", "M2V", "MPG", "SFD", "BIK", "STR", "IPU", "PMF", "VAG", "VGS", "VPK", "ADS", "SS2",
        "MIB", "XA", "WAV", "ADX", "AT3", "MUS", "VB", "IRX", "DAT_BGM", "BGM", "SND", "SEQ", "HD", "BD", "SQ",
    )

    private const val HEAD = 256 * 1024
    private const val TAIL = 128 * 1024
    private const val MAX_ELF = 48 * 1024 * 1024
    private const val MAX_PROBES = 20000
    private const val MIN_RUN = 8

    /** The network-settings icon every online game's disc carries: byte-identical everywhere. */
    private const val SYS_NET_CRC = 0x2c6bee19L
    private const val SYS_NET_LENGTH = 33688

    fun find(disc: DiscImage, stats: Stats = Stats()): Found? {
        val files = files(disc)
        stats.files = files.size
        if (files.isEmpty()) return null
        val found = ArrayList<Candidate>()

        // 1. The executable and small icon-named files.
        stats.step = 1
        val boot = bootFile(disc, files)
        if (boot != null && boot.size in 1..MAX_ELF) scanFile(disc, boot, "${boot.name} (executable)", found)
        for (f in files) {
            if (f === boot || f.size !in 20..(2L shl 20) || !iconNamed(f.name)) continue
            val b = disc.readBytes(f.lba * DiscImage.SECTOR, f.size.toInt())
            collect(b, 0, b.size, f.name, found)
        }
        best(found)?.let { return Found(it.bytes, it.where) }

        // 2. Archives: their heads and tails, then every entry their tables list.
        stats.step = 2
        val archives = files.filter { it !== boot && !it.container && it.size >= 64 * 1024 && it.ext !in MEDIA }
            .sortedByDescending { it.size }.take(40)
        val tables = ArrayList<Pair<DiscFile, LongArray>>()
        for (f in archives) {
            val head = disc.readBytes(f.lba * DiscImage.SECTOR, f.size.coerceAtMost(HEAD.toLong()).toInt())
            collect(head, 0, head.size, f.name, found)
            tables += f to entriesIn(head, f.size)
            if (f.size > HEAD + TAIL) {
                val tailPos = f.size - TAIL
                val tail = disc.readBytes(f.lba * DiscImage.SECTOR + tailPos, TAIL)
                tables += f to entriesIn(tail, f.size)
            }
            // An index kept beside the archive: DATA.HED for DATA.BIN, and so on.
            for (idx in files) {
                if (idx === f || idx.base != f.base || idx.size !in 16..(1L shl 20) || idx.ext in MEDIA) continue
                val b = disc.readBytes(idx.lba * DiscImage.SECTOR, idx.size.toInt())
                tables += f to entriesIn(b, f.size)
            }
        }
        best(found)?.let { return Found(it.bytes, it.where) }

        for ((f, offsets) in tables) {
            for (off in offsets) {
                if (stats.probes >= MAX_PROBES) break
                stats.probes++
                val at = f.lba * DiscImage.SECTOR + off
                val head = disc.readBytes(at, 20)
                if (!looksLikeIconHeader(head, 0)) continue
                val shapes = le32(head, 4).toInt()
                val vertices = le32(head, 16).toInt()
                val want = (20L + vertices.toLong() * (shapes * 8 + 16) + 96 * 1024).coerceAtMost(f.size - off).toInt()
                val b = disc.readBytes(at, want)
                collect(b, 0, 4, "${f.name} +0x${off.toString(16)}", found)
            }
            best(found)?.let { return Found(it.bytes, it.where) }
        }
        stats.step = 3
        return null
    }

    // ---- the file system -----------------------------------------------------------------

    private fun files(disc: DiscImage): List<DiscFile> {
        val out = ArrayList<DiscFile>()
        val seen = HashSet<Long>()
        if (!volume(disc, 0, "", out, seen)) return emptyList()
        // CVMs: a 6 KB header, then an ISO volume whose sectors count from there.
        val listed = out.toList()
        for ((i, f) in listed.withIndex()) {
            if (f.size < CVM_HEADER + 17L * DiscImage.SECTOR || !(f.ext == "CVM" || f.size >= 16L shl 20)) continue
            val magic = disc.readBytes(f.lba * DiscImage.SECTOR, 4)
            if (magic.size < 4 || String(magic, Charsets.ISO_8859_1) != "CVMH") continue
            if (volume(disc, f.lba + CVM_HEADER / DiscImage.SECTOR, f.path, out, seen)) {
                out[i] = DiscFile(f.path, f.lba, f.size, container = true)
            }
        }
        return out
    }

    private const val CVM_HEADER = 0x1800

    /** The files of the ISO volume whose sector 0 is disc sector [base], added to [out]. */
    private fun volume(disc: DiscImage, base: Long, prefix: String, out: MutableList<DiscFile>, seen: MutableSet<Long>): Boolean {
        val pvd = disc.read(base + 16, 1)
        if (pvd.size < DiscImage.SECTOR || pvd[0] != 1.toByte() || String(pvd, 1, 5, Charsets.ISO_8859_1) != "CD001") return false
        walk(disc, base, le32(pvd, 158), le32(pvd, 166), prefix, 0, out, seen)
        return true
    }

    private fun walk(disc: DiscImage, base: Long, lba: Long, length: Long, prefix: String, depth: Int, out: MutableList<DiscFile>, seen: MutableSet<Long>) {
        if (depth > 8 || out.size > 50_000 || !seen.add(base + lba)) return
        val sectors = ((length + DiscImage.SECTOR - 1) / DiscImage.SECTOR).coerceIn(1, 256).toInt()
        val data = disc.read(base + lba, sectors)
        var o = 0
        while (o < data.size) {
            val n = data[o].toInt() and 0xFF
            if (n == 0) { // records never cross a sector: the rest of this one is padding
                o = (o / DiscImage.SECTOR + 1) * DiscImage.SECTOR
                continue
            }
            if (n < 34 || o + n > data.size) break
            val extent = le32(data, o + 2)
            val size = le32(data, o + 10)
            val directory = data[o + 25].toInt() and 2 != 0
            val nameLength = data[o + 32].toInt() and 0xFF
            val raw = data.copyOfRange(o + 33, (o + 33 + nameLength).coerceAtMost(o + n))
            o += n
            if (raw.size == 1 && (raw[0].toInt() == 0 || raw[0].toInt() == 1)) continue // . and ..
            val path = prefix + "\\" + String(raw, Charsets.ISO_8859_1).substringBefore(';').uppercase()
            if (directory) walk(disc, base, extent, size, path, depth + 1, out, seen) else out += DiscFile(path, base + extent, size)
        }
    }

    /** The executable SYSTEM.CNF boots ("BOOT2 = cdrom0:\SLUS_203.12;1"). */
    private fun bootFile(disc: DiscImage, files: List<DiscFile>): DiscFile? {
        val cnf = files.firstOrNull { it.path == "\\SYSTEM.CNF" } ?: return null
        val text = String(disc.readBytes(cnf.lba * DiscImage.SECTOR, cnf.size.coerceAtMost(4096).toInt()), Charsets.ISO_8859_1)
        val line = text.lineSequence().firstOrNull { it.trim().startsWith("BOOT2", ignoreCase = true) } ?: return null
        val at = line.indexOf("cdrom0:", ignoreCase = true).takeIf { it >= 0 } ?: return null
        var path = line.substring(at + 7).substringBefore(';').trim().uppercase().replace('/', '\\')
        if (!path.startsWith("\\")) path = "\\" + path
        return files.firstOrNull { it.path == path }
    }

    private fun iconNamed(name: String) = name.endsWith(".ICO") || name.endsWith(".ICN") || "ICON" in name ||
        (name.endsWith(".SYS") && name != "SYSTEM.CNF")

    // ---- icons in bytes ------------------------------------------------------------------

    /** Every icon in [f], a window at a time, so a 40 MB executable is never in memory whole. The
     *  windows overlap by more than the largest icon, so none is cut in two. */
    private fun scanFile(disc: DiscImage, f: DiscFile, where: String, out: MutableList<Candidate>) {
        val window = 8 shl 20
        val overlap = 2 shl 20
        var pos = 0L
        while (pos < f.size) {
            val length = (f.size - pos).coerceAtMost(window.toLong()).toInt()
            val b = disc.readBytes(f.lba * DiscImage.SECTOR + pos, length)
            val last = pos + length >= f.size
            collect(b, 0, if (last) b.size else b.size - overlap, where, out, base = pos)
            if (last) break
            pos += window - overlap
        }
    }

    /** Every whole icon starting in b[from until to] (4-byte aligned), added to [out]; [base] is
     *  where b starts in its file, for saying where each was. */
    private fun collect(b: ByteArray, from: Int, to: Int, where: String, out: MutableList<Candidate>, base: Long = 0) {
        var o = from - from % 4
        while (o < to && o + 20 <= b.size) {
            if (b[o].toInt() == 0 && b[o + 1].toInt() == 0 && b[o + 2].toInt() == 1 && b[o + 3].toInt() == 0) {
                val length = iconLength(b, o)
                if (length > 0) {
                    val bytes = b.copyOfRange(o, o + length)
                    val icon = Ps2Icon.parse(bytes)
                    if (icon != null && !isNetworkIcon(bytes)) {
                        val place = if (base + o == 0L) where else "$where +0x${(base + o).toString(16)}"
                        out += Candidate(bytes, place, icon.vertexCount.toLong() * (1 + icon.shapeCount))
                    }
                    o += (length + 3) and 3.inv()
                    continue
                }
            }
            o += 4
        }
    }

    private fun best(found: List<Candidate>) = found.maxByOrNull { it.score }

    private fun isNetworkIcon(b: ByteArray): Boolean {
        if (b.size != SYS_NET_LENGTH) return false
        return CRC32().apply { update(b) }.value == SYS_NET_CRC
    }

    private fun looksLikeIconHeader(b: ByteArray, o: Int): Boolean {
        if (o + 20 > b.size || le32(b, o) != 0x00010000L) return false
        val shapes = le32(b, o + 4)
        val texType = le32(b, o + 8)
        val vertices = le32(b, o + 16)
        return shapes in 1..16 && texType <= 0xFF && vertices in 3..30000 && vertices % 3 == 0L
    }

    /** The whole length of the icon at b[o], or -1 when it isn't one (or doesn't fit). */
    private fun iconLength(b: ByteArray, o: Int): Int {
        if (!looksLikeIconHeader(b, o)) return -1
        val shapes = le32(b, o + 4).toInt()
        val texType = le32(b, o + 8).toInt()
        val vertices = le32(b, o + 16).toInt()
        var p = o + 20L + vertices.toLong() * (shapes * 8 + 16)
        if (p + 20 > b.size) return -1
        if (le32(b, p.toInt()) != 1L) return -1
        val frameLength = le32(b, p.toInt() + 4)
        val frames = le32(b, p.toInt() + 16)
        if (frames > 64 || frameLength > 100_000) return -1
        p += 20
        repeat(frames.toInt()) {
            if (p + 8 > b.size) return -1
            val shape = le32(b, p.toInt())
            val keys = le32(b, p.toInt() + 4)
            if (shape > 64 || keys > 2000) return -1
            p += 8 + keys * 8
        }
        if (texType and 8 != 0) {
            if (p + 4 > b.size) return -1
            val size = le32(b, p.toInt())
            if (size !in 1..0x10000) return -1
            p += 4 + size
        } else {
            p += 128 * 128 * 2
        }
        if (p > b.size) return -1
        return (p - o).toInt()
    }

    // ---- tables of contents --------------------------------------------------------------

    /**
     * Where entries of a file of [fileSize] bytes may start, going by any table of offsets in [b]:
     * runs of at least eight 32-bit values at a fixed stride (8 to 64 bytes) that climb steadily
     * and stay inside the file, counted either in bytes (then each a whole sector) or in sectors.
     * No format is assumed, so it fits AFS, AIF and most home-grown packs alike; what isn't really
     * a table only costs a few wasted looks.
     */
    private fun entriesIn(b: ByteArray, fileSize: Long): LongArray {
        val words = b.size / 4
        if (words < MIN_RUN) return LongArray(0)
        val w = LongArray(words) { le32(b, it * 4) }
        val out = LinkedHashSet<Long>()
        for (unit in longArrayOf(1L, DiscImage.SECTOR.toLong())) {
            for (stride in intArrayOf(2, 3, 4, 5, 6, 8, 16)) {
                for (phase in 0 until stride) {
                    var start = -1
                    var count = 0
                    var previous = -1L
                    var allOnes = true
                    var k = phase
                    fun flush() {
                        if (count >= MIN_RUN && !(unit != 1L && allOnes)) {
                            var i = start
                            repeat(count) { out += w[i] * unit; i += stride }
                        }
                    }
                    while (k <= words) {
                        val v = if (k < words) w[k] else -1L
                        val inside = v >= 0 && v * unit < fileSize && (unit != 1L || v % DiscImage.SECTOR == 0L)
                        if (inside && v > previous) {
                            if (count == 0) { start = k; allOnes = true } else if (v - previous != 1L) allOnes = false
                            count++
                            previous = v
                        } else {
                            flush()
                            if (inside) { start = k; count = 1; previous = v; allOnes = true } else { count = 0; previous = -1L }
                        }
                        k += stride
                    }
                    if (out.size > MAX_PROBES * 4) return out.toLongArray()
                }
            }
        }
        return out.toLongArray()
    }

    private fun le32(b: ByteArray, o: Int): Long =
        ((b[o].toLong() and 0xFF) or ((b[o + 1].toLong() and 0xFF) shl 8) or
            ((b[o + 2].toLong() and 0xFF) shl 16) or ((b[o + 3].toLong() and 0xFF) shl 24))
}
