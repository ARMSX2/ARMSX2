package com.armsx2.ui.home

import android.graphics.Bitmap
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.min
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import com.armsx2.EmuState
import com.armsx2.i18n.str
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.Ps2IconRenderer
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.settings.IntSliderRow
import com.armsx2.ui.settings.ToggleRow
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.random.Random

/**
 * The library's screensaver: after a set time with no input on the library (10 minutes unless
 * the player picks 1 to 60), the save icons take the screen one game at a time, each moving on
 * its own background with the game's name, drifting slowly so nothing sits still on an OLED
 * panel. Any input wakes it, and only wakes it: the press that does is not passed on, so it can't
 * also start a game. Only over the library, never over a game or while the app is hidden.
 *
 * Input is seen at the activity (see MainActivityRuntime's dispatch methods), before anything
 * else, which is the one place every key, stick and touch passes through.
 */
object LibraryScreensaver {
    private const val KEY_ENABLED = "library.screensaver"
    private const val KEY_MINUTES = "library.screensaver.minutes"
    const val DEFAULT_MINUTES = 10

    val enabled = mutableStateOf(true)
    val minutes = mutableIntStateOf(DEFAULT_MINUTES)

    /** On screen now. */
    val showing = mutableStateOf(false)

    @Volatile private var lastInput = SystemClock.uptimeMillis()

    // The key whose press woke it, so its release is swallowed too, and a touch that woke it.
    private var wakeKey = -1
    private var swallowingTouch = false
    private var loaded = false

    fun load() {
        if (loaded) return
        loaded = true
        enabled.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, true) }.getOrDefault(true)
        minutes.intValue = runCatching { MainActivityRuntime.prefs.getInt(KEY_MINUTES, DEFAULT_MINUTES) }
            .getOrDefault(DEFAULT_MINUTES).coerceIn(1, 60)
    }

    fun setEnabled(on: Boolean) {
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ENABLED, on).apply()
        enabled.value = on
    }

    fun setMinutes(value: Int) {
        val m = value.coerceIn(1, 60)
        MainActivityRuntime.prefs.edit().putInt(KEY_MINUTES, m).apply()
        minutes.intValue = m
    }

    fun idleMillis(): Long = SystemClock.uptimeMillis() - lastInput

    fun resetIdle() {
        lastInput = SystemClock.uptimeMillis()
    }

    /** Show it now, from its settings, to see it without waiting. */
    fun showNow() {
        resetIdle()
        showing.value = true
    }

    private fun wake() {
        showing.value = false
        resetIdle()
    }

    /** Every key event, first thing. True: it woke the screensaver (or is that press's release)
     *  and nothing else should see it. The volume keys neither wake it nor stop at it: they turn
     *  the volume, of its music too. */
    fun onKey(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_VOLUME_UP || event.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
            event.keyCode == KeyEvent.KEYCODE_VOLUME_MUTE
        ) return false
        resetIdle()
        if (showing.value) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                wakeKey = event.keyCode
                wake()
            }
            return true
        }
        if (event.action == KeyEvent.ACTION_UP && event.keyCode == wakeKey) {
            wakeKey = -1
            return true
        }
        return false
    }

    /** Every touch, first thing; the whole gesture that wakes it is swallowed. */
    fun onTouch(event: MotionEvent): Boolean {
        resetIdle()
        if (showing.value && event.actionMasked == MotionEvent.ACTION_DOWN) {
            swallowingTouch = true
            wake()
            return true
        }
        if (swallowingTouch) {
            if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL) {
                swallowingTouch = false
            }
            return true
        }
        return false
    }

    /** Sticks, triggers, a mouse: only a real push counts, not a resting stick's jitter. */
    fun onMotion(event: MotionEvent): Boolean {
        val real = event.isFromSource(InputDevice.SOURCE_CLASS_POINTER) ||
            AXES.any { abs(event.getAxisValue(it)) > 0.4f }
        if (!real) return showing.value
        resetIdle()
        if (showing.value) {
            wake()
            return true
        }
        return false
    }

    private val AXES = intArrayOf(
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
        MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS,
    )
}

/**
 * Starts the screensaver when the library has sat idle long enough, and shows it. Put in the
 * library screen; [titles] maps serials to the library's game names.
 */
@Composable
internal fun LibraryScreensaverHost(titles: () -> Map<String, String>) {
    remember { LibraryScreensaver.load() }
    val enabled = LibraryScreensaver.enabled.value
    val minutes = LibraryScreensaver.minutes.intValue
    val lifecycle by LocalLifecycleOwner.current.lifecycle.currentStateAsState()
    val visible = lifecycle.isAtLeast(Lifecycle.State.RESUMED)
    val noGame = MainActivityRuntime.eState.value == EmuState.STOPPED
    LaunchedEffect(enabled, minutes, visible, noGame) {
        if (!visible || !noGame) {
            LibraryScreensaver.showing.value = false
            return@LaunchedEffect
        }
        // Coming back to the library (from a game, from another app) starts the wait afresh.
        LibraryScreensaver.resetIdle()
        if (!enabled) return@LaunchedEffect
        while (isActive) {
            val left = minutes * 60_000L - LibraryScreensaver.idleMillis()
            if (left <= 0) {
                if (!LibraryScreensaver.showing.value) LibraryScreensaver.showing.value = true
                delay(1_000)
            } else {
                delay(left.coerceIn(250, 5_000))
            }
        }
    }
    if (LibraryScreensaver.showing.value && visible && noGame) Screensaver(titles)
}

// How long each game's icon holds the screen, and the fade between two.
private const val HOLD_MS = 9_000
private const val FADE_MS = 1_400
private const val DRIFT_STEP_MS = 42L // the moving icons' beat

@Composable
private fun Screensaver(titles: () -> Map<String, String>) {
    val context = LocalContext.current
    val icons by produceState<List<MemcardCovers.ShowIcon>?>(null) {
        value = withContext(Dispatchers.IO) { runCatching { MemcardCovers.showIcons(context, titles()).shuffled() }.getOrDefault(emptyList()) }
    }
    com.armsx2.ui.common.PadModal(
        key = "library-screensaver",
        onDismiss = { LibraryScreensaver.showing.value = false },
        scrimAlpha = 1f,
    ) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            val list = icons
            when {
                list == null -> Unit // black for the moment it takes to read the cards
                // No icons anywhere: nothing to show, so no screensaver.
                list.isEmpty() -> LaunchedEffect(Unit) { LibraryScreensaver.showing.value = false }
                else -> Slideshow(list)
            }
        }
    }
}

@Composable
private fun Slideshow(icons: List<MemcardCovers.ShowIcon>) {
    var index by remember { mutableIntStateOf(0) }
    LaunchedEffect(icons) {
        while (isActive) {
            delay(HOLD_MS.toLong())
            index = (index + 1) % icons.size
        }
    }
    Crossfade(targetState = index, animationSpec = tween(FADE_MS), label = "screensaver") { i ->
        SlideshowPage(icons[i], i)
    }
}

@Composable
private fun SlideshowPage(icon: MemcardCovers.ShowIcon, seed: Int) {
    val loaded by produceState<MemcardCovers.Loaded?>(null, icon.key) {
        value = withContext(Dispatchers.IO) { runCatching { icon.load() }.getOrNull() }
    }
    // The save's own background gradient, dimmed: this is a screensaver, not the viewer.
    val background by produceState<ImageBitmap?>(null, loaded) {
        val sys = loaded?.sys ?: return@produceState
        value = withContext(Dispatchers.Default) {
            Bitmap.createBitmap(Ps2IconRenderer.backgroundPixels(sys, 32, 32), 32, 32, Bitmap.Config.ARGB_8888).asImageBitmap()
        }
    }
    // Each icon drifts across part of the screen while it is up, from and to a random spot, so
    // nothing burns in and it never looks like a still.
    val random = remember(seed, icon.key) { Random(icon.key.hashCode() * 31 + seed) }
    val from = remember(seed, icon.key) { floatArrayOf(random.nextFloat() - 0.5f, random.nextFloat() - 0.5f) }
    val to = remember(seed, icon.key) { floatArrayOf(random.nextFloat() - 0.5f, random.nextFloat() - 0.5f) }
    // Stepped on the icons' beat rather than animated every refresh: it moves a few pixels a second,
    // and a per-refresh animation kept the whole screen redrawing at the panel's 120 Hz for it.
    var drift by remember(seed, icon.key) { mutableFloatStateOf(0f) }
    LaunchedEffect(seed, icon.key) {
        val start = SystemClock.uptimeMillis()
        val total = (HOLD_MS + 2 * FADE_MS).toFloat()
        while (isActive && drift < 1f) {
            val now = SystemClock.uptimeMillis()
            drift = ((now - start) / total).coerceAtMost(1f)
            delay(DRIFT_STEP_MS - now % DRIFT_STEP_MS)
        }
    }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        background?.let {
            Image(it, contentDescription = null, contentScale = ContentScale.FillBounds, modifier = Modifier.fillMaxSize().alpha(0.45f))
        }
        val t = drift
        val dx = maxWidth * 0.36f * (from[0] + (to[0] - from[0]) * t)
        val dy = maxHeight * 0.22f * (from[1] + (to[1] - from[1]) * t)
        val side = min(maxWidth, maxHeight) * 0.56f
        Column(
            Modifier.align(Alignment.Center).offset(dx, dy),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            val ready = loaded
            Box(Modifier.size(side)) {
                if (ready != null) {
                    AnimatedPs2Icon(
                        key = icon.key,
                        load = { ready },
                        options = Ps2IconRenderer.Options(background = false, fill = 0.9f),
                        aspect = 1f,
                        maxWidth = 480,
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
            Text(
                icon.title,
                color = Color.White.copy(alpha = 0.82f),
                fontSize = 22.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.widthIn(max = side * 1.6f),
            )
        }
    }
}

/** The screensaver's settings, from the library menu: on or off, how long before it starts, and
 *  a way to see it now. */
@Composable
internal fun ScreensaverSettings(onClose: () -> Unit) {
    remember { LibraryScreensaver.load() }
    val enabled = LibraryScreensaver.enabled.value
    val minutes = LibraryScreensaver.minutes.intValue
    val minutesFormat = str("screensaver.minutes")
    com.armsx2.ui.common.PadModal(key = "library-screensaver-settings", onDismiss = onClose, initialFocusId = "library-screensaver-settings.ok") {
        Surface(
            modifier = Modifier.padding(24.dp).widthIn(max = 520.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(22.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(str("screensaver.title"), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text(
                    str("screensaver.body"),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                ToggleRow(label = str("screensaver.enabled"), value = enabled) { LibraryScreensaver.setEnabled(it) }
                IntSliderRow(
                    label = str("screensaver.after"),
                    value = minutes,
                    min = 1,
                    max = 60,
                    valueFormatter = { minutesFormat.replace("%d", it.toString()) },
                    onReset = if (minutes != LibraryScreensaver.DEFAULT_MINUTES) {
                        { LibraryScreensaver.setMinutes(LibraryScreensaver.DEFAULT_MINUTES) }
                    } else null,
                ) { LibraryScreensaver.setMinutes(it) }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    TextButton(
                        onClick = { onClose(); LibraryScreensaver.showNow() },
                        modifier = Modifier.controllerFocusable(controllerId = "library-screensaver-settings.try", onConfirm = { onClose(); LibraryScreensaver.showNow() }),
                    ) { Text(str("screensaver.try")) }
                    TextButton(
                        onClick = onClose,
                        modifier = Modifier.controllerFocusable(controllerId = "library-screensaver-settings.ok", onConfirm = onClose),
                    ) { Text(str("action.ok")) }
                }
            }
        }
    }
}
