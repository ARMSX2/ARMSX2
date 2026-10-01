package com.armsx2.ui.home

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntSize
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.Ps2IconRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlin.math.PI
import kotlin.math.min

/**
 * A game's Memory Card Cover, moving: the icon turns slowly and plays its own animation, as the
 * PS2's memory card screen shows its saves. Drawn with the cover's own framing and shape, so it
 * lands exactly on the still picture; [onFirstFrame] tells the tile when it can hide that
 * picture, and [live], when given, gets every frame (the shelf's reflection shows it too).
 */
@Composable
internal fun MemcardAnimatedCover(
    serial: String,
    modifier: Modifier = Modifier,
    onFirstFrame: () -> Unit = {},
    live: MutableState<ImageBitmap?>? = null,
) {
    AnimatedPs2Icon(
        key = serial,
        load = { MemcardCovers.loadIcon(serial) },
        options = MemcardCovers.COVER_OPTIONS,
        aspect = MemcardCovers.COVER_H.toFloat() / MemcardCovers.COVER_W,
        maxWidth = 300,
        modifier = modifier,
        onFirstFrame = onFirstFrame,
        onFrame = { live?.value = it },
    )
}

/**
 * A moving cover again, for the shelf's reflection under it: the same frames, not a second
 * render. [onShown] says whether there is a frame to show, so the still picture can step aside.
 * Only this reads each new frame, so only this recomposes for it.
 */
@Composable
internal fun MemcardLiveMirror(live: State<ImageBitmap?>, modifier: Modifier = Modifier, onShown: (Boolean) -> Unit) {
    val frame = live.value
    val shown = frame != null
    val report by rememberUpdatedState(onShown)
    LaunchedEffect(shown) { report(shown) }
    frame?.let { Image(it, contentDescription = null, contentScale = ContentScale.Fit, modifier = modifier) }
}

/**
 * A PS2 save icon drawn live: playing its animation when [animate], and turning too when Spin is
 * on ([MemcardCovers.spin]), or one still frame when not, at [aspect] (height / width) fitted into
 * this composable. Drawn by the GPU ([IconGl]) when it can be, else by the software renderer. It
 * starts from the pose the loaded icon's still picture uses, and [onFirstFrame] says when the first
 * frame is on screen, so a still picture under it can step aside.
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
    if (IconGl.available.value) {
        GpuPs2Icon(key, load, options, aspect, modifier, animate, onFirstFrame)
    } else {
        SoftwarePs2Icon(key, load, options, aspect, maxWidth, modifier, animate, onFirstFrame, onFrame)
    }
}

/** The GPU's way: one [IconView] the GL thread draws into on the shared beat. */
@Composable
private fun GpuPs2Icon(
    key: Any,
    load: suspend () -> MemcardCovers.Loaded?,
    options: Ps2IconRenderer.Options,
    aspect: Float,
    modifier: Modifier,
    animate: Boolean,
    onFirstFrame: () -> Unit,
) {
    val loaded by produceState<MemcardCovers.Loaded?>(null, key) {
        value = withContext(Dispatchers.IO) { runCatching { load() }.getOrNull() }
    }
    val lifecycle by LocalLifecycleOwner.current.lifecycle.currentStateAsState()
    val visible = lifecycle.isAtLeast(Lifecycle.State.STARTED)
    LaunchedEffect(visible) { IconGl.setVisible(visible) }
    val firstFrame by rememberUpdatedState(onFirstFrame)
    Box(modifier, contentAlignment = Alignment.Center) {
        val ready = loaded ?: return@Box
        // Fitted the way the still picture under it is (ContentScale.Fit), so they line up.
        AndroidView(
            factory = { context -> IconView(context) },
            modifier = Modifier.aspectRatio(1f / aspect),
            update = { view -> view.bind(key, ready, options, animate) { firstFrame() } },
        )
    }
}

/**
 * The software renderer's way, for when the GPU's can't be used. A PS2 save icon drawn live: playing its animation when [animate], and turning too when Spin is
 * on ([MemcardCovers.spin]), or one still frame when not. Frames are rendered off the main thread
 * at this composable's width (capped at [maxWidth], since it runs every frame) and [aspect]
 * (height / width), into two bitmaps used in turn, reusing the renderer's buffers. It starts from
 * the pose the loaded icon's still picture uses, and holds still while the app is out of sight,
 * carrying on from there when it is back. An icon with no animation of its own that is not
 * turning never changes, so it is drawn once and costs nothing after.
 */
@Composable
private fun SoftwarePs2Icon(
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
    val lifecycle by LocalLifecycleOwner.current.lifecycle.currentStateAsState()
    val moving = animate && lifecycle.isAtLeast(Lifecycle.State.STARTED)
    val spinning = MemcardCovers.spin.value
    // Seconds of animation and of turning so far, kept when motion pauses so it carries on from
    // the same pose. With Spin off the icon faces the way its still picture does, its animation
    // playing on, and the turn starts again from there when Spin is back on.
    val clock = remember(key) { FloatArray(2) }
    val scratch = remember(key) { Ps2IconRenderer.Scratch() }
    LaunchedEffect(key, size, moving, spinning) {
        if (size.width <= 0 || size.height <= 0) return@LaunchedEffect
        if (!moving && frame != null) return@LaunchedEffect
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
        val still = !icon.animated && !spinning
        val start = System.nanoTime()
        val baseAnimation = clock[0]
        val baseTurn = clock[1]
        // The very first frame is drawn as smoothly as the still picture it replaces.
        var first = frame == null
        while (isActive) {
            val began = System.nanoTime()
            val elapsed = if (moving) (began - start) / 1e9f else 0f
            val animation = baseAnimation + elapsed
            val turn = if (spinning) baseTurn + elapsed else 0f
            clock[0] = animation
            clock[1] = turn
            val pose = Ps2IconRenderer.Pose(
                yaw = loaded.pose.yaw + turn * TURN_PER_SECOND,
                // At the console's 60 frames a second, looping, from the still picture's moment.
                time = if (icon.animated) (loaded.pose.time + animation * 60f * icon.animSpeed) % length else loaded.pose.time,
            )
            val bmp = bitmaps[which]
            which = which xor 1
            // Into the bitmap off the main thread too: the conversion to the bitmap's own format
            // was the main thread's biggest cost with a screenful moving. It is the bitmap not on
            // screen, the other of the two.
            withContext(Dispatchers.Default) {
                val px = if (first) Ps2IconRenderer.renderFrame(icon, loaded.sys, w, h, pose, options)
                    else Ps2IconRenderer.renderFrame(icon, loaded.sys, w, h, pose, frameOptions, scratch)
                bmp.setPixels(px, 0, w, 0, 0, w, h)
            }
            val image = bmp.asImageBitmap()
            frame = image
            eachFrame(image)
            if (first) { first = false; firstFrame() }
            if (!moving || still) break
            // Every moving icon draws on the same beat of one clock, so a screenful changes together
            // and the screen is redrawn once per beat. On beats of their own they changed at
            // different moments and kept an Odin 3 redrawing the whole library at 120 Hz.
            val now = android.os.SystemClock.uptimeMillis()
            delay((FRAME_MS - now % FRAME_MS).coerceAtLeast(1))
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
