package com.armsx2.memcard

import org.junit.Assume
import org.junit.Test
import java.awt.Color
import java.awt.Font
import java.awt.RenderingHints
import java.awt.image.BufferedImage
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import javax.imageio.ImageIO

/**
 * Builds the icon archive the app fetches icons from for games with no save on the player's cards
 * and no icon it can read on the disc. Reads a folder of shared PS2 saves (PSV from a PS3, PSU from
 * uLaunchELF or EMS, Action Replay MAX, bare save folders, card images), keeps only each save's
 * icon.sys and list icon, checks that both parse, and writes one file per distinct icon plus an
 * index from serial to file. A serial with no save of its own points at the same game's icon from
 * another release (another region, a Greatest Hits), matched by GameIndex title as Cover Region is.
 *
 * Nothing else of a save is kept, and the icon.sys title is blanked: players type names into some.
 *
 * Skipped unless ICON_ARCHIVE_IN (the saves) and ICON_ARCHIVE_OUT are set: the saves are other
 * people's and never live in the repository, and neither does the archive.
 */
class IconArchiveDevTest {
    private class Candidate(val serial: String, val folder: String, val source: String, val bytes: ByteArray, val hash: String, val texture: String)

    @Test
    fun build() {
        val input = System.getenv("ICON_ARCHIVE_IN")?.let(::File)
        val out = System.getenv("ICON_ARCHIVE_OUT")?.let(::File)
        Assume.assumeTrue(input != null && input.isDirectory && out != null)
        out!!.mkdirs()
        val iconsDir = File(out, "icons").apply { mkdirs() }
        val log = StringBuilder()
        val bySerial = HashMap<String, MutableList<Candidate>>()
        val skipped = HashMap<String, Int>()
        var saves = 0

        fun take(save: Ps2Save, source: File) {
            saves++
            fun skip(why: String) { skipped[why] = (skipped[why] ?: 0) + 1 }
            val serial = save.serial ?: return skip("no serial in the folder name")
            // "BESLES-00000 DATEL" and the like: a cheat device's own saves, not a game's.
            if (serial.endsWith("-00000")) return skip("placeholder serial")
            val sysBytes = save.read("icon.sys") ?: return skip("no icon.sys")
            val sys = Ps2IconSys.parse(sysBytes) ?: return skip("icon.sys unreadable")
            val iconBytes = save.read(sys.iconNormal) ?: return skip("icon file missing")
            val icon = Ps2Icon.parse(iconBytes) ?: return skip("icon unreadable")
            val bytes = sanitized(sysBytes) + iconBytes
            val texture = icon.texture?.let { t -> sha1(ByteBuffer.allocate(t.size * 4).also { it.asIntBuffer().put(t) }.array()).take(12) } ?: "none"
            val candidate = Candidate(serial, save.folder, source.relativeTo(input!!).path, bytes, sha1(bytes).take(16), texture)
            bySerial.getOrPut(serial) { mutableListOf() } += candidate
        }

        for (f in input!!.walkTopDown()) {
            when {
                f.isDirectory && f.listFiles()?.any { it.isFile && it.name.equals("icon.sys", true) } == true ->
                    take(folderSave(f), f)
                !f.isFile -> Unit
                else -> when (f.extension.lowercase()) {
                    "psv" -> psv(f.readBytes())?.let { take(it, f) } ?: run { skipped["bad psv"] = (skipped["bad psv"] ?: 0) + 1 }
                    "psu" -> psu(f.readBytes())?.let { take(it, f) } ?: run { skipped["bad psu"] = (skipped["bad psu"] ?: 0) + 1 }
                    "max" -> max(f.readBytes())?.let { take(it, f) } ?: run { skipped["bad max"] = (skipped["bad max"] ?: 0) + 1 }
                    "ps2", "mcd", "mc2" -> Ps2MemoryCard.open(f)?.use { card -> card.saves().forEach { take(it, f) } }
                }
            }
        }

        val gameIndex = (System.getenv("GAMEINDEX")?.let(::File) ?: File("../../../bin/resources/GameIndex.yaml"))
        val titles = if (gameIndex.isFile) gameIndexTitles(gameIndex) else emptyMap()

        // A cheat device's or save tool's own icon (Action Replay Max's cube, X-Port's...) replaced
        // the game's on many shared saves. A game's own texture belongs to that game and its other
        // releases; one used by the saves of many different games is a brand, not an icon.
        val gamesByTexture = HashMap<String, MutableSet<String>>()
        for (list in bySerial.values) for (c in list) {
            if (c.texture != "none") gamesByTexture.getOrPut(c.texture) { HashSet() } += titles[c.serial] ?: c.serial
        }
        val brands = gamesByTexture.filterValues { it.size >= BRAND_GAMES }.keys + KNOWN_BRANDS
        val brandSerials = HashMap<String, MutableSet<String>>()
        for (list in bySerial.values) for (c in list) if (c.texture in brands) brandSerials.getOrPut(c.texture) { sortedSetOf() } += c.serial
        for (texture in gamesByTexture.entries.sortedByDescending { it.value.size }.take(40)) {
            log.appendLine("texture ${texture.key} used by ${texture.value.size} games${if (texture.key in brands) " BRAND" else ""}: ${texture.value.take(6)}")
        }
        brandSheet(brands.map { t -> bySerial.values.flatten().first { it.texture == t } }, File(out, "brands.png"))
        // Textures two or three games share: sequels and renamed releases, mostly; worth a look.
        brandSheet(gamesByTexture.filterValues { it.size in 2 until BRAND_GAMES }.keys.map { t -> bySerial.values.flatten().first { it.texture == t } }, File(out, "shared.png"))
        val dropped = bySerial.values.sumOf { list -> list.count { it.texture in brands } }
        for (key in bySerial.keys.toList()) {
            val kept = bySerial[key]!!.filter { it.texture !in brands }
            if (kept.isEmpty()) bySerial.remove(key) else bySerial[key] = kept.toMutableList()
        }
        log.appendLine("brand textures: ${brands.size}, saves dropped for them: $dropped")

        // One icon per serial: the one most of its saves use; a tie goes to the bigger icon.
        val chosen = bySerial.mapValues { (_, list) ->
            list.groupBy { it.hash }.values.sortedWith(
                compareByDescending<List<Candidate>> { it.size }.thenByDescending { it[0].bytes.size }.thenBy { it[0].hash },
            ).first()
        }
        val index = sortedMapOf<String, String>()
        for ((serial, group) in chosen) {
            val c = group[0]
            File(iconsDir, "${c.hash}.bin").writeBytes(c.bytes)
            index[serial] = c.hash
        }

        // Other releases of the same game, by GameIndex title, prefer their own region's icon.
        val aliases = sortedMapOf<String, String>()
        val groups = titles.entries.groupBy({ it.value }, { it.key })
        for ((serial, key) in titles) {
            if (serial in index) continue
            val have = groups[key].orEmpty().filter { it in chosen }
            if (have.isEmpty()) continue
            val region = regionOf(serial)
            val pick = have.sortedWith(
                compareByDescending<String> { regionOf(it) == region }.thenByDescending { chosen[it]!!.size }.thenBy { it },
            ).first()
            index[serial] = index[pick]!!
            aliases[serial] = pick
        }

        File(out, "index.txt").writeText(buildString {
            appendLine("# ARMSX2 memory card icons v1: SERIAL HASH, the icon is icons/HASH.bin (icon.sys, 964 bytes, then the icon)")
            for ((s, h) in index) appendLine("$s $h")
        })

        log.appendLine("saves read: $saves; serials with an icon of their own: ${chosen.size}; distinct icons: ${chosen.values.map { it[0].hash }.toSet().size}")
        log.appendLine("aliases from GameIndex titles: ${aliases.size} (index lines: ${index.size}); GameIndex: ${if (gameIndex.isFile) gameIndex.path else "missing"}")
        log.appendLine("skipped: $skipped")
        log.appendLine()
        for ((serial, group) in chosen.toSortedMap()) {
            val all = bySerial[serial]!!
            log.appendLine("$serial ${group[0].hash} ${group.size}/${all.size} saves (${all.map { it.hash }.toSet().size} icons) e.g. ${group[0].folder} <- ${group[0].source}")
        }
        log.appendLine()
        for ((serial, from) in aliases) log.appendLine("alias $serial -> $from (${titles[serial]})")
        File(out, "report.txt").writeText(log.toString())

        contactSheet(chosen.toSortedMap(), File(out, "sheet.png"))
        // ICON_ARCHIVE_FOCUS=SLUS-21008,...: those icons large, at four angles and four moments.
        System.getenv("ICON_ARCHIVE_FOCUS")?.split(',')?.map { it.trim() }?.filter { it in chosen }?.takeIf { it.isNotEmpty() }?.let { focus ->
            val size = 240
            val sheet = BufferedImage(4 * size, focus.size * 4 * size, BufferedImage.TYPE_INT_RGB)
            focus.forEachIndexed { n, serial ->
                val bytes = chosen[serial]!![0].bytes
                val sys = Ps2IconSys.parse(bytes.copyOfRange(0, Ps2IconSys.SIZE))!!
                val icon = Ps2Icon.parse(bytes.copyOfRange(Ps2IconSys.SIZE, bytes.size))!!
                val len = icon.frameLength.coerceAtLeast(1).toFloat()
                for (a in 0 until 4) for (t in 0 until 4) {
                    val px = Ps2IconRenderer.render(icon, sys, size, size, Ps2IconRenderer.Options(yaw = a * 1.5708f, time = t * len / 4f))
                    sheet.setRGB(t * size, (n * 4 + a) * size, size, size, px, 0, size)
                }
            }
            ImageIO.write(sheet, "png", File(out, "focus.png"))
        }
        println("ICON ARCHIVE ${log.lineSequence().take(3).joinToString(" | ")}")
    }

    /** One save per brand texture, to check by eye that each really is a tool's icon. */
    private fun brandSheet(examples: List<Candidate>, file: File) {
        contactSheet(examples.associate { "${it.serial} ${it.texture.take(6)}" to listOf(it) }, file)
    }

    /** Every chosen icon, labelled with its serial, to check them by eye. */
    private fun contactSheet(chosen: Map<String, List<Candidate>>, file: File) {
        val tileW = 112; val tileH = 150; val cols = minOf(20, maxOf(1, chosen.size))
        val list = chosen.entries.toList()
        val rows = (list.size + cols - 1) / cols
        if (rows == 0) return
        val sheet = BufferedImage(cols * tileW, rows * tileH, BufferedImage.TYPE_INT_RGB)
        val g = sheet.createGraphics()
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON)
        g.font = Font(Font.SANS_SERIF, Font.PLAIN, 11)
        list.forEachIndexed { i, (serial, group) ->
            val bytes = group[0].bytes
            val sys = Ps2IconSys.parse(bytes.copyOfRange(0, Ps2IconSys.SIZE))
            val icon = Ps2Icon.parse(bytes.copyOfRange(Ps2IconSys.SIZE, bytes.size))
            val x = (i % cols) * tileW; val y = (i / cols) * tileH
            if (sys != null && icon != null) {
                val px = runCatching { Ps2IconRenderer.render(icon, sys, tileW, tileH - 16, Ps2IconRenderer.Options()) }.getOrNull()
                if (px != null) sheet.setRGB(x, y, tileW, tileH - 16, px, 0, tileW)
            }
            g.color = Color.WHITE
            g.drawString(serial, x + 4, y + tileH - 4)
        }
        g.dispose()
        ImageIO.write(sheet, "png", file)
    }

    private companion object {
        /** A texture on the saves of this many different games is a tool's, not a game's. */
        const val BRAND_GAMES = 4

        /** Tools' textures seen on too few games to trip [BRAND_GAMES], found by eye in shared.png:
         *  the "Made With ps2 Save Builder" plate, a SharkPort disc and a second SharkPort cube. */
        val KNOWN_BRANDS = setOf("72da0d3a131d", "c4bf8efd880d", "4d557281b3bf")
    }

    private fun sha1(b: ByteArray): String =
        MessageDigest.getInstance("SHA-1").digest(b).joinToString("") { "%02x".format(it) }

    /** The first 964 bytes of icon.sys with the title blanked; the app shows the library's name. */
    private fun sanitized(sys: ByteArray): ByteArray =
        sys.copyOf(Ps2IconSys.SIZE).also { it.fill(0, 0xC0, 0xC0 + 68); it[6] = 0; it[7] = 0 }

    private fun regionOf(serial: String): Int = when (serial.substringBefore('-').uppercase()) {
        "SLUS", "SCUS" -> 1
        "SLES", "SCES", "SLED", "SCED" -> 2
        "SLPS", "SLPM", "SCPS", "SLKA", "SCKA", "SCAJ", "SLAJ" -> 3
        else -> 0
    }

    /** serial -> title key from GameIndex.yaml, the English name when it has one (as CoverRegionIndex). */
    private fun gameIndexTitles(file: File): Map<String, String> {
        val of = HashMap<String, String>(16384)
        var serial: String? = null; var name: String? = null; var nameEn: String? = null
        fun flush() {
            val s = serial ?: return
            val key = normalize(nameEn ?: name ?: "")
            if (key.isNotEmpty()) of[s] = key
            serial = null; name = null; nameEn = null
        }
        file.forEachLine { raw ->
            if (raw.isEmpty() || raw.startsWith('#')) return@forEachLine
            if (!raw[0].isWhitespace()) {
                flush()
                val s = raw.substringBefore(':').trim()
                if (s.length in 8..12 && s.getOrNull(4) == '-') serial = s.uppercase()
            } else if (serial != null) {
                val t = raw.trimStart()
                when {
                    t.startsWith("name-en:") -> nameEn = unquote(t.removePrefix("name-en:"))
                    t.startsWith("name:") -> name = unquote(t.removePrefix("name:"))
                }
            }
        }
        flush()
        return of
    }

    private fun unquote(v: String): String {
        var s = v.trim()
        if (s.startsWith("\"")) {
            val end = s.indexOf('"', 1)
            if (end > 0) return s.substring(1, end)
        }
        s = s.substringBefore('#').trim()
        return s.trim('"')
    }

    private fun normalize(title: String): String =
        title.lowercase().replace(Regex("[\\[(][^\\])]*[\\])]"), " ").replace(Regex("[^a-z0-9 ]"), " ").trim().replace(Regex("\\s+"), " ")

    // ---- save files -----------------------------------------------------------------------------

    private fun cString(b: ByteArray, at: Int, max: Int): String {
        var end = at
        while (end < at + max && end < b.size && b[end] != 0.toByte()) end++
        return String(b, at, end - at, Charsets.ISO_8859_1)
    }

    private fun le(b: ByteArray) = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)

    private fun folderSave(dir: File): Ps2Save =
        Ps2Save(dir.name, dir.lastModified(), (dir.listFiles() ?: emptyArray()).filter { it.isFile }.associate { f -> f.name to { f.readBytes() } })

    /** A PS3's export of a PS2 save: a 0x40 header, the PS2 part's own header, the folder, then a
     *  table of files with where each one sits in this file. Type 1 would be a PS1 save. */
    private fun psv(b: ByteArray): Ps2Save? {
        if (b.size < 0xA0 || b[0] != 0.toByte() || String(b, 1, 3, Charsets.US_ASCII) != "VSP") return null
        val bb = le(b)
        if (bb.getInt(0x3C) != 2) return null
        val files = HashMap<String, () -> ByteArray?>()
        for (i in 0 until bb.getInt(0x64).coerceIn(0, 4096)) {
            val e = 0xA0 + i * 0x3C
            if (e + 0x3C > b.size) break
            val size = bb.getInt(e + 0x10)
            val pos = bb.getInt(e + 0x38)
            if (size < 0 || pos < 0 || pos.toLong() + size > b.size) continue
            files[cString(b, e + 0x18, 32)] = { b.copyOfRange(pos, pos + size) }
        }
        return Ps2Save(cString(b, 0x80, 32), 0L, files)
    }

    /** uLaunchELF / EMS: the folder's 512-byte card entry, "." and "..", then each file's entry
     *  followed by its data padded to 1 KB. */
    private fun psu(b: ByteArray): Ps2Save? {
        if (b.size < 512 * 3) return null
        val bb = le(b)
        if (bb.getShort(0).toInt() and 0x20 == 0) return null
        val count = bb.getInt(4)
        val files = HashMap<String, () -> ByteArray?>()
        var off = 512
        var n = 0
        while (n < count && off + 512 <= b.size) {
            val mode = bb.getShort(off).toInt() and 0xFFFF
            val len = bb.getInt(off + 4)
            val name = cString(b, off + 0x40, 32)
            off += 512
            n++
            if (mode and 0x20 != 0) continue
            if (mode and 0x10 == 0 || len < 0 || off.toLong() + len > b.size) break
            val start = off
            files[name] = { b.copyOfRange(start, start + len) }
            off += (len + 1023) / 1024 * 1024
        }
        return Ps2Save(cString(b, 0x40, 32), 0L, files)
    }

    /** Action Replay Max: a 0x5C-byte header, then straight into the LZARI stream (its length is
     *  the header's figure less 4), which holds the files as (length, name, data) runs, each run's
     *  end padded so the next one starts 8 bytes before a 16-byte boundary. */
    private fun max(b: ByteArray): Ps2Save? {
        if (b.size < 0x60 || String(b, 0, 12, Charsets.US_ASCII) != "Ps2PowerSave") return null
        val bb = le(b)
        val clen = bb.getInt(0x50)
        val count = bb.getInt(0x54)
        val length = bb.getInt(0x58)
        if (length !in 0..(64 shl 20) || clen < 4) return null
        val data = Lzari(b, 0x5C, minOf(b.size, 0x5C + clen - 4)).decode(length)
        val files = HashMap<String, () -> ByteArray?>()
        var off = 0
        for (i in 0 until count) {
            if (off + 36 > data.size) break
            val len = le(data).getInt(off)
            val name = cString(data, off + 4, 32)
            off += 36
            if (len < 0 || off + len > data.size) break
            val start = off
            files[name] = { data.copyOfRange(start, start + len) }
            off += len
            off = (off + 8 + 15) / 16 * 16 - 8
        }
        return Ps2Save(cString(b, 0x10, 32), 0L, files)
    }

    /** Haruhiko Okumura's LZARI decoder (1989), which Action Replay Max compresses saves with. */
    private class Lzari(private val src: ByteArray, private var pos: Int, private val end: Int) {
        private var buffer = 0
        private var mask = 0
        private var low = 0L
        private var high = Q4
        private var value = 0L
        private val charToSym = IntArray(N_CHAR)
        private val symToChar = IntArray(N_CHAR + 1)
        private val symFreq = IntArray(N_CHAR + 1)
        private val symCum = IntArray(N_CHAR + 1)
        private val positionCum = IntArray(N + 1)

        fun decode(length: Int): ByteArray {
            val out = ByteArray(length)
            repeat(M + 2) { value = 2 * value + bit() }
            startModel()
            val text = ByteArray(N)
            text.fill(' '.code.toByte(), 0, N - F)
            var r = N - F
            var count = 0
            while (count < length) {
                val c = decodeChar()
                if (c < 256) {
                    out[count++] = c.toByte(); text[r] = c.toByte(); r = (r + 1) and (N - 1)
                } else {
                    val i = (r - decodePosition() - 1) and (N - 1)
                    for (k in 0 until c - 255 + THRESHOLD) {
                        if (count >= length) break
                        val ch = text[(i + k) and (N - 1)]
                        out[count++] = ch; text[r] = ch; r = (r + 1) and (N - 1)
                    }
                }
            }
            return out
        }

        private fun bit(): Int {
            mask = mask shr 1
            if (mask == 0) {
                buffer = if (pos < end) src[pos++].toInt() and 0xFF else 0
                mask = 128
            }
            return if (buffer and mask != 0) 1 else 0
        }

        private fun startModel() {
            symCum[N_CHAR] = 0
            for (sym in N_CHAR downTo 1) {
                val ch = sym - 1
                charToSym[ch] = sym; symToChar[sym] = ch
                symFreq[sym] = 1
                symCum[sym - 1] = symCum[sym] + symFreq[sym]
            }
            symFreq[0] = 0
            positionCum[N] = 0
            for (i in N downTo 1) positionCum[i - 1] = positionCum[i] + 10000 / (i + 200)
        }

        private fun updateModel(sym: Int) {
            if (symCum[0] >= MAX_CUM) {
                var c = 0
                for (i in N_CHAR downTo 1) {
                    symCum[i] = c
                    symFreq[i] = (symFreq[i] + 1) shr 1
                    c += symFreq[i]
                }
                symCum[0] = c
            }
            var i = sym
            while (symFreq[i] == symFreq[i - 1]) i--
            if (i < sym) {
                val chI = symToChar[i]; val chSym = symToChar[sym]
                symToChar[i] = chSym; symToChar[sym] = chI
                charToSym[chI] = sym; charToSym[chSym] = i
            }
            symFreq[i]++
            while (--i >= 0) symCum[i]++
        }

        private fun searchSym(x: Long): Int {
            var i = 1; var j = N_CHAR
            while (i < j) { val k = (i + j) / 2; if (symCum[k] > x) i = k + 1 else j = k }
            return i
        }

        private fun searchPos(x: Long): Int {
            var i = 1; var j = N
            while (i < j) { val k = (i + j) / 2; if (positionCum[k] > x) i = k + 1 else j = k }
            return i - 1
        }

        private fun decodeChar(): Int {
            val range = high - low
            val sym = searchSym(((value - low + 1) * symCum[0] - 1) / range)
            high = low + range * symCum[sym - 1] / symCum[0]
            low += range * symCum[sym] / symCum[0]
            renormalize()
            val ch = symToChar[sym]
            updateModel(sym)
            return ch
        }

        private fun decodePosition(): Int {
            val range = high - low
            val position = searchPos(((value - low + 1) * positionCum[0] - 1) / range)
            high = low + range * positionCum[position] / positionCum[0]
            low += range * positionCum[position + 1] / positionCum[0]
            renormalize()
            return position
        }

        private fun renormalize() {
            while (true) {
                if (low >= Q2) { value -= Q2; low -= Q2; high -= Q2 }
                else if (low >= Q1 && high <= Q3) { value -= Q1; low -= Q1; high -= Q1 }
                else if (high > Q2) break
                low += low; high += high
                value = 2 * value + bit()
            }
        }

        private companion object {
            const val N = 4096
            const val F = 60
            const val THRESHOLD = 2
            const val M = 15
            const val Q1 = 1L shl M
            const val Q2 = 2 * Q1
            const val Q3 = 3 * Q1
            const val Q4 = 4 * Q1
            const val MAX_CUM = Q1 - 1
            const val N_CHAR = 256 - THRESHOLD + F
        }
    }
}
