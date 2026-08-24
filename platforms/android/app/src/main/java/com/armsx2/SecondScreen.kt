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

    // Panel palette. Deliberately not pulled from the Compose theme: a Presentation is outside
    // the Compose tree entirely, and reaching into MaterialTheme from a plain View would mean
    // holding a composition alive just to read six colours.
    private const val BG_TOP = 0xFF11151C.toInt()
    private const val BG_BOTTOM = 0xFF080A0E.toInt()
    private const val TILE_ACTION = 0xFF1B2331.toInt()
    private const val TILE_STAT = 0xFF141A23.toInt()
    private const val BORDER = 0x33FFFFFF
    private const val ACCENT = 0xFF7FB2FF.toInt()
    private const val ACCENT_DIM = 0x557FB2FF
    private const val TEXT = 0xFFE6EAF0.toInt()
    private const val TEXT_DIM = 0xFF9AA0A6.toInt()

    private const val PREF_KEY = "secondScreen.enabled"
    private const val PREF_OSD_KEY = "secondScreen.moveOsd"
    private const val TICK_MS = 500L

    /** Move the performance OSD off the game and onto this panel while it is showing (Shane [TDD]:
     *  "OSD down there instead of up top"). Uses the LIVE-only flag apply, so the user's saved
     *  per-stat OSD selection is never overwritten — it is restored the moment the panel goes away. */
    val moveOsd = mutableStateOf(true)

    fun setMoveOsd(value: Boolean) {
        moveOsd.value = value
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_OSD_KEY, value).apply() }
        applyOsdRouting()
    }

    /** Suppress the on-game OSD while the panel is up; restore the user's own flags when it isn't. */
    private fun applyOsdRouting() {
        val suppress = moveOsd.value && presentation?.isShowing == true
        runCatching {
            if (suppress) {
                NativeApp.osdApplyFlags(
                    false, false, false, false, false, false, false, false, false, false, false, false,
                )
            } else {
                // Re-assert the user's own OSD mode rather than blanket-true, so someone who had
                // most stats off doesn't get them all switched on when the panel goes away.
                com.armsx2.ui.InGameOverlay.reapplyOsdMode()
            }
        }
    }

    /** User toggle (App settings). Default OFF — dual-screen owners turn it on themselves, and it
     *  should never surprise someone who plugs into a TV or casts (asked for by Shane [TDD]). */
    val enabled = mutableStateOf(false)

    private var presentation: Panel? = null
    private var listener: DisplayManager.DisplayListener? = null
    private val handler = Handler(Looper.getMainLooper())

    fun load() {
        runCatching {
            enabled.value = MainActivityRuntime.prefs.getBoolean(PREF_KEY, false)
            moveOsd.value = MainActivityRuntime.prefs.getBoolean(PREF_OSD_KEY, true)
        }
    }

    fun set(context: Context, value: Boolean) {
        enabled.value = value
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_KEY, value).apply() }
        if (value) attach(context) else detach()
    }

    /** Whether ARMSX2 is in the foreground. A Presentation belongs to the app's window token but
     *  is NOT torn down when the activity stops, so the panel stayed up on the second display while
     *  the user was off doing something else entirely — reported, and it also meant a stale FPS
     *  reading sitting on screen. Driven from the activity's onResume/onPause. */
    @Volatile private var foreground: Boolean = true

    fun setForeground(context: Context, value: Boolean) {
        val changed = foreground != value
        foreground = value
        // Re-attach on EVERY resume, not only on a foreground change: the activity can come back
        // on a different display than it left on (dual-screen handhelds let you move the app),
        // and refresh() is the only thing that re-picks the target. Idempotent — a panel already
        // on the right display is left alone. Detach still only fires on a real change.
        if (value) attach(context) else if (changed) detach()
    }

    /** Start watching for a second display and show the panel on one if present. */
    fun attach(context: Context) {
        if (!enabled.value || !foreground) return
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

    /** Tear the panel down and put it back, so a layout change shows immediately. The panel is
     *  built once in onCreate — cheaper than making every tile individually reconfigurable, and
     *  the only thing that triggers it is a human editing the grid. */
    fun rebuild() {
        if (presentation?.isShowing != true) return
        val ctx = MainActivityRuntime.instance?.applicationContext ?: return
        detach()
        attach(ctx)
    }

    fun detach() {
        runCatching { presentation?.dismiss() }
        presentation = null
        // Hand the OSD back to the game screen the moment the panel is gone.
        applyOsdRouting()
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

    /** The display ARMSX2 itself is on right now.
     *
     *  ★ NOT [Display.DEFAULT_DISPLAY]. On a dual-screen handheld the user can start (or move)
     *  ARMSX2 onto the second panel, and the whole notion of "the other display" inverts: the
     *  built-in one becomes the free display and the second one is where the game is. Anchoring
     *  on DEFAULT_DISPLAY put the panel on top of the running game whenever the app launched on
     *  the second screen — the panel covered the very thing it reports on.
     *
     *  Read from the Activity, not the app context: only a visual context knows which display it
     *  is being shown on. Falls back to DEFAULT_DISPLAY, which is right for the ordinary case of
     *  the app on the built-in panel. */
    private fun hostDisplayId(context: Context): Int {
        var current: Context? = context
        while (current is android.content.ContextWrapper) {
            if (current is android.app.Activity) break
            current = current.baseContext
        }
        val activity = current as? android.app.Activity
            ?: MainActivityRuntime.instance
            ?: return Display.DEFAULT_DISPLAY
        @Suppress("DEPRECATION")
        return runCatching {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R)
                activity.display?.displayId
            else
                activity.windowManager?.defaultDisplay?.displayId
        }.getOrNull() ?: Display.DEFAULT_DISPLAY
    }

    private fun secondaryDisplay(context: Context): Display? {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager ?: return null
        val hostId = hostDisplayId(context)
        // PRESENTATION category is the one Android intends for this; fall back to "any display the
        // app itself isn't on" because some handhelds don't tag their second panel. Both paths
        // exclude the host — a display can be PRESENTATION-tagged and still be the one showing the
        // game, which is exactly the case that produced the panel-over-the-game report.
        val presentationDisplays = runCatching {
            dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        }.getOrNull()
        presentationDisplays?.firstOrNull { it.displayId != hostId }?.let { return it }
        return runCatching {
            dm.displays?.firstOrNull { it.displayId != hostId }
        }.getOrNull()
    }

    private fun refresh(context: Context) {
        if (!enabled.value || !foreground) { detach(); return }
        val target = secondaryDisplay(context)
        if (target == null) { detach(); return }
        // Already showing on this display? Leave it alone.
        presentation?.let { if (it.display?.displayId == target.displayId && it.isShowing) return }
        detach()
        runCatching {
            val p = Panel(context, target)
            p.show()
            presentation = p
            applyOsdRouting()
        }
    }

    /** The panel itself. */
    private class Panel(context: Context, display: Display) : Presentation(context, display) {

        private lateinit var stats: TextView
        private lateinit var idleLabel: TextView
        private lateinit var grid: android.widget.GridLayout
        private var dp: Float = 1f
        private val tileViews = HashMap<SecondScreenTile, View>()
        /** Rows that only make sense with a game running; hidden in the library. */
        private val gameRows = mutableListOf<View>()
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
            // ★ A Presentation is a Dialog, so BACK dismissed it — and nothing re-showed it, since
            // the panel is only (re)created when a display is added or removed. Reported by Shane
            // [TDD]: "hit back on the bottom screen and I can't get it back". The panel is not a
            // dialog the user opened, so it should not be dismissable; the App-settings toggle and
            // unplugging the display are the ways out.
            setCancelable(false)
            setCanceledOnTouchOutside(false)
            dp = resources.displayMetrics.density
            val pad = (dp * 14).toInt()

            // ★ Styled in code, not from a theme/XML. A Presentation gets the *system* dialog
            // theme, not the app's Compose theme, which is why the panel looked like a stock
            // Android dialog dropped onto the second screen (NiceRon: "the current stock Android
            // UI for it really doesn't look good at all"). Painting it here keeps the plain-View
            // build — see the class note on why Compose is not used in a Presentation — while
            // matching the app: dark ground, rounded surface tiles, one accent.
            val rootView = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                background = android.graphics.drawable.GradientDrawable(
                    android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                    intArrayOf(BG_TOP, BG_BOTTOM),
                )
                setPadding(pad, pad, pad, pad)
            }

            stats = TextView(context).apply {
                setTextColor(TEXT_DIM)
                textSize = 13f
                gravity = Gravity.CENTER_HORIZONTAL
                visibility = View.GONE   // the header line only shows if no TITLE tile is placed
            }
            rootView.addView(stats, lp())

            // Shown in the library, where the game actions below would all be dead buttons.
            idleLabel = TextView(context).apply {
                text = I18n.get("secondScreen.noGame")
                setTextColor(TEXT_DIM)
                textSize = 14f
                gravity = Gravity.CENTER_HORIZONTAL
                setPadding(0, pad, 0, pad)
            }
            rootView.addView(idleLabel, lp())

            // The customisable grid. Every tile is one box; the user picks which and in what order
            // (SecondScreenLayout), so this is a single loop rather than hand-placed rows.
            val columns = SecondScreenLayout.columns()
            grid = android.widget.GridLayout(context).apply {
                columnCount = columns
                useDefaultMargins = false
            }
            SecondScreenLayout.tiles().forEach { tile ->
                val view = buildTile(tile) ?: return@forEach
                val params = android.widget.GridLayout.LayoutParams().apply {
                    width = 0
                    height = ViewGroup.LayoutParams.WRAP_CONTENT
                    columnSpec = android.widget.GridLayout.spec(
                        android.widget.GridLayout.UNDEFINED, 1, 1f,
                    )
                    val m = (dp * 4).toInt()
                    setMargins(m, m, m, m)
                }
                grid.addView(view, params)
                // Actions need a VM; read-outs like the clock and battery do not, so only the
                // former are hidden in the library. Hiding the clock too would leave an empty
                // panel that looks broken.
                if (!tile.stat) gameRows += view
                tileViews[tile] = view
            }
            rootView.addView(grid, lp())

            setContentView(rootView)
            updateStats()
        }

        /** One box. Read-out tiles are TextViews refreshed by [updateStats]; action tiles are the
         *  same box with a press effect and a click. Returns null for a macro with nothing
         *  assigned — an empty macro tile is a button that does nothing. */
        private fun buildTile(tile: SecondScreenTile): View? {
            macroFor(tile)?.let { id ->
                if (runCatching { com.armsx2.ui.touch.TouchControls.macroCodes(id).isEmpty() }
                        .getOrDefault(true)
                ) return null
                return macroAction(id).styleAsTile(action = true)
            }
            if (tile.stat) {
                return TextView(context).styleAsTile(action = false).also {
                    (it as TextView).text = I18n.get(tile.labelKey)
                }
            }
            val label = I18n.get(tile.labelKey)
            return TextView(context).styleAsTile(action = true).also { view ->
                (view as TextView).text = label
                view.setOnClickListener { runCatching { fire(tile) } }
            }
        }

        private fun macroFor(tile: SecondScreenTile) = when (tile) {
            SecondScreenTile.MACRO1 -> com.armsx2.ui.touch.TouchButtonId.MACRO1
            SecondScreenTile.MACRO2 -> com.armsx2.ui.touch.TouchButtonId.MACRO2
            SecondScreenTile.MACRO3 -> com.armsx2.ui.touch.TouchButtonId.MACRO3
            SecondScreenTile.MACRO4 -> com.armsx2.ui.touch.TouchButtonId.MACRO4
            else -> null
        }

        /** Common tile chrome: rounded surface, hairline border, centred text. */
        private fun View.styleAsTile(action: Boolean): View = apply {
            background = android.graphics.drawable.GradientDrawable().apply {
                cornerRadius = dp * 14f
                setColor(if (action) TILE_ACTION else TILE_STAT)
                setStroke((dp * 1f).toInt(), if (action) ACCENT_DIM else BORDER)
            }
            val h = (dp * 10).toInt()
            val v = (dp * 14).toInt()
            setPadding(h, v, h, v)
            if (this is TextView) {
                gravity = Gravity.CENTER
                setTextColor(if (action) ACCENT else TEXT)
                textSize = 14f
                // ALWAYS two lines, not just at most two. An active tile appends a state line
                // ("\n❚❚" on Pause, likewise Fast Forward), so with maxLines alone a tile grew
                // the moment you used it and the row went ragged. Reserving the second line makes
                // every tile the same height whether or not it is currently showing state, and it
                // scales with the text size instead of being pinned to a magic dp value.
                minLines = 2
                maxLines = 2
                if (this is Button) isAllCaps = false
            }
            if (action) isClickable = true
        }

        /** What an action tile does. Kept in one place so the tile list stays declarative. */
        private fun fire(tile: SecondScreenTile) {
            when (tile) {
                SecondScreenTile.SAVE -> MainActivityRuntime.instance?.saveState()
                SecondScreenTile.LOAD -> MainActivityRuntime.instance?.loadState()
                SecondScreenTile.FAST_FORWARD -> MainActivityRuntime.instance?.toggleFastForward()
                SecondScreenTile.PAUSE ->
                    if (MainActivityRuntime.eState.value == EmuState.PAUSED) {
                        MainActivityRuntime.resume()
                    } else {
                        // Mark it deliberate FIRST. This is the only pause that leaves the game
                        // uncovered, and the stuck-paused backstop resumes exactly that state
                        // unless it is told the user meant it. resume() clears the flag.
                        MainActivityRuntime.userHeldPause.value = true
                        MainActivityRuntime.pause()
                    }
                SecondScreenTile.SCREENSHOT ->
                    MainActivityRuntime.instance?.applicationContext?.let { Screenshots.capture(it) }
                SecondScreenTile.ASPECT -> {
                    // Cycles the nine display modes the in-game menu offers, through the same
                    // save path, so it lands in the same scope (per-game when the game has one).
                    val cur = com.armsx2.ui.InGameOverlay.settingsState.value
                    com.armsx2.ui.InGameOverlay.saveSettings(
                        cur.copy(aspectRatio = (cur.aspectRatio + 1) % 9),
                    )
                }
                SecondScreenTile.SLOT ->
                    MainActivityRuntime.currentSaveSlot.intValue =
                        (MainActivityRuntime.currentSaveSlot.intValue + 1) % 10
                SecondScreenTile.HIDE ->
                    MainActivityRuntime.instance?.let { SecondScreen.set(it.applicationContext, false) }
                else -> Unit
            }
            // Reflect the new state immediately rather than at the next 500ms tick — a tile that
            // updates half a second after the tap reads as a missed press.
            updateStats()
        }

        /** A macro button: press fires every assigned pad button (honouring its turbo Frequency),
         *  release drops them — the same fireMacro path the on-screen macro widget uses. */
        private fun macroAction(id: com.armsx2.ui.touch.TouchButtonId): View =
            Button(context).apply {
                text = id.label
                isAllCaps = false
                setOnTouchListener { v, ev ->
                    when (ev.actionMasked) {
                        android.view.MotionEvent.ACTION_DOWN -> {
                            runCatching {
                                com.armsx2.ui.touch.TouchControls.fireMacro(id, "secondScreen", true) { code, pressed ->
                                    NativeApp.setPadButton(code, 0, pressed)
                                }
                            }
                            v.isPressed = true
                        }
                        android.view.MotionEvent.ACTION_UP,
                        android.view.MotionEvent.ACTION_CANCEL -> {
                            runCatching {
                                com.armsx2.ui.touch.TouchControls.fireMacro(id, "secondScreen", false) { code, pressed ->
                                    NativeApp.setPadButton(code, 0, pressed)
                                }
                            }
                            v.isPressed = false
                            v.performClick()
                        }
                    }
                    true
                }
            }

        private fun lp() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        )

        // MATCH_PARENT height, not WRAP_CONTENT: inside a horizontal row this stretches every
        // tile to the tallest in that row, so any residual difference (a Button's built-in
        // padding against a TextView's) is absorbed rather than showing as a ragged edge.
        private fun rowLp() = LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.MATCH_PARENT, 1f,
        )

        private fun action(label: String, onClick: () -> Unit): View =
            Button(context).apply {
                text = label
                isAllCaps = false
                setOnClickListener { runCatching { onClick() } }
            }

        private fun updateStats() {
            // In the library there is no VM, so save/load/pause/FF/screenshot and the macros are
            // all dead buttons — hide them and say so rather than showing controls that do nothing.
            val inGame = MainActivityRuntime.eState.value == EmuState.RUNNING ||
                MainActivityRuntime.eState.value == EmuState.PAUSED
            gameRows.forEach { it.visibility = if (inGame) View.VISIBLE else View.GONE }
            idleLabel.visibility = if (inGame) View.GONE else View.VISIBLE

            val fps = runCatching { NativeApp.getFPS() }.getOrDefault(0f)
            val title = MainActivityRuntime.currentGame.value?.title.orEmpty()
            // Read charge straight from BatteryManager rather than plumbing state over from the
            // main-display status cluster — this panel ticks on its own and the call is cheap.
            val battery = runCatching {
                (context.getSystemService(Context.BATTERY_SERVICE) as? android.os.BatteryManager)
                    ?.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
            }.getOrDefault(-1)
            // Charging state, so the icon can show a bolt rather than a misleading empty cell.
            val charging = runCatching {
                val bm = context.getSystemService(Context.BATTERY_SERVICE) as? android.os.BatteryManager
                bm?.isCharging == true
            }.getOrDefault(false)
            val clock = java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
                .format(java.util.Date(System.currentTimeMillis()))

            tileViews.forEach { (tile, view) ->
                val text: String? = when (tile) {
                    SecondScreenTile.TITLE -> title.ifBlank { I18n.get("secondScreen.tile.title") }
                    // FPS is meaningless with no VM — the reading would just sit at the last value.
                    SecondScreenTile.FPS ->
                        if (inGame) "FPS\n" + String.format(java.util.Locale.US, "%.1f", fps)
                        else "FPS\n—"
                    SecondScreenTile.SPEED -> {
                        val nominal = runCatching { NativeApp.getNominalFrameRate() }.getOrDefault(0f)
                        if (inGame && nominal > 1f) "SPEED\n" + (fps / nominal * 100f).toInt() + "%"
                        else "SPEED\n—"
                    }
                    SecondScreenTile.BATTERY ->
                        if (battery >= 0) batteryIcon(battery, charging) + "\n" + battery + "%" else null
                    SecondScreenTile.CLOCK -> clock
                    SecondScreenTile.ACHIEVEMENTS -> achievementSummary()
                    // Action tiles that carry state show it, so the panel reads as a status
                    // display and not just a remote control.
                    SecondScreenTile.FAST_FORWARD -> I18n.get(tile.labelKey) +
                        if (runCatching { MainActivityRuntime.isFastForwardActive() }.getOrDefault(false))
                            "\n▶▶" else ""
                    SecondScreenTile.PAUSE -> I18n.get(tile.labelKey) +
                        if (MainActivityRuntime.eState.value == EmuState.PAUSED) "\n❚❚" else ""
                    SecondScreenTile.SLOT ->
                        I18n.get(tile.labelKey) + "\n" + MainActivityRuntime.currentSaveSlot.intValue
                    SecondScreenTile.ASPECT -> I18n.get(tile.labelKey) + "\n" +
                        aspectLabel(com.armsx2.ui.InGameOverlay.settingsState.value.aspectRatio)
                    else -> null
                }
                if (text != null && view is TextView) view.text = text
            }

            // Header line, only when the user has no TITLE tile placed — otherwise the game name
            // would appear twice.
            if (SecondScreenTile.TITLE in SecondScreenLayout.tiles() || title.isBlank()) {
                stats.visibility = View.GONE
            } else {
                stats.visibility = View.VISIBLE
                stats.text = title
            }
        }

        /** "12/40  ·  Last: <title>" — the collection at a glance plus whatever unlocked most
         *  recently THIS SESSION. RetroAchievements' own snapshot carries no unlock timestamp, so
         *  "recent" is tracked by watching the locked→unlocked edge on the panel's own tick rather
         *  than invented from list order. */
        private fun achievementSummary(): String {
            val json = runCatching { NativeApp.getAchievementsJSON() }.getOrDefault("")
            val items = runCatching { com.armsx2.ui.achievements.parseAchievementItems(json) }
                .getOrDefault(emptyList())
            if (items.isEmpty()) return I18n.get("secondScreen.tile.achievements") + "\n—"
            val unlocked = items.filter { it.unlocked }
            unlocked.map { it.id }.toSet().let { ids ->
                val fresh = ids - seenUnlocked
                if (seenUnlocked.isNotEmpty() && fresh.isNotEmpty())
                    lastUnlock = unlocked.firstOrNull { it.id in fresh }?.title
                seenUnlocked = ids
            }
            return buildString {
                append(unlocked.size).append('/').append(items.size)
                lastUnlock?.let { append('\n').append(it) }
            }
        }

        /** Session-only: which achievement ids were already unlocked when we last looked. */
        private var seenUnlocked: Set<Int> = emptySet()
        private var lastUnlock: String? = null

        private fun aspectLabel(value: Int): String = when (value) {
            0 -> I18n.get("setup.aspect.stretch")
            1 -> I18n.get("setup.aspect.auto")
            2 -> "4:3"
            3 -> "16:9"
            4 -> "10:7"
            5 -> "21:9"
            6 -> "20:9"
            7 -> "19.5:9"
            else -> "Custom"
        }

        /** A battery ICON that tracks the level, not just a number (asked for on the panel).
         *  Uses the block glyphs rather than an emoji so it renders in the same weight as the
         *  surrounding text on every device, and a bolt while charging. */
        private fun batteryIcon(pct: Int, charging: Boolean): String = when {
            charging -> "⚡"
            pct >= 80 -> "▰▰▰▰"
            pct >= 60 -> "▰▰▰▱"
            pct >= 40 -> "▰▰▱▱"
            pct >= 20 -> "▰▱▱▱"
            else -> "▱▱▱▱"
        }

        /** Belt-and-braces with setCancelable(false): some OEM shells still route BACK here. */
        @Deprecated("Dialog.onBackPressed", ReplaceWith(""))
        override fun onBackPressed() {
            // Intentionally nothing — the panel is not dismissable from the second screen.
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
