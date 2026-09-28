package com.armsx2.ui.home

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntSize
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.Ps2IconRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlin.math.PI
import kotlin.math.min

/**
 * The selected game's Memory Card Cover, moving: the icon turns slowly and plays its own
 * animation, as the PS2's memory card screen shows the save under the cursor.
 *
 * Drawn over the still cover, which stays visible until the first frame is ready. Frames are
 * rendered off the main thread at the tile's size (capped, since this runs every frame) into two
 * bitmaps used in turn. It starts from the still picture's own pose, so taking the selection
 * doesn't jump.
 */
@Composable
internal fun MemcardAnimatedCover(serial: String, modifier: Modifier = Modifier) {
    var frame by remember(serial) { mutableStateOf<ImageBitmap?>(null) }
    var size by remember { mutableStateOf(IntSize.Zero) }
    LaunchedEffect(serial, size) {
        if (size.width <= 0 || size.height <= 0) return@LaunchedEffect
        val loaded = withContext(Dispatchers.IO) { MemcardCovers.loadIcon(serial) } ?: return@LaunchedEffect
        val w = min(size.width, MAX_WIDTH)
        val h = (w.toFloat() * size.height / size.width).toInt().coerceAtLeast(1)
        val bitmaps = arrayOf(
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888),
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888),
        )
        var which = 0
        val icon = loaded.icon
        val length = icon.frameLength.coerceAtLeast(1).toFloat()
        val options = Ps2IconRenderer.Options(supersample = 1)
        val start = System.nanoTime()
        while (isActive) {
            val began = System.nanoTime()
            val seconds = (began - start) / 1e9f
            val pose = Ps2IconRenderer.Pose(
                yaw = loaded.pose.yaw + seconds * TURN_PER_SECOND,
                // At the console's 60 frames a second, looping, from the still picture's moment.
                time = if (icon.animated) (loaded.pose.time + seconds * 60f * icon.animSpeed) % length else loaded.pose.time,
            )
            val px = withContext(Dispatchers.Default) { Ps2IconRenderer.renderFrame(icon, loaded.sys, w, h, pose, options) }
            val bmp = bitmaps[which]
            which = which xor 1
            bmp.setPixels(px, 0, w, 0, 0, w, h)
            frame = bmp.asImageBitmap()
            val spentMs = (System.nanoTime() - began) / 1_000_000
            delay((FRAME_MS - spentMs).coerceAtLeast(1))
        }
    }
    Box(modifier.onSizeChanged { size = it }) {
        frame?.let {
            Image(it, contentDescription = null, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize())
        }
    }
}

private const val MAX_WIDTH = 360
private const val FRAME_MS = 42L // about 24 frames a second
// A full turn every six seconds, the unhurried spin of the console's own browser.
private const val TURN_PER_SECOND = (2.0 * PI / 6.0).toFloat()
