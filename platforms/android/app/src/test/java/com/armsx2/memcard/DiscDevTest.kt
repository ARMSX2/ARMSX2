package com.armsx2.memcard

import org.junit.Assume
import org.junit.Test
import java.io.File
import java.io.RandomAccessFile

/**
 * Checks the disc readers against real disc images: MEMCARD_TEST_DISCS names them (':'-separated).
 * Skipped without it: the images are the user's own games and never live in the repository.
 */
class DiscDevTest {
    private fun discs() = System.getenv("MEMCARD_TEST_DISCS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()

    private fun open(path: String): DiscImage? {
        val raf = RandomAccessFile(File(path), "r")
        return DiscImage.open(ChannelSource(raf.channel) { raf.close() })
    }

    /** Every hunk of every CHD decoded and checked against its stored CRC. */
    @Test
    fun chdHunksMatchTheirCrcs() {
        val discs = discs().filter { it.endsWith(".chd", ignoreCase = true) }
        Assume.assumeTrue(discs.isNotEmpty() && System.getenv("MEMCARD_TEST_CHD_VERIFY") != null)
        for (path in discs) {
            val chd = open(path) as? ChdDisc ?: error("$path: not a readable CHD")
            chd.use {
                var ok = 0; var bad = 0; var unknown = 0; var failed = 0
                val firstBad = ArrayList<String>()
                val t0 = System.nanoTime()
                for (h in 0 until it.hunkCount) {
                    when (runCatching { it.verifyHunk(h) }.getOrElse { e -> failed++; if (firstBad.size < 5) firstBad += "hunk $h: ${e.message}"; false.also { bad-- } }) {
                        true -> ok++
                        false -> { bad++; if (firstBad.size < 5) firstBad += "hunk $h: CRC mismatch" }
                        null -> unknown++
                    }
                }
                val ms = (System.nanoTime() - t0) / 1_000_000
                println("CHD ${File(path).name}: ${it.hunkCount} hunks ${it.mapStats()} -> ok=$ok bad=$bad unverifiable=$unknown failed=$failed in ${ms}ms")
                firstBad.forEach { println("CHD    $it") }
            }
        }
    }

    /** The disc-icon search on each image: where it found the icon, what it read, how long. */
    @Test
    fun findsIcons() {
        val discs = discs()
        Assume.assumeTrue(discs.isNotEmpty())
        val out = System.getenv("MEMCARD_TEST_OUT")?.let(::File)?.apply { mkdirs() }
        for (path in discs) {
            val disc = open(path) ?: run { println("ICON ${File(path).name}: not a readable disc"); continue }
            disc.use {
                val stats = DiscIcons.Stats()
                val t0 = System.nanoTime()
                val found = runCatching { DiscIcons.find(it, stats) }.onFailure { e -> println("ICON ${File(path).name}: failed ${e}") }.getOrNull()
                val ms = (System.nanoTime() - t0) / 1_000_000
                val icon = found?.let { f -> Ps2Icon.parse(f.icon) }
                println("ICON %-40s %s read %.1f MB, %d files, %d probes, step %d, %d ms%s".format(
                    File(path).name.take(40), if (found != null) "FOUND" else "none ", it.bytesRead / 1e6, stats.files, stats.probes,
                    stats.step, ms, if (found != null) "  ${found.where} shapes=${icon?.shapeCount} verts=${icon?.vertexCount}" else ""))
                if (found != null && out != null) File(out, File(path).nameWithoutExtension + ".icn").writeBytes(found.icon)
            }
        }
    }

    /** MEMCARD_TEST_ICN_DIR: every NAME.icn there (with NAME.sys when present) drawn as a cover. */
    @Test
    fun rendersIconFiles() {
        val dir = System.getenv("MEMCARD_TEST_ICN_DIR")?.let(::File)
        Assume.assumeTrue(dir != null && dir.isDirectory)
        val icns = dir!!.listFiles { f -> f.name.endsWith(".icn") }!!.sortedBy { it.name }
        val w = 144; val h = 206; val cols = 8
        val sheet = java.awt.image.BufferedImage(cols * w, ((icns.size + cols - 1) / cols) * h, java.awt.image.BufferedImage.TYPE_INT_ARGB)
        icns.forEachIndexed { i, f ->
            val icon = Ps2Icon.parse(f.readBytes()) ?: return@forEachIndexed println("BAD ${f.name}")
            val sys = File(dir, f.nameWithoutExtension + ".sys").takeIf { it.isFile }?.let { Ps2IconSys.parse(it.readBytes()) }
            val px = Ps2IconRenderer.render(icon, sys, w, h, Ps2IconRenderer.Options(background = false, anchorBottom = true))
            sheet.setRGB((i % cols) * w, (i / cols) * h, w, h, px, 0, w)
            println("ICN #$i ${f.nameWithoutExtension} shapes=${icon.shapeCount} verts=${icon.vertexCount} sys=${sys?.title}")
        }
        javax.imageio.ImageIO.write(sheet, "png", File(dir, "sheet.png"))
    }

    /** The volume descriptor and root directory read through each image. */
    @Test
    fun readsTheFileSystem() {
        val discs = discs()
        Assume.assumeTrue(discs.isNotEmpty())
        for (path in discs) {
            val disc = open(path) ?: error("$path: not a readable disc")
            disc.use {
                val pvd = it.read(16, 1)
                val volume = String(pvd, 40, 32, Charsets.ISO_8859_1).trim()
                println("DISC ${File(path).name}: ${it.sectorCount} sectors, volume '$volume', ${it.javaClass.simpleName}")
            }
        }
    }
}
