package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.awt.image.BufferedImage
import java.io.File
import javax.imageio.ImageIO

/**
 * JVM tests for when the screenshot hotkey copies the core's PNG into the gallery: only once the core
 * has finished writing it. A file it is still writing, cut off anywhere, does not count.
 */
class ScreenshotsTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private fun png(): File =
        tmp.newFile("shot.png").also { ImageIO.write(BufferedImage(64, 48, BufferedImage.TYPE_INT_ARGB), "png", it) }

    @Test
    fun aFinishedPng() {
        assertTrue(Screenshots.isCompletePng(png()))
    }

    @Test
    fun aPngStillBeingWritten() {
        val file = png()
        val bytes = file.readBytes()
        // One byte short, everything but the IEND chunk, half, and just the signature.
        for (cut in listOf(bytes.size - 1, bytes.size - 12, bytes.size / 2, 8)) {
            file.writeBytes(bytes.copyOf(cut))
            assertFalse("cut at $cut of ${bytes.size}", Screenshots.isCompletePng(file))
        }
    }

    @Test
    fun noFileOrAnEmptyOne() {
        assertFalse(Screenshots.isCompletePng(File(tmp.root, "missing.png")))
        assertFalse(Screenshots.isCompletePng(tmp.newFile("empty.png")))
    }
}
