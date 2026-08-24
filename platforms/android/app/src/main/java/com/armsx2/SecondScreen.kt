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
import androidx.compose.ui.graphics.toArgb
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

    // Panel palette, taken from the app's LIVE theme.
    //
    // This used to be a hand-written set of neutral greys, on the reasoning that a Presentation
    // sits outside the Compose tree and reading MaterialTheme from a plain View would mean
    // holding a composition alive just to get six colours. The reasoning was right; the
    // conclusion was not. ARMSX2's night theme is BLUE (0xFF0A1C36), so a grey panel was not a
    // neutral choice, it was a different app on the second screen -- which is what "it still
    // looks quite unpleasant... more like stock android instead of armsx2" was describing.
    //
    // ThemeBridge publishes the already-resolved scheme, so there is no composition to hold and
    // no second copy of the theme logic to drift: the panel follows Blue, Purple, OLED, Custom,
    // Material You and the animated RGB mode without knowing any of them exist. The old values
    // stay as the fallback for the window between process start and the first composition.
    private fun themed(
        fallback: Int,
        pick: (androidx.compose.material3.ColorScheme) -> androidx.compose.ui.graphics.Color,
    ): Int = com.armsx2.ui.theme.ThemeBridge.scheme?.let { pick(it).toArgb() } ?: fallback

    /** Scale a colour's RGB toward black, keeping alpha. For the ground gradient. */
    private fun Int.darken(factor: Float): Int {
        val a = this ushr 24 and 0xFF
        val r = ((this shr 16 and 0xFF) * factor).toInt().coerceIn(0, 255)
        val g = ((this shr 8 and 0xFF) * factor).toInt().coerceIn(0, 255)
        val b = ((this and 0xFF) * factor).toInt().coerceIn(0, 255)
        return (a shl 24) or (r shl 16) or (g shl 8) or b
    }

    private val BG_TOP get() = themed(0xFF11151C.toInt()) { it.background }
    private val BG_BOTTOM get() = BG_TOP.darken(0.55f)
    private val TILE_ACTION get() = themed(0xFF1B2331.toInt()) { it.surfaceVariant }
    private val TILE_STAT get() = themed(0xFF141A23.toInt()) { it.surface }
    private val BORDER get() = (themed(0xFFFFFFFF.toInt()) { it.outline } and 0x00FFFFFF) or 0x33000000
    private val ACCENT get() = themed(0xFF7FB2FF.toInt()) { it.primary }
    private val ACCENT_DIM get() = (ACCENT and 0x00FFFFFF) or 0x55000000
    private val TEXT get() = themed(0xFFE6EAF0.toInt()) { it.onSurface }
    private val TEXT_DIM get() = themed(0xFF9AA0A6.toInt()) { it.onSurfaceVariant }

    // ---- Background choice (requested alongside the restyle) --------------------------------
    /** 0 = the theme's own ground, 1 = the library's backdrop darkened, 2 = solid black. */
    private const val PREF_BACKGROUND = "secondScreen.background"
    const val BG_THEME = 0
    const val BG_LIBRARY = 1
    const val BG_BLACK = 2

    /** A user-supplied image, the fourth choice ("or perhaps an own background"). */
    const val BG_CUSTOM = 3
    private const val PREF_BACKGROUND_URI = "secondScreen.background.uri"

    val background = mutableStateOf(BG_THEME)
    val backgroundUri = mutableStateOf<String?>(null)

    /**
     * Adopt a picked image. Takes the persistable read grant, exactly as the library's own
     * background picker does -- without it the URI works until the process restarts and then
     * silently resolves to nothing, which reads as "my background disappeared".
     */
    fun setBackgroundImage(context: Context, uri: android.net.Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
        }
        backgroundUri.value = uri.toString()
        runCatching {
            MainActivityRuntime.prefs.edit().putString(PREF_BACKGROUND_URI, uri.toString()).apply()
        }
        setBackground(BG_CUSTOM)
    }

    // ---- Top bar ------------------------------------------------------------------------------
    /** Clock and battery in a slim bar across the top rather than as grid tiles ("much cleaner to
     *  just move the time and battery into a top bar similar to the ayn thors menu"). On by
     *  default: it is the same information, and it gives the grid back two cells. */
    private const val PREF_TOP_BAR = "secondScreen.topBar"
    val topBar = mutableStateOf(true)

    fun loadTopBar() {
        topBar.value = runCatching {
            MainActivityRuntime.prefs.getBoolean(PREF_TOP_BAR, true)
        }.getOrDefault(true)
    }

    fun setTopBar(on: Boolean) {
        topBar.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_TOP_BAR, on).apply() }
        rebuild()
    }

    fun loadBackground() {
        background.value = runCatching {
            MainActivityRuntime.prefs.getInt(PREF_BACKGROUND, BG_THEME)
        }.getOrDefault(BG_THEME).coerceIn(BG_THEME, BG_CUSTOM)
        backgroundUri.value = runCatching {
            MainActivityRuntime.prefs.getString(PREF_BACKGROUND_URI, null)
        }.getOrNull()
    }

    // ---- Thermal polling interval -----------------------------------------------------------
    /** Seconds between sensor reads: 1, 2, 3 or 5. Not "realtime" — these are sysfs reads on the
     *  UI thread, and a temperature that moves slower than a second is not worth the syscalls. */
    private const val PREF_TEMP_INTERVAL = "secondScreen.tempInterval"
    val tempIntervalSec = mutableStateOf(2)

    fun loadTempInterval() {
        tempIntervalSec.value = runCatching {
            MainActivityRuntime.prefs.getInt(PREF_TEMP_INTERVAL, 2)
        }.getOrDefault(2).coerceIn(1, 5)
    }

    fun setTempInterval(seconds: Int) {
        tempIntervalSec.value = seconds.coerceIn(1, 5)
        runCatching {
            MainActivityRuntime.prefs.edit().putInt(PREF_TEMP_INTERVAL, tempIntervalSec.value).apply()
        }
    }

    private fun tempIntervalMs(): Long = tempIntervalSec.value * 1000L

    fun setBackground(value: Int) {
        background.value = value.coerceIn(BG_THEME, BG_CUSTOM)
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_BACKGROUND, background.value).apply() }
        rebuild()
    }

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
        loadBackground()
        loadTempInterval()
        loadTopBar()
        loadIgnoredDisplays()
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

    // ---- Per-display opt-out ------------------------------------------------------------------
    /**
     * Displays the user has told the panel to stay off.
     *
     * "The second screen also still appears on the external monitor when connected via usbc"
     * (NiceRon). By the display-picking rule a USB-C monitor is a perfectly good second display,
     * so this is not a bug to fix but a preference to record -- someone with a dual-screen
     * handheld wants the panel on the bottom screen and NOT on the TV they occasionally plug in,
     * and no rule about internal-vs-external gets that right for everyone. Android does not
     * expose a stable public display type before API 34 either, so guessing would be wrong on
     * old devices as well as on unusual ones.
     *
     * Keyed by NAME rather than displayId: ids are reassigned across replugs, names are not.
     */
    private const val PREF_IGNORED = "secondScreen.ignoredDisplays"
    val ignoredDisplays = mutableStateOf<Set<String>>(emptySet())

    fun loadIgnoredDisplays() {
        ignoredDisplays.value = runCatching {
            MainActivityRuntime.prefs.getStringSet(PREF_IGNORED, emptySet())?.toSet()
        }.getOrNull() ?: emptySet()
    }

    private fun persistIgnored() {
        runCatching {
            MainActivityRuntime.prefs.edit().putStringSet(PREF_IGNORED, ignoredDisplays.value).apply()
        }
    }

    /** Stop using [name] and take the panel down from it now. */
    fun ignoreDisplay(name: String) {
        if (name.isBlank()) return
        ignoredDisplays.value = ignoredDisplays.value + name
        persistIgnored()
        detach()
        MainActivityRuntime.instance?.let { refresh(it.applicationContext) }
    }

    /** Forget every opt-out, so the panel can use any second display again. */
    fun clearIgnoredDisplays() {
        ignoredDisplays.value = emptySet()
        persistIgnored()
        MainActivityRuntime.instance?.let { refresh(it.applicationContext) }
    }

    private fun secondaryDisplay(context: Context): Display? {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager ?: return null
        val hostId = hostDisplayId(context)
        val ignored = ignoredDisplays.value
        fun usable(d: Display) = d.displayId != hostId && d.name !in ignored
        // PRESENTATION category is the one Android intends for this; fall back to "any display the
        // app itself isn't on" because some handhelds don't tag their second panel. Both paths
        // exclude the host — a display can be PRESENTATION-tagged and still be the one showing the
        // game, which is exactly the case that produced the panel-over-the-game report.
        val presentationDisplays = runCatching {
            dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        }.getOrNull()
        presentationDisplays?.firstOrNull { usable(it) }?.let { return it }
        return runCatching {
            dm.displays?.firstOrNull { usable(it) }
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
        /** Null when the top bar is off; the clock and battery are grid tiles then instead. */
        private var topBarClock: TextView? = null
        private var topBarBattery: TextView? = null
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
                background = panelBackground(context)
                setPadding(pad, pad, pad, pad)
            }

            // ---- Top bar: clock left, battery right -------------------------------------------
            // A status strip rather than two grid cells. Same information, but it stops the
            // clock competing for space with the things you actually press, and it gives the
            // panel the shape of a menu instead of a wall of identical boxes.
            if (topBar.value) {
                val bar = LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(0, 0, 0, (dp * 10).toInt())
                }
                topBarClock = TextView(context).apply {
                    setTextColor(TEXT)
                    textSize = 15f
                    gravity = Gravity.START
                }
                topBarBattery = TextView(context).apply {
                    setTextColor(TEXT_DIM)
                    textSize = 15f
                    gravity = Gravity.END
                }
                bar.addView(topBarClock, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                bar.addView(topBarBattery, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                rootView.addView(bar, lp())
                // A hairline under it, so the bar reads as chrome and not as another tile.
                rootView.addView(
                    View(context).apply { setBackgroundColor(BORDER) },
                    LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (dp * 1).toInt())
                        .apply { bottomMargin = (dp * 10).toInt() },
                )
            } else {
                topBarClock = null
                topBarBattery = null
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
                // The bar owns these two while it is on; leaving them in the grid as well would
                // show the time twice.
                if (topBar.value && (tile == SecondScreenTile.CLOCK || tile == SecondScreenTile.BATTERY))
                    return@forEach
                val view = buildTile(tile) ?: return@forEach
                val fixedH = SecondScreenLayout.tileHeight()
                val params = android.widget.GridLayout.LayoutParams().apply {
                    width = 0
                    // 0 means "as tall as the text needs", which is what the panel always did.
                    height = if (fixedH > 0) (dp * fixedH).toInt()
                    else ViewGroup.LayoutParams.WRAP_CONTENT
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
            if (tile == SecondScreenTile.COVER) return buildCoverTile()
            if (tile.stat) {
                return TextView(context).styleAsTile(action = false).also {
                    (it as TextView).text = I18n.get(tile.labelKey)
                }
            }
            val label = I18n.get(tile.labelKey)
            return TextView(context).styleAsTile(action = true).also { view ->
                (view as TextView).text = tileFace(tile.icon, label)
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

        /**
         * The cover art tile: the only tile that is a picture rather than text.
         *
         * Loading goes through the same two sources the library uses -- a user-set custom cover
         * file first, then the fetched cover URL -- so the panel shows whatever the library
         * shows, including a per-game cover the user chose by hand. The URL path uses Coil's
         * ImageLoader directly because a Presentation is plain Views; the file path decodes
         * inline, since it is local and already on disk.
         *
         * Re-resolved on the panel tick rather than once at build, so it follows a game change
         * without the panel being rebuilt.
         */
        private fun buildCoverTile(): View =
            android.widget.ImageView(context).apply {
                scaleType = android.widget.ImageView.ScaleType.CENTER_CROP
                adjustViewBounds = true
                background = android.graphics.drawable.GradientDrawable().apply {
                    cornerRadius = dp * 14f
                    setColor(TILE_STAT)
                    setStroke((dp * 1f).toInt(), BORDER)
                }
                clipToOutline = true
                outlineProvider = object : android.view.ViewOutlineProvider() {
                    override fun getOutline(v: View, o: android.graphics.Outline) {
                        o.setRoundRect(0, 0, v.width, v.height, dp * 14f)
                    }
                }
            }

        /** The cover currently shown, so the tick only reloads when the game actually changes. */
        private var coverKey: String? = null

        private fun updateCover(view: android.widget.ImageView) {
            val game = MainActivityRuntime.currentGame.value
            val key = game?.serial ?: game?.title
            if (key == coverKey) return
            coverKey = key
            if (game == null) { view.setImageDrawable(null); return }
            val custom = runCatching { com.armsx2.CustomCovers.fileFor(context, game) }.getOrNull()
            if (custom != null) {
                runCatching {
                    view.setImageBitmap(android.graphics.BitmapFactory.decodeFile(custom.absolutePath))
                }
                return
            }
            val url = game.coverUrl ?: run { view.setImageDrawable(null); return }
            runCatching {
                val loader = coil.ImageLoader(context)
                val req = coil.request.ImageRequest.Builder(context)
                    .data(url)
                    .target(
                        onSuccess = { d -> view.setImageDrawable(d) },
                        onError = { _ -> view.setImageDrawable(null) },
                    )
                    .build()
                loader.enqueue(req)
            }
        }

        /**
         * A tile's face: the glyph on its own line, larger and in the accent, over the label.
         *
         * Built as a Spannable rather than two stacked TextViews because the grid measures one
         * view per tile, and the two-line shape is what keeps every tile the same height. When a
         * tile has no glyph (the stat tiles) this is just the text, so callers need no branch.
         */
        private fun tileFace(icon: String, label: String): CharSequence {
            if (icon.isEmpty()) return label
            val text = "$icon\n$label"
            return android.text.SpannableString(text).apply {
                setSpan(
                    android.text.style.RelativeSizeSpan(1.55f), 0, icon.length,
                    android.text.Spannable.SPAN_EXCLUSIVE_EXCLUSIVE,
                )
            }
        }

        /**
         * The panel's ground, per the user's choice.
         *
         * The library's backdrop is offered because the panel sits next to the library and
         * looking like a different app was the complaint; it is darkened rather than drawn as-is
         * so tile text stays readable over whatever image is behind it. Solid black is for OLED
         * second screens, where a gradient is just power spent on something nobody asked to see.
         */
        private fun panelBackground(context: Context): android.graphics.drawable.Drawable =
            when (background.value) {
                BG_BLACK -> android.graphics.drawable.ColorDrawable(Color.BLACK)
                BG_CUSTOM -> runCatching {
                    val uri = android.net.Uri.parse(backgroundUri.value ?: error("no image"))
                    val art = context.contentResolver.openInputStream(uri).use { stream ->
                        android.graphics.drawable.Drawable.createFromStream(stream, uri.toString())
                    } ?: error("undecodable")
                    // Same scrim as the library backdrop: an arbitrary photo has no contract to
                    // be dark, and tile text has to stay readable over whatever was picked.
                    android.graphics.drawable.LayerDrawable(
                        arrayOf(art, android.graphics.drawable.ColorDrawable(0xB0000000.toInt())),
                    )
                }.getOrElse { themeGround() }
                BG_LIBRARY -> runCatching {
                    val art = androidx.core.content.ContextCompat.getDrawable(
                        context, com.armsx2.R.drawable.library_bg_xmb,
                    ) ?: throw IllegalStateException("no backdrop")
                    android.graphics.drawable.LayerDrawable(
                        arrayOf(art, android.graphics.drawable.ColorDrawable(0xB0000000.toInt())),
                    )
                }.getOrElse { themeGround() }
                else -> themeGround()
            }

        private fun themeGround(): android.graphics.drawable.Drawable =
            android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(BG_TOP, BG_BOTTOM),
            )

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
                // The panel knows its own display; the settings screen does not.
                SecondScreenTile.NOT_HERE -> ignoreDisplay(display?.name.orEmpty())
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
            // Thermals are file reads, so they run on their own interval rather than on every
            // panel tick — that interval IS the mitigation Cotcho asked about. Cheap to call:
            // Thermals.poll returns immediately until the interval is up.
            runCatching { Thermals.poll(context, tempIntervalMs()) }
            // ONLY with a VM. getAchievementsJSON is VM-scoped -- the native side calls
            // Achievements::GetAchievementsAsJSON() with no guard, and with no game loaded that
            // dereferences null and takes the process down. The panel ticks whether or not a game
            // is running, so calling it unconditionally crashed the app the moment the panel
            // appeared. runCatching is no help here: a SIGSEGV is not a Throwable.
            if (MainActivityRuntime.eState.value == EmuState.RUNNING ||
                MainActivityRuntime.eState.value == EmuState.PAUSED
            ) {
                runCatching { trackAchievements() }
            } else {
                raItems = emptyList()
            }

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

            // The bar, when it is the one showing these. Temps ride along on the right when the
            // user has them, because that is where a status strip is read from.
            topBarClock?.text = clock
            topBarBattery?.text = buildString {
                val t = Thermals.format(Thermals.cpu)
                if (t != null) append("CPU $t   ")
                if (battery >= 0) append(batteryIcon(battery, charging)).append(" ").append(battery).append("%")
            }

            tileViews.forEach { (tile, view) ->
                val text: CharSequence? = when (tile) {
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
                    SecondScreenTile.VPS ->
                        if (inGame) "VPS\n" + runCatching { NativeApp.getVPS() }.getOrDefault(0f).toInt()
                        else "VPS\n—"
                    SecondScreenTile.CPU_LOAD ->
                        if (inGame) "EE\n" + runCatching { NativeApp.getCpuThreadUsage() }.getOrDefault(0f).toInt() + "%"
                        else "EE\n—"
                    SecondScreenTile.GS_LOAD ->
                        if (inGame) "GS\n" + runCatching { NativeApp.getGsThreadUsage() }.getOrDefault(0f).toInt() + "%"
                        else "GS\n—"
                    SecondScreenTile.GPU_LOAD ->
                        if (inGame) "GPU\n" + runCatching { NativeApp.getGpuUsage() }.getOrDefault(0f).toInt() + "%"
                        else "GPU\n—"
                    SecondScreenTile.FRAME_TIME ->
                        if (inGame) "FRAME\n" + String.format(
                            java.util.Locale.US, "%.1f", runCatching { NativeApp.getAverageFrameTime() }.getOrDefault(0f),
                        ) + "ms"
                        else "FRAME\n—"
                    SecondScreenTile.CPU_TEMP -> "CPU\n" + (Thermals.format(Thermals.cpu) ?: "—")
                    SecondScreenTile.GPU_TEMP -> "GPU\n" + (Thermals.format(Thermals.gpu) ?: "—")
                    SecondScreenTile.BATTERY_TEMP -> "BATT\n" + (Thermals.format(Thermals.battery) ?: "—")
                    SecondScreenTile.ACHIEVEMENTS -> achievementSummary()
                    // The picture tile updates itself; the when only produces text.
                    SecondScreenTile.COVER -> {
                        (view as? android.widget.ImageView)?.let { updateCover(it) }
                        null
                    }
                    SecondScreenTile.RA_POINTS -> raPoints()
                    SecondScreenTile.RA_RECENT ->
                        I18n.get(tile.labelKey) + "\n" + (lastUnlock ?: "—")
                    // RetroAchievements' own description of where you are in the game. It is the
                    // one line that says something a number cannot.
                    SecondScreenTile.RICH_PRESENCE ->
                        (if (inGame) runCatching { NativeApp.getRichPresence() }.getOrDefault("") else "")
                            .ifBlank { null } ?: (I18n.get(tile.labelKey) + "\n—")
                    // Action tiles that carry state show it, so the panel reads as a status
                    // display and not just a remote control.
                    // State is carried by the GLYPH, not by an extra line. Appending one was
                    // what made an active tile taller than its neighbours.
                    SecondScreenTile.FAST_FORWARD -> tileFace(
                        if (runCatching { MainActivityRuntime.isFastForwardActive() }.getOrDefault(false))
                            "▶▶▶" else tile.icon,
                        I18n.get(tile.labelKey),
                    )
                    // Shows what the tap will DO: ▶ while paused, ❚❚ while running.
                    SecondScreenTile.PAUSE -> tileFace(
                        if (MainActivityRuntime.eState.value == EmuState.PAUSED) "▶" else tile.icon,
                        I18n.get(tile.labelKey),
                    )
                    SecondScreenTile.SLOT ->
                        tileFace(tile.icon, MainActivityRuntime.currentSaveSlot.intValue.toString())
                    SecondScreenTile.ASPECT -> tileFace(
                        tile.icon,
                        aspectLabel(com.armsx2.ui.InGameOverlay.settingsState.value.aspectRatio),
                    )
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
        /**
         * This tick's achievements, parsed ONCE.
         *
         * Three tiles read this now, and each used to parse the JSON for itself -- so placing all
         * three meant three parses of the same string every tick, for identical results.
         */
        private var raItems: List<com.armsx2.ui.achievements.AchievementItem> = emptyList()

        /**
         * Refresh [raItems] and note any new unlock.
         *
         * Called once per tick regardless of which tiles are placed. It used to live inside the
         * Achievements tile's own text builder, which meant the Latest-unlock tile read "—"
         * forever unless the Achievements tile happened to be on the panel as well.
         */
        private fun trackAchievements() {
            val json = runCatching { NativeApp.getAchievementsJSON() }.getOrDefault("")
            raItems = runCatching { com.armsx2.ui.achievements.parseAchievementItems(json) }
                .getOrDefault(emptyList())
            if (raItems.isEmpty()) return
            val ids = raItems.filter { it.unlocked }.map { it.id }.toSet()
            val fresh = ids - seenUnlocked
            // The first poll of a session seeds the set without announcing: everything already
            // unlocked is not news.
            if (seenUnlocked.isNotEmpty() && fresh.isNotEmpty())
                lastUnlock = raItems.firstOrNull { it.id in fresh }?.title
            seenUnlocked = ids
        }

        /** Earned / total points, which is the figure RA itself leads with. */
        private fun raPoints(): String {
            if (raItems.isEmpty()) return I18n.get("secondScreen.tile.raPoints") + "\n—"
            val earned = raItems.filter { it.unlocked }.sumOf { it.points }
            return "RA\n$earned/${raItems.sumOf { it.points }}"
        }

        private fun achievementSummary(): String {
            if (raItems.isEmpty()) return I18n.get("secondScreen.tile.achievements") + "\n—"
            return buildString {
                append(raItems.count { it.unlocked }).append('/').append(raItems.size)
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
