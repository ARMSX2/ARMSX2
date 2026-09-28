package com.armsx2.ui.home

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
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
 * animation, as the PS2's memory card screen shows the save under the cursor. Drawn with the
 * cover's own framing and shape, so it lands exactly on the still picture; [onFirstFrame] tells
 * the tile when it can hide that picture.
 */
@Composable
internal fun MemcardAnimatedCover(serial: String, modifier: Modifier = Modifier, onFirstFrame: () -> Unit = {}) {
    DisposableEffect(serial) {
        onDispose { if (MemcardLiveFrame.current.value?.first == serial) MemcardLiveFrame.current.value = null }
    }
    AnimatedPs2Icon(
        key = serial,
        load = { MemcardCovers.loadIcon(serial) },
        options = MemcardCovers.COVER_OPTIONS,
        aspect = MemcardCovers.COVER_H.toFloat() / MemcardCovers.COVER_W,
        maxWidth = 360,
        modifier = modifier,
        onFirstFrame = onFirstFrame,
        onFrame = { MemcardLiveFrame.current.value = serial to it },
    )
}

/** The frame the moving cover is showing, and whose it is: one tile moves at a time. */
internal object MemcardLiveFrame {
    val current = mutableStateOf<Pair<String, ImageBitmap>?>(null)
}

/**
 * The moving cover again, for the shelf's reflection under it: the same frame, not a second
 * render. [onShown] says whether there is a frame to show, so the still picture can step aside.
 */
@Composable
internal fun MemcardLiveMirror(serial: String, modifier: Modifier = Modifier, onShown: (Boolean) -> Unit) {
    val live = MemcardLiveFrame.current.value?.takeIf { it.first == serial }?.second
    val shown = live != null
    val report by rememberUpdatedState(onShown)
    LaunchedEffect(shown) { report(shown) }
    live?.let { Image(it, contentDescription = null, contentScale = ContentScale.Fit, modifier = modifier) }
}

/**
 * A PS2 save icon drawn live: turning and playing its animation when [animate], or one still
 * frame when not. Frames are rendered off the main thread at this composable's width (capped at
 * [maxWidth], since it runs every frame) and [aspect] (height / width), into two bitmaps used in
 * turn. It starts from the pose the loaded icon's still picture uses.
 */
@Composable
internal fun AnimatedPs2Icon(
    key: Any,
    load: suspend () -> MemcardCovers.Loaded?,
    options: Ps2IconRenderer.Options,
    aspect: Float,
    maxWidth: Int,
    modifier: Modifier = Modifier,
    animate: Boolean = true,
    onFirstFrame: () -> Unit = {},
    onFrame: (ImageBitmap) -> Unit = {},
) {
    var frame by remember(key) { mutableStateOf<ImageBitmap?>(null) }
    var size by remember { mutableStateOf(IntSize.Zero) }
    val firstFrame by rememberUpdatedState(onFirstFrame)
    val eachFrame by rememberUpdatedState(onFrame)
    LaunchedEffect(key, size, animate) {
        if (size.width <= 0 || size.height <= 0) return@LaunchedEffect
        val loaded = withContext(Dispatchers.IO) { load() } ?: return@LaunchedEffect
        val w = min(size.width, maxWidth)
        val h = (w * aspect).toInt().coerceAtLeast(1)
        val bitmaps = arrayOf(
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888),
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888),
        )
        var which = 0
        val icon = loaded.icon
        val length = icon.frameLength.coerceAtLeast(1).toFloat()
        val frameOptions = options.copy(supersample = 1)
        val start = System.nanoTime()
        var first = true
        while (isActive) {
            val began = System.nanoTime()
            val seconds = if (animate) (began - start) / 1e9f else 0f
            val pose = Ps2IconRenderer.Pose(
                yaw = loaded.pose.yaw + seconds * TURN_PER_SECOND,
                // At the console's 60 frames a second, looping, from the still picture's moment.
                time = if (icon.animated) (loaded.pose.time + seconds * 60f * icon.animSpeed) % length else loaded.pose.time,
            )
            val px = withContext(Dispatchers.Default) {
                Ps2IconRenderer.renderFrame(icon, loaded.sys, w, h, pose, if (first) options else frameOptions)
            }
            val bmp = bitmaps[which]
            which = which xor 1
            bmp.setPixels(px, 0, w, 0, 0, w, h)
            val image = bmp.asImageBitmap()
            frame = image
            eachFrame(image)
            if (first) { first = false; firstFrame() }
            if (!animate) break
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

private const val FRAME_MS = 42L // about 24 frames a second
// A full turn every six seconds, the unhurried spin of the console's own browser.
private const val TURN_PER_SECOND = (2.0 * PI / 6.0).toFloat()
