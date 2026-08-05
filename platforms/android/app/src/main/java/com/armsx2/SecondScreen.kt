package com.armsx2

import android.app.Presentation
import android.content.Context
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.compose.runtime.mutableStateOf
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Utility panel on a SECOND display — Ayn Thor, the Retroid dual-screen add-on, or anything else
 * Android reports as an extra display (requested by Mike22). Shows live stats and the actions you
 * otherwise have to pause the game to reach.
 *
 * ★ Built from plain Views, not Compose, on purpose. A [Presentation] is its own Window with its
 * own decor view, and a ComposeView inside one only works after the ViewTree lifecycle/saved-state
 * owners are attached to that decor view — get it wrong and it throws at inflate time, on hardware
 * almost nobody testing this has. A handful of buttons does not justify that risk.
 *
 * Everything it calls is already thread-safe and already used by the on-screen equivalents, so the
 * panel adds no new emulator surface — it is a second set of buttons for existing actions.
 */
object SecondScreen {

    private const val PREF_KEY = "secondScreen.enabled"
    private const val TICK_MS = 500L

    /** User toggle (App settings). Default ON: with no second display attached it does nothing. */
    val enabled = mutableStateOf(true)

    private var presentation: Panel? = null
    private var listener: DisplayManager.DisplayListener? = null
    private val handler = Handler(Looper.getMainLooper())

    fun load() {
        runCatching { enabled.value = MainActivityRuntime.prefs.getBoolean(PREF_KEY, true) }
    }

    fun set(context: Context, value: Boolean) {
        enabled.value = value
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_KEY, value).apply() }
        if (value) attach(context) else detach()
    }

    /** Start watching for a second display and show the panel on one if present. */
    fun attach(context: Context) {
        if (!enabled.value) return
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager ?: return
        if (listener == null) {
            val l = object : DisplayManager.DisplayListener {
                override fun onDisplayAdded(displayId: Int) = refresh(context)
                override fun onDisplayRemoved(displayId: Int) = refresh(context)
                override fun onDisplayChanged(displayId: Int) = Unit
            }
            runCatching { dm.registerDisplayListener(l, handler) }.onSuccess { listener = l }
        }
        refresh(context)
    }

    fun detach() {
        runCatching { presentation?.dismiss() }
        presentation = null
    }

    /** Fully release (activity destroy). */
    fun release(context: Context) {
        detach()
        listener?.let { l ->
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
            runCatching { dm?.unregisterDisplayListener(l) }
        }
        listener = null
    }

    private fun secondaryDisplay(context: Context): Display? {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager ?: return null
        // PRESENTATION category is the one Android intends for this; fall back to "any display that
        // isn't the built-in one" because some handhelds don't tag their second panel.
        val presentationDisplays = runCatching {
            dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        }.getOrNull()
        if (!presentationDisplays.isNullOrEmpty()) return presentationDisplays.first()
        return runCatching {
            dm.displays?.firstOrNull { it.displayId != Display.DEFAULT_DISPLAY }
        }.getOrNull()
    }

    private fun refresh(context: Context) {
        if (!enabled.value) { detach(); return }
        val target = secondaryDisplay(context)
        if (target == null) { detach(); return }
        // Already showing on this display? Leave it alone.
        presentation?.let { if (it.display?.displayId == target.displayId && it.isShowing) return }
        detach()
        runCatching {
            val p = Panel(context, target)
            p.show()
            presentation = p
        }
    }

    /** The panel itself. */
    private class Panel(context: Context, display: Display) : Presentation(context, display) {

        private lateinit var stats: TextView
        private var ticking = false
        private val tick = object : Runnable {
            override fun run() {
                if (!ticking) return
                updateStats()
                handler.postDelayed(this, TICK_MS)
            }
        }

        override fun onCreate(savedInstanceState: Bundle?) {
            super.onCreate(savedInstanceState)
            val pad = (resources.displayMetrics.density * 12).toInt()
            val rootView = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(Color.BLACK)
                setPadding(pad, pad, pad, pad)
            }

            stats = TextView(context).apply {
                setTextColor(Color.WHITE)
                textSize = 16f
                gravity = Gravity.CENTER_HORIZONTAL
            }
            rootView.addView(stats, lp())

            // Two rows of actions, so a wide strip display stays readable.
            val row1 = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            val row2 = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            row1.addView(action(I18n.get("touch.stateAction.save")) {
                MainActivityRuntime.instance?.saveState()
            }, rowLp())
            row1.addView(action(I18n.get("touch.stateAction.load")) {
                MainActivityRuntime.instance?.loadState()
            }, rowLp())
            row1.addView(action(I18n.get("secondScreen.fastForward")) {
                MainActivityRuntime.instance?.toggleFastForward()
            }, rowLp())
            row2.addView(action(I18n.get("secondScreen.pause")) {
                // Same toggle the on-screen pause button uses.
                if (MainActivityRuntime.eState.value == EmuState.PAUSED) MainActivityRuntime.resume()
                else MainActivityRuntime.pause()
            }, rowLp())
            row2.addView(action(I18n.get("touch.stateAction.screenshot")) {
                MainActivityRuntime.instance?.applicationContext?.let { Screenshots.capture(it) }
            }, rowLp())
            rootView.addView(row1, lp())
            rootView.addView(row2, lp())

            setContentView(rootView)
            updateStats()
        }

        private fun lp() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        )

        private fun rowLp() = LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f,
        )

        private fun action(label: String, onClick: () -> Unit): View =
            Button(context).apply {
                text = label
                isAllCaps = false
                setOnClickListener { runCatching { onClick() } }
            }

        private fun updateStats() {
            val fps = runCatching { NativeApp.getFPS() }.getOrDefault(0f)
            val title = MainActivityRuntime.currentGame.value?.title.orEmpty()
            // Read charge straight from BatteryManager rather than plumbing state over from the
            // main-display status cluster — this panel ticks on its own and the call is cheap.
            val battery = runCatching {
                (context.getSystemService(Context.BATTERY_SERVICE) as? android.os.BatteryManager)
                    ?.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
            }.getOrDefault(-1)
            val clock = java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
                .format(java.util.Date(System.currentTimeMillis()))
            stats.text = buildString {
                if (title.isNotBlank()) append(title).append('\n')
                append("FPS ").append(String.format(java.util.Locale.US, "%.1f", fps))
                if (battery >= 0) append("   ").append(battery).append('%')
                append("   ").append(clock)
            }
        }

        override fun onStart() {
            super.onStart()
            ticking = true
            handler.post(tick)
        }

        override fun onStop() {
            ticking = false
            handler.removeCallbacks(tick)
            super.onStop()
        }
    }
}
