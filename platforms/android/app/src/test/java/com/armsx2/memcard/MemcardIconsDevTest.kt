package com.armsx2.memcard

import org.junit.Assume
import org.junit.Test
import java.awt.image.BufferedImage
import java.io.File
import javax.imageio.ImageIO

/**
 * Renders every save icon on real memory cards into a contact sheet, to judge the parser and the
 * renderer by eye. Skipped unless MEMCARD_TEST_CARDS (card paths, ':'-separated) and
 * MEMCARD_TEST_OUT (a directory) are set: the cards are the user's own saves and never live in
 * the repository.
 */
class MemcardIconsDevTest {
    private val tileW = 144
    private val tileH = 206

    @Test
    fun contactSheet() {
        val cards = System.getenv("MEMCARD_TEST_CARDS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()
        val out = System.getenv("MEMCARD_TEST_OUT")?.let(::File)
        Assume.assumeTrue(cards.isNotEmpty() && out != null)
        out!!.mkdirs()
        val variant = System.getenv("MEMCARD_TEST_VARIANT") ?: "default"
        val options = when (variant) {
            "front" -> Ps2IconRenderer.Options(yaw = 0f)
            "nofloor" -> Ps2IconRenderer.Options(ambientFloor = 0f)
            else -> Ps2IconRenderer.Options()
        }
        val log = StringBuilder()
        val tiles = ArrayList<IntArray>()
        var saves = 0; var withSys = 0; var withIcon = 0; var animated = 0; var noTexture = 0
        val started = System.nanoTime()
        for (path in cards) {
            val card = Ps2MemoryCard.open(File(path))
            if (card == null) { log.appendLine("$path: not a readable card"); continue }
            card.use {
                for (save in it.saves()) {
                    saves++
                    val sys = Ps2IconSys.parse(save.read("icon.sys"))
                    if (sys == null) { log.appendLine("${save.folder}: no icon.sys"); continue }
                    withSys++
                    val icon = Ps2Icon.parse(save.read(sys.iconNormal))
                    if (icon == null) { log.appendLine("${save.folder}: icon '${sys.iconNormal}' unreadable"); continue }
                    withIcon++
                    if (icon.animated) animated++
                    if (icon.texture == null) noTexture++
                    log.appendLine("${save.folder} [${save.serial}] '${sys.title}' shapes=${icon.shapeCount} " +
                        "verts=${icon.vertexCount} frames=${icon.frames.size} len=${icon.frameLength} tex=${icon.texture != null}")
                    tiles += Ps2IconRenderer.render(icon, sys, tileW, tileH, options)
                }
            }
        }
        val ms = (System.nanoTime() - started) / 1_000_000
        log.appendLine("saves=$saves icon.sys=$withSys icons=$withIcon animated=$animated untextured=$noTexture in ${ms}ms")
        File(out, "summary-$variant.txt").writeText(log.toString())

        val cols = 12
        val rows = (tiles.size + cols - 1) / cols
        if (rows == 0) return
        val sheet = BufferedImage(cols * tileW, rows * tileH, BufferedImage.TYPE_INT_RGB)
        tiles.forEachIndexed { i, px -> sheet.setRGB((i % cols) * tileW, (i / cols) * tileH, tileW, tileH, px, 0, tileW) }
        ImageIO.write(sheet, "png", File(out, "sheet-$variant.png"))
    }
}
