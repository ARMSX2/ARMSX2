package com.armsx2.memcard

import org.junit.Assume
import org.junit.Test
import java.awt.Color
import java.awt.Font
import java.awt.RenderingHints
import java.awt.image.BufferedImage
import java.io.File
import javax.imageio.ImageIO

/**
 * Checks a built icon index before it is published: every icon in it must parse with the app's
 * own readers and render, and ICON_INDEX_SHOW=SLUS-20312,... draws those serials' icons to a sheet
 * to look at. Skipped unless ICON_INDEX_DIR (index.txt plus icons/) is set.
 */
class IconIndexDevTest {
    @Test
    fun everyIconParsesAndRenders() {
        val dir = System.getenv("ICON_INDEX_DIR")?.let(::File)
        Assume.assumeTrue(dir != null && File(dir, "index.txt").isFile)
        val index = File(dir, "index.txt").readLines().filter { it.isNotBlank() && !it.startsWith("#") }
            .associate { it.substringBefore(' ') to it.substringAfter(' ').trim() }
        val bad = ArrayList<String>()
        var animated = 0
        // Every icon file, not only the ones the index names: the rest are the Icon Museum's.
        val all = File(dir, "icons").listFiles().orEmpty().filter { it.name.endsWith(".bin") }.map { it.name.removeSuffix(".bin") }
        org.junit.Assert.assertTrue("icons the index names but the folder lacks", all.containsAll(index.values.toSet()))
        for (hash in all) {
            val bytes = File(dir, "icons/$hash.bin").takeIf { it.isFile }?.readBytes()
            val sys = bytes?.let { Ps2IconSys.parse(it.copyOfRange(0, minOf(it.size, Ps2IconSys.SIZE))) }
            val icon = bytes?.takeIf { it.size > Ps2IconSys.SIZE }?.let { Ps2Icon.parse(it.copyOfRange(Ps2IconSys.SIZE, it.size)) }
            if (sys == null || icon == null || icon.texture == null) { bad += "$hash: ${if (bytes == null) "missing" else if (sys == null) "icon.sys" else "icon"}"; continue }
            if (icon.animated) animated++
            runCatching { Ps2IconRenderer.render(icon, sys, 48, 64, Ps2IconRenderer.Options(time = icon.frameLength / 2f)) }
                .onFailure { bad += "$hash: render ${it.javaClass.simpleName} ${it.message}" }
        }
        println("ICON INDEX ${index.size} serials, ${index.values.toSet().size} icons indexed, ${all.size} in all, $animated animated, ${bad.size} bad")
        bad.take(20).forEach { println("  $it") }
        org.junit.Assert.assertTrue(bad.joinToString("\n"), bad.isEmpty())

        // ICON_INDEX_FOCUS=SLUS-21115,...: those icons large, at four angles by four moments.
        System.getenv("ICON_INDEX_FOCUS")?.split(',')?.map { it.trim() }?.filter { it in index }?.takeIf { it.isNotEmpty() }?.let { focus ->
            val size = 200
            val sheet = BufferedImage(4 * size, focus.size * 4 * size, BufferedImage.TYPE_INT_RGB)
            focus.forEachIndexed { n, serial ->
                val bytes = File(dir, "icons/${index[serial]}.bin").readBytes()
                val sys = Ps2IconSys.parse(bytes.copyOfRange(0, Ps2IconSys.SIZE))!!
                val icon = Ps2Icon.parse(bytes.copyOfRange(Ps2IconSys.SIZE, bytes.size))!!
                val len = icon.frameLength.coerceAtLeast(1).toFloat()
                for (a in 0 until 4) for (t in 0 until 4) {
                    val px = Ps2IconRenderer.render(icon, sys, size, size, Ps2IconRenderer.Options(yaw = a * 1.5708f, time = t * len / 4f))
                    sheet.setRGB(t * size, (n * 4 + a) * size, size, size, px, 0, size)
                }
            }
            ImageIO.write(sheet, "png", File(dir, "focus.png"))
        }

        val show = System.getenv("ICON_INDEX_SHOW")?.split(',')?.map { it.trim() }?.filter { it in index }.orEmpty()
        if (show.isEmpty()) return
        val tileW = 160; val tileH = 200; val cols = 8
        val rows = (show.size + cols - 1) / cols
        val sheet = BufferedImage(cols * tileW, rows * tileH, BufferedImage.TYPE_INT_RGB)
        val g = sheet.createGraphics()
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON)
        g.font = Font(Font.SANS_SERIF, Font.PLAIN, 12)
        show.forEachIndexed { i, serial ->
            val bytes = File(dir, "icons/${index[serial]}.bin").readBytes()
            val sys = Ps2IconSys.parse(bytes.copyOfRange(0, Ps2IconSys.SIZE))!!
            val icon = Ps2Icon.parse(bytes.copyOfRange(Ps2IconSys.SIZE, bytes.size))!!
            val px = Ps2IconRenderer.render(icon, sys, tileW, tileH - 18, Ps2IconRenderer.Options())
            sheet.setRGB((i % cols) * tileW, (i / cols) * tileH, tileW, tileH - 18, px, 0, tileW)
            g.color = Color.WHITE
            g.drawString(serial, (i % cols) * tileW + 4, (i / cols) * tileH + tileH - 5)
        }
        g.dispose()
        ImageIO.write(sheet, "png", File(dir, "show.png"))
    }
}
