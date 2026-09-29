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

    /** MEMCARD_TEST_FOCUS=folder1,folder2: those saves at 0/90/180/270 degrees x 4 animation
     *  times, one row per save and angle, to study facing and animation. */
    @Test
    fun focus() {
        val cards = System.getenv("MEMCARD_TEST_CARDS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()
        val out = System.getenv("MEMCARD_TEST_OUT")?.let(::File)
        val focus = System.getenv("MEMCARD_TEST_FOCUS")?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() }.orEmpty()
        Assume.assumeTrue(cards.isNotEmpty() && out != null && focus.isNotEmpty())
        val tiles = ArrayList<IntArray>()
        var rows = 0
        for (path in cards) Ps2MemoryCard.open(File(path))?.use { card ->
            for (save in card.saves().filter { it.folder in focus }) {
                val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: continue
                val icon = Ps2Icon.parse(save.read(sys.iconNormal)) ?: continue
                val len = icon.frameLength.coerceAtLeast(1).toFloat()
                for (yaw in floatArrayOf(0f, 1.5708f, 3.1416f, 4.7124f)) {
                    rows++
                    for (t in floatArrayOf(0f, 0.25f, 0.5f, 0.75f)) {
                        tiles += Ps2IconRenderer.render(icon, sys, tileW, tileH, Ps2IconRenderer.Options(yaw = yaw, time = t * len))
                    }
                }
            }
        }
        val sheet = BufferedImage(4 * tileW, rows * tileH, BufferedImage.TYPE_INT_RGB)
        tiles.forEachIndexed { i, px -> sheet.setRGB((i % 4) * tileW, (i / 4) * tileH, tileW, tileH, px, 0, tileW) }
        ImageIO.write(sheet, "png", File(out, "focus.png"))
    }

    /** MEMCARD_TEST_FILM=folder1,folder2: 8 moments across each save's animation, front view. */
    @Test
    fun filmstrip() {
        val cards = System.getenv("MEMCARD_TEST_CARDS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()
        val out = System.getenv("MEMCARD_TEST_OUT")?.let(::File)
        val film = System.getenv("MEMCARD_TEST_FILM")?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() }.orEmpty()
        Assume.assumeTrue(cards.isNotEmpty() && out != null && film.isNotEmpty())
        val tiles = ArrayList<IntArray>()
        for (path in cards) Ps2MemoryCard.open(File(path))?.use { card ->
            val byName = card.saves().associateBy { it.folder }
            for (name in film) {
                val save = byName[name] ?: continue
                val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: continue
                val icon = Ps2Icon.parse(save.read(sys.iconNormal)) ?: continue
                val len = icon.frameLength.coerceAtLeast(1).toFloat()
                for (k in 0 until 8) {
                    tiles += Ps2IconRenderer.render(icon, sys, tileW, tileH, Ps2IconRenderer.Options(yaw = -0.35f, time = len * k / 8f))
                }
            }
        }
        val rows = tiles.size / 8
        val sheet = BufferedImage(8 * tileW, rows * tileH, BufferedImage.TYPE_INT_RGB)
        tiles.forEachIndexed { i, px -> sheet.setRGB((i % 8) * tileW, (i / 8) * tileH, tileW, tileH, px, 0, tileW) }
        ImageIO.write(sheet, "png", File(out, "film.png"))
    }

    /** Frames drawn with a reused Scratch (buffers and framing kept between frames) are the same
     *  pixels as frames drawn fresh, and cost less. */
    @Test
    fun scratchFramesMatchFreshOnes() {
        val cards = System.getenv("MEMCARD_TEST_CARDS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()
        Assume.assumeTrue(cards.isNotEmpty())
        val opts = Ps2IconRenderer.Options(background = false, anchorBottom = true, supersample = 1)
        var icons = 0; var frames = 0; var freshNs = 0L; var reusedNs = 0L
        for (path in cards) Ps2MemoryCard.open(File(path))?.use { card ->
            for (save in card.saves()) {
                val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: continue
                val icon = Ps2Icon.parse(save.read(sys.iconNormal)) ?: continue
                icons++
                val scratch = Ps2IconRenderer.Scratch()
                for (k in 0 until 12) {
                    val pose = Ps2IconRenderer.Pose(k * 0.5f, k * 3f)
                    var t = System.nanoTime()
                    val fresh = Ps2IconRenderer.renderFrame(icon, sys, 240, 333, pose, opts)
                    freshNs += System.nanoTime() - t
                    t = System.nanoTime()
                    val reused = Ps2IconRenderer.renderFrame(icon, sys, 240, 333, pose, opts, scratch)
                    reusedNs += System.nanoTime() - t
                    org.junit.Assert.assertArrayEquals("${save.folder} frame $k", fresh, reused)
                    frames++
                }
            }
        }
        println("SCRATCH $icons icons, $frames frames identical; fresh %.2f ms/frame, reused %.2f ms/frame".format(freshNs / 1e6 / frames, reusedNs / 1e6 / frames))
    }

    @Test
    fun contactSheet() {
        val cards = System.getenv("MEMCARD_TEST_CARDS")?.split(':')?.filter { it.isNotBlank() }.orEmpty()
        val out = System.getenv("MEMCARD_TEST_OUT")?.let(::File)
        Assume.assumeTrue(cards.isNotEmpty() && out != null)
        out!!.mkdirs()
        val variant = System.getenv("MEMCARD_TEST_VARIANT") ?: "default"
        val options = when (variant) {
            "front" -> Ps2IconRenderer.Options(yaw = 0f, time = 0f)
            "nofloor" -> Ps2IconRenderer.Options(ambientFloor = 0f)
            "cover" -> Ps2IconRenderer.Options(background = false, anchorBottom = true)
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
                    log.appendLine("    #${tiles.size - 1} ${Ps2IconRenderer.lastPick}")
                }
            }
        }
        val ms = (System.nanoTime() - started) / 1_000_000
        log.appendLine("saves=$saves icon.sys=$withSys icons=$withIcon animated=$animated untextured=$noTexture in ${ms}ms")
        File(out, "summary-$variant.txt").writeText(log.toString())

        val cols = 12
        val rows = (tiles.size + cols - 1) / cols
        if (rows == 0) return
        val sheet = BufferedImage(cols * tileW, rows * tileH, BufferedImage.TYPE_INT_ARGB)
        tiles.forEachIndexed { i, px -> sheet.setRGB((i % cols) * tileW, (i / cols) * tileH, tileW, tileH, px, 0, tileW) }
        ImageIO.write(sheet, "png", File(out, "sheet-$variant.png"))
    }
}
