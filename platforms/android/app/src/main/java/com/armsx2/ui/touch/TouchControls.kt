package com.armsx2.ui.touch

import android.view.KeyEvent
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.EmuState
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import androidx.core.content.edit

/**
 * On-screen touch controls — state, persistence, and the runtime
 * input-mode latch.
 *
 * Three storage tiers in MainActivityRuntime.prefs (single "ARMSX2" SharedPreferences):
 *   - `touch.profiles`            JSON array of {name, layout}
 *   - `touch.active`              Currently-selected profile name
 *
 * The "active layout" is always a copy of the active profile's layout —
 * edits live there until the user explicitly saves them back into a
 * profile (Save / Save As New).
 */
object TouchControls {
    private const val KEY_PROFILES = "touch.profiles"
    private const val KEY_ACTIVE = "touch.active"
    private const val KEY_OPACITY = "touch.opacity"
    private const val KEY_PRESSURE_PERCENT = "touch.pressurePercent"
    // #357: pause button is now a visible top-right single-tap button; this toggles its glyph
    // (off = invisible but still tappable). The migration key one-shot relocates the old
    // invisible center pause hotspot to the new top-right spot for existing layouts.
    /** Legacy show/hide pref — only read now, to seed [KEY_PAUSE_TAP_REVEAL] once. */
    private const val KEY_SHOW_PAUSE = "touch.showPauseButton"
    private const val KEY_PAUSE_TAP_REVEAL = "touch.pauseTapToReveal"
    /** Pass 1 missed per-game layouts, pass 2 swept them, pass 3 moves the button inboard of the
     *  R1/R2 column — the corner spot pass 2 used sat on top of R2. */
    private const val KEY_PAUSE_TOPRIGHT_MIGRATED_3 = "touch.migrated.pauseTopRight3"

    /** Default on-screen pause position. Inboard of the R1/R2 column rather than in the true
     *  corner: ARMSX2 parks its shoulders far higher than NetherSX2 does (R2 at y=0.10 vs ~0.39),
     *  so the corner belongs to R2 — at 56dp it reaches ~789dp of an ~827dp-wide screen, leaving
     *  less room than the button needs. x=0.83 still reads as top-right, clears R2 by ~23dp, and
     *  overlaps only the OSD text, which is what Nether's button does too. */
    const val PAUSE_DEFAULT_X = 0.83f
    const val PAUSE_DEFAULT_Y = 0.055f

    /** On-screen size of the visible ⏸ glyph, dp. 48 = Android's minimum touch target and a
     *  close match for the small top-right button NetherSX2 draws. */
    const val PAUSE_DEFAULT_DP = 48f
    private const val KEY_FACE_MULTI = "touch.faceMulti"
    private const val KEY_TOUCH_GLIDING = "touch.gliding"
    private const val KEY_TOUCH_HAPTICS = "touch.haptics"
    private const val KEY_GESTURE_ON = "touch.gesture.enabled"
    private const val KEY_GESTURE_UP = "touch.gesture.up"
    private const val KEY_GESTURE_DOWN = "touch.gesture.down"
    private const val KEY_GESTURE_LEFT = "touch.gesture.left"
    private const val KEY_GESTURE_RIGHT = "touch.gesture.right"
    private const val KEY_GESTURE_SENS = "touch.gesture.sensitivity"
    private const val KEY_GESTURE_DTAP = "touch.gesture.doubleTap"
    private const val KEY_GESTURE_DTAP_HOLD = "touch.gesture.doubleTapHold"
    private const val KEY_MULTI_RADIUS = "touch.multiRadius"
    private const val KEY_DPAD_SPACING = "touch.dpadSpacing"
    private const val KEY_FLOATING_STICK = "touch.floatingStick"
    private const val KEY_FULL_HALF_STICKS = "touch.fullHalfSticks"
    private const val KEY_ANALOG_EXTRA = "touch.analogExtra"
    private const val KEY_ANALOG_EXTRA_CODE = "touch.analogExtraCode"
    private const val KEY_ANALOG_EXTRA_DIST = "touch.analogExtraDist"
    private const val KEY_ANALOG_EXTRA_ANGLE = "touch.analogExtraAngle"
    private const val KEY_GRID_SNAP = "touch.gridSnap"
    private const val KEY_VIS_MODE = "touch.visibilityMode"
    // One-shot 2.4.7 defaults migration for EXISTING users (saved prefs/layouts
    // predate the default changes, so the new defaults wouldn't otherwise apply).
    private const val KEY_DEFAULTS_MIGRATED_247 = "touch.defaults.migrated.247"
    // Per-game active-profile override: touch.active.game.<serial> -> profile name.
    private const val KEY_ACTIVE_GAME_PREFIX = "touch.active.game."
    // Per-game CUSTOM layout: touch.layout.game.<serial> -> layout JSON. This is
    // an independent per-serial layout (NOT a shared profile object), so editing
    // one game's layout never mutates what another game loads.
    private const val KEY_LAYOUT_GAME_PREFIX = "touch.layout.game."
    // Portable on-disk mirror of profiles, under <DataRoot>/inputprofiles/.
    private const val PROFILE_FILE_SUFFIX = ".touch.json"

    /** Visible to the user. False when a controller is being used (latched
     *  off in onControllerInputDetected); flipped back on by any screen
     *  touch via onSurfaceTouched. Default true so first-run users see
     *  the controls. */
    val visible = mutableStateOf(true)

    /** Edit mode — buttons are draggable + resizable, JNI pad writes are
     *  suppressed. Toggled from InGameOverlay's "Edit Touch Controls"
     *  row. */
    val editMode = mutableStateOf(false)

    /** Leave layout-edit mode and resume the game. Edit mode is entered from
     *  the (paused) pause overlay and runs with the VM paused for a stable
     *  editing screen, so on exit we must resume — otherwise the game stays
     *  frozen with no overlay shown until the user re-opens the menu and hits
     *  Resume (the "emulator froze after saving a controller profile" report).
     *  No-op if the VM isn't paused. */
    fun exitEditMode() {
        editMode.value = false
        // Next time the editor opens it should have its controls, not the collapsed grip the
        // user happened to leave behind.
        editorCollapsed.value = false
        if (MainActivityRuntime.eState.value == EmuState.PAUSED) MainActivityRuntime.resume()
    }

    /** Currently-selected widget in edit mode. When non-null, the edit
     *  toolbar exposes a size slider for the selected widget — useful
     *  for resizing tiny buttons (L3/R3, Start/Select) that are
     *  awkward to pinch-zoom. Tap a widget to select; tap the dim
     *  backdrop to deselect. */
    val selectedButton = mutableStateOf<TouchButtonId?>(null)

    /**
     * Editor panel collapsed to just its grip.
     *
     * The panel covers a real part of the screen, and to select a widget under it you have to
     * touch that widget first -- which the panel is in the way of. Moving it out of the way
     * REACTIVELY cannot help with that, because the obstruction happens before there is anything
     * to react to. This is the way out that does not involve dragging: one tap uncovers
     * everything, one tap brings the controls back.
     *
     * Not persisted. It is a momentary "let me see under this", and a session that opened the
     * editor to a panel with no controls on it would look broken.
     */
    val editorCollapsed = mutableStateOf(false)

    /** Profile picker / save-as dialog shown over the editor. */
    val profileDialogOpen = mutableStateOf(false)

    /** All saved profiles. Mutated via mutators below so observers
     *  recompose. */
    val profiles = mutableStateListOf<TouchProfile>()

    /** Name of the currently-active profile. Persisted. */
    val activeProfileName = mutableStateOf("Default")

    /** Live layout being rendered + edited. Diverges from the saved
     *  profile while editing; "Save" commits it back, "Discard" reloads. */
    val activeLayout = mutableStateOf(TouchLayout.default())

    // ---- Per-orientation layouts --------------------------------------------
    // Landscape and portrait keep SEPARATE layouts. Button positions are fractions of
    // the screen, so one set of fractions authored in landscape produces a squished mess
    // in portrait (and editing either corrupted the other, since they shared a key). The
    // overlay feeds [portrait] from its live size; every layout key is then suffixed by
    // orientation. Landscape keeps the UN-suffixed key, so existing saved layouts stay put
    // and only portrait gets a new key — no migration needed.
    val portrait = mutableStateOf(false)

    /** [baseKey] for the current orientation. Landscape is the bare key (back-compat);
     *  portrait appends ".portrait". */
    private fun orient(baseKey: String) = if (portrait.value) "$baseKey.portrait" else baseKey

    /** Read a game layout for the current orientation: the orientation-specific key first,
     *  then the landscape key as a SEED — so a game with only a landscape layout still
     *  shows something in portrait, and the next Save writes the portrait key, leaving
     *  landscape untouched. */
    private fun readGameLayoutJson(baseKey: String): String? =
        MainActivityRuntime.prefs.getString(orient(baseKey), null)
            ?: MainActivityRuntime.prefs.getString(baseKey, null)

    /** Master opacity 0.00..1.00. Persisted. 0 = fully invisible controls, which stay touchable —
     *  requested by players who know the layout by feel (#428), and matches PPSSPP/Dolphin/Citra.
     *  The floor used to be 0.20; the per-widget legibility floors in TouchControlsOverlay now fade
     *  out below that instead of bottoming out, so 0 really means invisible. */
    val opacity = mutableFloatStateOf(0.55f)

    /** #357: "tap to reveal" mode for the on-screen pause button. Off (default) = the ⏸ glyph
     *  is always drawn top-right. On = it stays invisible until you tap its zone, which surfaces
     *  it for a few seconds (the two-step the old settings cog used) — so hiding the button can
     *  never lock you out of the pause menu, which a plain show/hide toggle did. Persisted. */
    val pauseTapToReveal = mutableStateOf(false)

    /** When enabled, the face-button diamond has a shared hit layer so a
     *  single thumb can slide/press between Cross/Square/Circle/Triangle and
     *  emit overlapping button presses. */
    // Multi-touch hit-test layer for face + shoulder buttons (lets you press
    // several at once / roll between them, and press them while the stick is
    // held). Persisted under KEY_FACE_MULTI. Default ON.
    val faceMultiTouch = mutableStateOf(true)

    // Touch Gliding (NetherSX2-style): while ON, dragging a finger LATCHES every
    // button it crosses (held until the finger lifts) instead of only the one it's
    // currently over — so you can hold several face/shoulder buttons with one drag.
    // Requires the multi-touch layer (faceMultiTouch). Default OFF. Under KEY_TOUCH_GLIDING.
    val touchGliding = mutableStateOf(false)

    // Touch Haptics (issue #247, PPSSPP/Azahar-style): a short vibration tick on every
    // on-screen button press (via NativeApp.touchHaptic). Independent of game rumble.
    // Default ON. Under KEY_TOUCH_HAPTICS.
    val touchHaptics = mutableStateOf(true)

    // Multi-touch hit radius as a fraction of a button's size (GGPO-style). Higher =
    // buttons register a press from further out, so multitouch/rolling works with more
    // space between them. Persisted under KEY_MULTI_RADIUS. Default 0.62.
    val multiTouchRadius = mutableFloatStateOf(0.62f)

    // ---- Gesture control (PPSSPP-style, #SNAKEATER) --------------------------
    // Swipes and a double-tap on EMPTY screen area (never on a button — the gesture layer
    // ignores any finger whose DOWN was claimed by a control) fire PS2 buttons. Lets a
    // touch-only player reach buttons that don't fit on screen.
    // All under KEY_GESTURE_*; default OFF so nothing changes for existing users.
    val gestureEnabled = mutableStateOf(false)
    /** PS2 keycode per swipe direction, or 0 for unassigned. Same codes as TouchButtonId.keycode. */
    val gestureSwipeUp = mutableIntStateOf(0)
    val gestureSwipeDown = mutableIntStateOf(0)
    val gestureSwipeLeft = mutableIntStateOf(0)
    val gestureSwipeRight = mutableIntStateOf(0)
    /** Fraction of the shorter screen edge a finger must travel to count as a swipe. */
    val gestureSwipeSensitivity = mutableFloatStateOf(0.17f)
    /** PS2 keycode fired by a double-tap, or 0 for unassigned. */
    val gestureDoubleTap = mutableIntStateOf(0)
    /**
     * Double-tap behaviour. false = TAP: a momentary pulse (NFS nitro — tap it and it's done).
     * true = HOLD: the button LATCHES down and a second double-tap releases it (ARPG camera lock).
     * Requested by SNAKEATER, who wanted both shapes from the one gesture.
     */
    val gestureDoubleTapHold = mutableStateOf(false)

    // On-screen D-pad key spacing, as a fraction of the pad's half-size. 0 = the four
    // directions meet at the center (a tight +). Higher pushes each direction OUT toward
    // its edge, opening a visible gap in the middle (NetherSX2-style) and growing the
    // center dead-zone to match. Edited in the Touch Layout editor (select the D-Pad).
    // Persisted under KEY_DPAD_SPACING. Default 0 (normal tight D-pad).
    val dpadSpacing = mutableFloatStateOf(0.0f)

    // Floating on-screen stick: the first touch-down inside a stick's zone becomes
    // its origin (the ring re-centers under your finger) instead of a fixed center —
    // easier to grab without looking. Snaps back to rest on release. Global; persisted
    // under KEY_FLOATING_STICK.
    val floatingStick = mutableStateOf(false)

    // Full-half invisible analog sticks (RPCSX-style): the entire LEFT half of the screen acts as
    // the left stick and the RIGHT half as the right stick, with nothing drawn — each finger drives
    // its stick from wherever it touches down (floating origin). The normal L/R stick widgets are
    // hidden while this is on. Global; persisted under KEY_FULL_HALF_STICKS.
    val fullHalfSticks = mutableStateOf(false)

    // Editor-only: while ON, dragging a widget in edit mode snaps its centre anchor to the
    // nearest cross of a square grid (see GRID_COLS in TouchControlsOverlay). Lets the user
    // align buttons precisely instead of eyeballing them "slightly off from one another".
    // Global; persisted under KEY_GRID_SNAP.
    val gridSnap = mutableStateOf(false)

    // ---- Extra button attached to the LEFT stick (SNAKEATER) ---------------------------------
    // A sprint/jump button sitting just ABOVE the left analog. The point of it is the GESTURE:
    // you steer with the stick and then GLIDE the same thumb up onto the button — sprint in
    // GTA / Silent Hill, jump in God of War / Kingdom Hearts — without lifting off and losing
    // your direction.
    //
    // ★ It is deliberately owned by the STICK, not drawn as an independent widget. A separate
    // widget could never do this: the stick locks the gesture onto the pointer that started on
    // it (see StickWidget), so a finger sliding off it never reaches another widget's handler.
    // The stick therefore hit-tests this zone itself and keeps emitting deflection at the same
    // time — and because the zone sits ABOVE the stick, reaching it naturally means full
    // forward, which is exactly the "run forward" the request describes.
    val analogExtraEnabled = mutableStateOf(false)

    /** PS2 button the extra stick button fires. Defaults to Cross — sprint in GTA, jump in
     *  God of War / Kingdom Hearts, the case the request names. */
    val analogExtraKeycode = mutableIntStateOf(96)

    /** Gap between the stick's edge and the button, as a fraction of the stick's radius.
     *  "Near" (0.35) sits within an easy thumb roll; "Far" (0.9) needs a deliberate reach. */
    val analogExtraDistance = mutableFloatStateOf(0.35f)

    fun setAnalogExtraEnabled(v: Boolean) {
        analogExtraEnabled.value = v
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY_ANALOG_EXTRA, v).apply() }
    }

    fun setAnalogExtraKeycode(code: Int) {
        analogExtraKeycode.intValue = code
        runCatching { MainActivityRuntime.prefs.edit().putInt(KEY_ANALOG_EXTRA_CODE, code).apply() }
    }

    fun setAnalogExtraDistance(frac: Float) {
        val c = frac.coerceIn(0.1f, 1.5f)
        analogExtraDistance.floatValue = c
        runCatching { MainActivityRuntime.prefs.edit().putFloat(KEY_ANALOG_EXTRA_DIST, c).apply() }
    }

    /* ---- Extra analog button: shared held state -------------------------------------------
     * TWO independent gestures can hold this button — a finger that presses the widget
     * directly, and a finger that glides up off the left stick into it — and both may be down
     * at once. A plain boolean would let whichever lifted FIRST release the button under the
     * other, so the holders are counted and the key is emitted only on 0 <-> 1 transitions.
     */

    /** True while any gesture holds the extra button; drives its pressed visual. */
    val analogExtraHeld = mutableStateOf(false)

    private var analogExtraHolders = 0

    /** Report one holder's transition. Returns true when the AGGREGATE state flipped, i.e.
     *  when the caller should actually emit the key down/up. */
    @Synchronized
    fun noteAnalogExtraHold(down: Boolean): Boolean {
        val before = analogExtraHolders
        analogExtraHolders = (if (down) before + 1 else before - 1).coerceAtLeast(0)
        val nowHeld = analogExtraHolders > 0
        if (nowHeld == (before > 0)) return false
        analogExtraHeld.value = nowHeld
        return true
    }

    // Editor panel (EditToolbar) placement — SESSION ONLY, and PER-ORIENTATION. Portrait and
    // landscape keep SEPARATE placements (like the touch layout itself): the panel lives at very
    // different screen coordinates in each, so a portrait offset applied in landscape would shove it
    // off-screen. Lets the user drag the settings panel out of the way and resize it so it stops
    // covering the buttons. Not persisted: raw-px offsets don't survive a resolution change.
    private val editorPanelDxP = mutableFloatStateOf(0f)
    private val editorPanelDyP = mutableFloatStateOf(0f)
    private val editorPanelScaleP = mutableFloatStateOf(1f)
    private val editorPanelDxL = mutableFloatStateOf(0f)
    private val editorPanelDyL = mutableFloatStateOf(0f)
    private val editorPanelScaleL = mutableFloatStateOf(1f)

    fun editorPanelDx(landscape: Boolean) = if (landscape) editorPanelDxL else editorPanelDxP
    fun editorPanelDy(landscape: Boolean) = if (landscape) editorPanelDyL else editorPanelDyP
    fun editorPanelScale(landscape: Boolean) = if (landscape) editorPanelScaleL else editorPanelScaleP

    fun resetEditorPanel(landscape: Boolean) {
        editorPanelDx(landscape).floatValue = 0f
        editorPanelDy(landscape).floatValue = 0f
        editorPanelScale(landscape).floatValue = 1f
    }

    /** Held-state for the DS2 pressure-sensitivity modifier. While true, the
     *  pressure-capable buttons report ~50% pressure (PCSX2's
     *  DEFAULT_PRESSURE_MODIFIER) for soft presses (MGS, GTA). Driven by the
     *  on-screen PRESSURE button and the bound physical button. */
    val pressureModifierHeld = mutableStateOf(false)

    /** Full native press range. setPadButton maps 0..PRESSURE_FULL_RANGE onto state 0..1, and
     *  treats a range of exactly 0 as "no modifier — full press". */
    const val PRESSURE_FULL_RANGE = 32767

    /** How hard the modifier presses, as a percentage of a full press. 50 reproduces the value
     *  that used to be hardcoded (PCSX2's DEFAULT_PRESSURE_MODIFIER), which is why the on-screen
     *  PRESSURE button was stuck at exactly half. Clamped away from BOTH ends deliberately: 0
     *  would collide with the "full press" sentinel above, and 100 is indistinguishable from not
     *  holding the modifier at all — neither is a usable setting. Persisted. */
    val pressurePercent = mutableIntStateOf(50)

    fun setPressurePercent(v: Int) {
        val c = v.coerceIn(5, 95)
        pressurePercent.intValue = c
        // Persisted immediately rather than waiting for the layout save() — this is driven from a
        // settings slider, not from the layout editor, so save() may never be called.
        runCatching { MainActivityRuntime.prefs.edit().putInt(KEY_PRESSURE_PERCENT, c).apply() }
    }

    // PS2 DualShock2 pressure-sensitive inputs (keycodes match native-lib.cpp's
    // setPadButton map): d-pad, face buttons, L1/L2/R1/R2. Start/Select/L3/R3 are
    // digital-only, so the modifier never touches them.
    private val PRESSURE_KEYCODES = setOf(
        19, 20, 21, 22,        // d-pad up/down/left/right
        96, 97, 99, 100,       // cross/circle/square/triangle
        102, 103, 104, 105,    // L1/R1/L2/R2
    )

    /** Range to send for [keycode] given the current modifier state: a reduced
     *  (soft) range while the modifier is held on a pressure-capable button,
     *  else 0 (full press). */
    fun pressureRangeFor(keycode: Int): Int =
        if (pressureModifierHeld.value && keycode in PRESSURE_KEYCODES)
            (PRESSURE_FULL_RANGE * pressurePercent.intValue / 100).coerceAtLeast(1)
        else 0

    /** Pressure-capable buttons currently held down, per port, so the modifier can be applied
     *  LIVE to a button that is ALREADY down. [pressureRangeFor] is only consulted when a press
     *  is emitted, so without this the modifier did nothing unless it was held BEFORE the button
     *  — while the gesture these games actually want is the opposite: hold to aim, then ease off
     *  to soften. That is the reported MGS2 "can't cancel my shots" (a half-press on Square is
     *  how MGS2 lowers the weapon without firing). Guarded by its own lock: touch emits on the
     *  UI thread, physical keys on the input thread. */
    private val heldPressureKeys = HashMap<Int, MutableSet<Int>>()

    /** Record/forget a pressure-capable button so [reapplyPressureToHeldButtons] can re-emit it. */
    fun notePressureKeyState(port: Int, keycode: Int, pressed: Boolean) {
        if (keycode !in PRESSURE_KEYCODES) return
        synchronized(heldPressureKeys) {
            val set = heldPressureKeys.getOrPut(port) { mutableSetOf() }
            if (pressed) set.add(keycode) else set.remove(keycode)
        }
    }

    /** Re-send every currently-held pressure-capable button at the CURRENT modifier strength.
     *  Called when the modifier is pressed or released, so easing on/off changes the pressure of
     *  buttons the player is already holding. */
    fun reapplyPressureToHeldButtons() {
        val snapshot = synchronized(heldPressureKeys) { heldPressureKeys.mapValues { it.value.toList() } }
        for ((port, keys) in snapshot) {
            for (kc in keys) {
                runCatching {
                    kr.co.iefriends.pcsx2.NativeApp.setPadButtonForPort(port, kc, pressureRangeFor(kc), true)
                }
            }
        }
    }

    /** Drop all held-pressure bookkeeping (VM stop / pad reset), so a stale key can't be re-emitted. */
    fun clearHeldPressureKeys() {
        synchronized(heldPressureKeys) { heldPressureKeys.clear() }
    }

    /** On-screen touch controls visibility. 0 = Never show (for physical-
     *  controls devices like the RP6 — also hides the settings cog so nothing
     *  overlaps R1); 1..10 = auto-hide after that many seconds of no touch;
     *  11 = Auto — show on screen touch, hide when a controller is used (the
     *  default / legacy behavior). Persisted. */
    val visibilityMode = mutableIntStateOf(11)

    /** Bumped on every touch interaction (screen tap or on-screen button press)
     *  so the auto-hide timer restarts. Not persisted. */
    val interactionTick = mutableIntStateOf(0)

    // ---- On-screen macro / combo buttons (Macro1-4) ----------------------------
    // Each macro fires a user-chosen SET of pad buttons at once (e.g. R1+R2+R3).
    // Stored per macro under touch.macro.<id> = comma-separated TouchButtonId names.
    private const val KEY_MACRO_PREFIX = "touch.macro."

    /** Bumped when any macro's button set changes so the config UI + overlay recompose. */
    val macroBindTick = mutableIntStateOf(0)

    /** One input a macro can fire.
     *
     *  Keyed by the native [code] the pad path takes, NOT by [TouchButtonId]: the
     *  directional inputs a macro wants have no TouchButtonId at all. The on-screen
     *  D-pad is ONE widget (`DPAD`) and each stick is one widget (`L_STICK`/`R_STICK`),
     *  because that's what you draw — but a macro needs "L-Stick Up" specifically, and
     *  there is no id for it. The codes exist regardless (the analog dispatcher already
     *  emits 110-113 / 120-123), so the macro speaks codes and sidesteps the widget
     *  vocabulary entirely. */
    data class MacroTarget(val code: Int, val label: String)

    /** Sentinel for the pressure MODIFIER, which is not a button — it's a flag that makes
     *  the buttons around it send a soft (~50%) press (see [pressureRangeFor]). Negative
     *  so it can never collide with a real keycode. */
    const val MACRO_CODE_PRESSURE = -2

    /** Everything a macro may fire, in display order. Mirrors the set other builds offer:
     *  face / shoulders / clicks / menu, plus the D-pad, the analog toggle, the pressure
     *  modifier and both sticks' directions. */
    val macroAssignableTargets: List<MacroTarget> = listOf(
        MacroTarget(KeyEvent.KEYCODE_BUTTON_Y, "Triangle"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_B, "Circle"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_A, "Cross"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_X, "Square"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_L1, "L1"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_R1, "R1"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_L2, "L2"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_R2, "R2"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_THUMBL, "L3"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_THUMBR, "R3"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_START, "Start"),
        MacroTarget(KeyEvent.KEYCODE_BUTTON_SELECT, "Select"),
        MacroTarget(KeyEvent.KEYCODE_DPAD_UP, "Up"),
        MacroTarget(KeyEvent.KEYCODE_DPAD_RIGHT, "Right"),
        MacroTarget(KeyEvent.KEYCODE_DPAD_DOWN, "Down"),
        MacroTarget(KeyEvent.KEYCODE_DPAD_LEFT, "Left"),
        // 200 = PAD_ANALOG in native-lib.cpp's setPadButton (the DualShock2 mode button).
        MacroTarget(200, "Analog"),
        MacroTarget(MACRO_CODE_PRESSURE, "Pressure"),
        // Stick directions, as the analog dispatcher numbers them:
        // 110 up / 111 right / 112 down / 113 left, and 120-123 for the right stick.
        MacroTarget(110, "L-Stick Up"),
        MacroTarget(111, "L-Stick Right"),
        MacroTarget(112, "L-Stick Down"),
        MacroTarget(113, "L-Stick Left"),
        MacroTarget(120, "R-Stick Up"),
        MacroTarget(121, "R-Stick Right"),
        MacroTarget(122, "R-Stick Down"),
        MacroTarget(123, "R-Stick Left"),
    )

    fun macroTargetFor(code: Int): MacroTarget? = macroAssignableTargets.firstOrNull { it.code == code }

    /** Codes macro [id] fires, in display order (empty if unconfigured).
     *
     *  Reads BOTH forms: this used to store TouchButtonId names, so an existing macro's
     *  "CROSS,R1" still resolves — each token is mapped to its keycode. New writes are
     *  codes, so the migration happens the next time a macro is edited and nothing has to
     *  be rewritten up front. */
    fun macroCodes(id: TouchButtonId): List<Int> {
        val raw = MainActivityRuntime.prefs.getString(KEY_MACRO_PREFIX + id.name, "").orEmpty()
        if (raw.isEmpty()) return emptyList()
        val codes = raw.split(",").mapNotNull { token ->
            token.toIntOrNull()
                ?: runCatching { TouchButtonId.valueOf(token).keycode }.getOrNull()
        }.toSet()
        // Display order, and anything unrecognised drops out.
        return macroAssignableTargets.map { it.code }.filter { it in codes }
    }

    fun setMacroCodes(id: TouchButtonId, codes: List<Int>) {
        val wanted = codes.toSet()
        val csv = macroAssignableTargets.map { it.code }.filter { it in wanted }.joinToString(",")
        MainActivityRuntime.prefs.edit { putString(KEY_MACRO_PREFIX + id.name, csv) }
        invalidateRuntimeMacroCache()
        macroBindTick.intValue++
    }

    // Optional PHYSICAL-controller trigger per macro: bind a physical button to fire
    // the macro's button set (the same set the on-screen M1-M4 buttons use). Stored
    // separately so a macro can be touch-only, physical-only, or both. KEYCODE_UNKNOWN
    // (0) = no physical trigger.
    private const val KEY_MACRO_PHYS_PREFIX = "touch.macro.phys."

    fun macroPhysicalCode(id: TouchButtonId): Int =
        MainActivityRuntime.prefs.getInt(KEY_MACRO_PHYS_PREFIX + id.name, KeyEvent.KEYCODE_UNKNOWN)

    fun setMacroPhysicalCode(id: TouchButtonId, keycode: Int) {
        MainActivityRuntime.prefs.edit { putInt(KEY_MACRO_PHYS_PREFIX + id.name, keycode)}
        invalidateRuntimeMacroCache()
        macroBindTick.intValue++
    }

    fun clearMacroPhysicalCode(id: TouchButtonId) = setMacroPhysicalCode(id, KeyEvent.KEYCODE_UNKNOWN)

    // ---- Macro frequency (turbo) ----------------------------------------------
    // Holding a macro can TOGGLE its button set instead of just holding it down —
    // NetherSX2 calls this Frequency, and it's what stops mash-heavy games (and
    // fighting-game inputs) from costing you a thumb.

    private const val KEY_MACRO_FREQ_PREFIX = "touch.macro.freq."

    /** Upper bound on the interval. A second between toggles is already past useful, and
     *  it keeps the slider walkable on a D-pad. */
    const val MACRO_FREQ_MAX = 60

    /** Frames between toggles while a macro is held. 0 = hold the buttons down, which is
     *  the original behaviour and stays the default. */
    fun macroFrequency(id: TouchButtonId): Int =
        MainActivityRuntime.prefs.getInt(KEY_MACRO_FREQ_PREFIX + id.name, 0).coerceIn(0, MACRO_FREQ_MAX)

    fun setMacroFrequency(id: TouchButtonId, frames: Int) {
        MainActivityRuntime.prefs.edit {
            putInt(KEY_MACRO_FREQ_PREFIX + id.name, frames.coerceIn(0, MACRO_FREQ_MAX))
        }
        macroBindTick.intValue++
    }

    /** A frame in ms. The emulated console is the thing being counted, so 60Hz — an NTSC
     *  frame. This is the one approximation here: a PAL title's frames are 20ms, so its
     *  toggle runs ~17% fast. Not worth chasing the live refresh rate for a turbo. */
    private const val MACRO_FRAME_MS = 1000.0 / 60.0

    /** Shortest time a macro may hold a pad state. The emulated pad is sampled on the VM's own
     *  schedule (~1 frame), so anything briefer can fall between samples and vanish — see the
     *  same ~24ms floor used for synthesized keyboard/pad input elsewhere. */
    private const val MACRO_MIN_STATE_MS = 24L

    private val macroHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private val macroRunnables = HashMap<String, Runnable>()

    /**
     * Press ([down]) or release a macro, honouring its [macroFrequency].
     *
     * [emit] sends ONE button, so each call site keeps its own routing — the on-screen
     * buttons go through setPadButton, a physical trigger through sendKeyAction with a
     * player port. [key] separates those concurrent uses of the same macro (touch and a
     * pad, or two ports) so one release can't cancel another's turbo.
     *
     * Release ALWAYS emits an up for every button, even mid-toggle: a macro let go on the
     * "on" half of its cycle would otherwise leave the buttons stuck down.
     */
    fun fireMacro(id: TouchButtonId, key: String, down: Boolean, emit: (Int, Boolean) -> Unit) {
        val codes = macroCodes(id)
        if (codes.isEmpty()) return
        // Pressure is a modifier, not a button: it makes the OTHER buttons send a soft
        // press. So it's held for the whole macro rather than emitted — and it is
        // deliberately NOT toggled by the turbo below, since a modifier that flickers
        // would just alternate the press strength.
        val wantsPressure = MACRO_CODE_PRESSURE in codes
        val buttons = codes.filter { it != MACRO_CODE_PRESSURE }
        val runKey = "${id.name}:$key"
        if (!down) {
            macroRunnables.remove(runKey)?.let { macroHandler.removeCallbacks(it) }
            buttons.forEach { emit(it, false) }
            if (wantsPressure) pressureModifierHeld.value = false
            return
        }
        // Set BEFORE the buttons go down — pressureRangeFor is read at emit time, so the
        // order is what decides whether the press is soft.
        if (wantsPressure) pressureModifierHeld.value = true
        if (buttons.isEmpty()) return
        val frames = macroFrequency(id)
        if (frames <= 0) {
            buttons.forEach { emit(it, true) }
            return
        }
        // Key auto-repeat re-delivers DOWN while held; the first one owns the toggle.
        if (macroRunnables.containsKey(runKey)) return
        // ★ The floor is a SAMPLING limit, not a rounding nicety. The VM samples the pad on its
        // own schedule, so a state held shorter than ~24ms can land entirely between two samples
        // and never register — the turbo then looks dead rather than fast (reported against
        // NetherSX2, whose fast frequencies do fire). 16ms was below that threshold, so the
        // quickest settings emitted presses the emulated pad never saw.
        val periodMs = (frames * MACRO_FRAME_MS).toLong().coerceAtLeast(MACRO_MIN_STATE_MS)
        var pressed = false
        val runnable = object : Runnable {
            override fun run() {
                pressed = !pressed
                buttons.forEach { emit(it, pressed) }
                macroHandler.postDelayed(this, periodMs)
            }
        }
        macroRunnables[runKey] = runnable
        macroHandler.post(runnable)
    }

    /**
     * Rapid-fire for ONE button (#619), on the same timer and the same sampling floor as a
     * macro's Frequency.
     *
     * Separate from [fireMacro] rather than folded into it because a macro is a SET of codes
     * plus a pressure modifier, and collapsing a single button into that shape would mean
     * building a list to throw it away. What matters is that both share [macroRunnables], so a
     * release always finds and cancels the toggle it started, and [MACRO_MIN_STATE_MS], so the
     * fastest settings still produce presses the VM actually samples.
     *
     * [key] namespaces concurrent users of the same button the way it does for macros.
     */
    fun fireTurboButton(keycode: Int, key: String, down: Boolean, frames: Int, emit: (Int, Boolean) -> Unit) {
        val runKey = "btn$keycode:$key"
        if (!down) {
            macroRunnables.remove(runKey)?.let { macroHandler.removeCallbacks(it) }
            // Always emit the up, even mid-cycle: let go on the "on" half and the button would
            // otherwise stay down in the emulator.
            emit(keycode, false)
            return
        }
        if (frames <= 0) {
            emit(keycode, true)
            return
        }
        if (macroRunnables.containsKey(runKey)) return // already firing
        val periodMs = (frames * MACRO_FRAME_MS).toLong().coerceAtLeast(MACRO_MIN_STATE_MS)
        var pressed = false
        val runnable = object : Runnable {
            override fun run() {
                pressed = !pressed
                emit(keycode, pressed)
                macroHandler.postDelayed(this, periodMs)
            }
        }
        macroRunnables[runKey] = runnable
        macroHandler.post(runnable)
    }

    /** The macro a physical [keycode] triggers — only if it's bound AND has buttons
     *  configured. Checked in the gameplay key path (Main) before normal pad routing. */
    @Volatile private var runtimeMacroMap: Map<Int, TouchButtonId>? = null

    fun invalidateRuntimeMacroCache() {
        runtimeMacroMap = null
    }

    private fun runtimeMacroMap(): Map<Int, TouchButtonId> =
        runtimeMacroMap ?: synchronized(this) {
            runtimeMacroMap ?: buildMap {
                listOf(TouchButtonId.MACRO1, TouchButtonId.MACRO2, TouchButtonId.MACRO3, TouchButtonId.MACRO4)
                    .forEach { id ->
                        val physical = macroPhysicalCode(id)
                        if (physical != KeyEvent.KEYCODE_UNKNOWN &&
                            macroCodes(id).isNotEmpty() &&
                            !containsKey(physical)
                        ) {
                            put(physical, id)
                        }
                    }
            }.also { runtimeMacroMap = it }
        }

    fun warmRuntimeMacroCache() {
        runtimeMacroMap()
    }

    fun macroForPhysicalCode(keycode: Int): TouchButtonId? {
        if (keycode == KeyEvent.KEYCODE_UNKNOWN) return null
        return runtimeMacroMap()[keycode]
    }

    /** Set true once load() has run — used to avoid clobbering disk state
     *  on first composition. */
    private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        loaded = true
        load()
    }

    private fun load() {
        val raw = MainActivityRuntime.prefs.getString(KEY_PROFILES, null)
        val list = mutableListOf<TouchProfile>()
        if (raw != null) {
            runCatching {
                val arr = JSONArray(raw)
                for (i in 0 until arr.length()) {
                    val obj = arr.getJSONObject(i)
                    list.add(TouchProfile.fromJson(obj))
                }
            }
        }
        // Merge in portable profiles from <DataRoot>/inputprofiles/ that aren't
        // already in prefs — lets profiles survive a data-folder move (the user's
        // pain point) and be shared/hand-dropped. Prefs stays the live source.
        importFolderProfilesInto(list)
        if (list.isEmpty()) {
            list.add(TouchProfile("Default", TouchLayout.default()))
        }
        profiles.clear()
        profiles.addAll(list)

        val active = MainActivityRuntime.prefs.getString(KEY_ACTIVE, list.first().name) ?: list.first().name
        activeProfileName.value = active
        val match = list.firstOrNull { it.name == active } ?: list.first()
        activeLayout.value = match.layoutFor(portrait.value).copy()
        opacity.floatValue = MainActivityRuntime.prefs.getFloat(KEY_OPACITY, 0.55f).coerceIn(0.0f, 1.0f)
        pressurePercent.intValue = MainActivityRuntime.prefs.getInt(KEY_PRESSURE_PERCENT, 50).coerceIn(5, 95)
        faceMultiTouch.value = MainActivityRuntime.prefs.getBoolean(KEY_FACE_MULTI, true)
        gestureEnabled.value = MainActivityRuntime.prefs.getBoolean(KEY_GESTURE_ON, false)
        gestureSwipeUp.intValue = MainActivityRuntime.prefs.getInt(KEY_GESTURE_UP, 0)
        gestureSwipeDown.intValue = MainActivityRuntime.prefs.getInt(KEY_GESTURE_DOWN, 0)
        gestureSwipeLeft.intValue = MainActivityRuntime.prefs.getInt(KEY_GESTURE_LEFT, 0)
        gestureSwipeRight.intValue = MainActivityRuntime.prefs.getInt(KEY_GESTURE_RIGHT, 0)
        gestureSwipeSensitivity.floatValue =
            MainActivityRuntime.prefs.getFloat(KEY_GESTURE_SENS, 0.17f).coerceIn(0.05f, 0.60f)
        gestureDoubleTap.intValue = MainActivityRuntime.prefs.getInt(KEY_GESTURE_DTAP, 0)
        gestureDoubleTapHold.value = MainActivityRuntime.prefs.getBoolean(KEY_GESTURE_DTAP_HOLD, false)
        touchGliding.value = MainActivityRuntime.prefs.getBoolean(KEY_TOUCH_GLIDING, false)
        touchHaptics.value = MainActivityRuntime.prefs.getBoolean(KEY_TOUCH_HAPTICS, true)
        multiTouchRadius.floatValue = MainActivityRuntime.prefs.getFloat(KEY_MULTI_RADIUS, 0.62f).coerceIn(0.50f, 0.95f)
        dpadSpacing.floatValue = MainActivityRuntime.prefs.getFloat(KEY_DPAD_SPACING, 0.0f).coerceIn(0.0f, 0.35f)
        floatingStick.value = MainActivityRuntime.prefs.getBoolean(KEY_FLOATING_STICK, false)
        fullHalfSticks.value = MainActivityRuntime.prefs.getBoolean(KEY_FULL_HALF_STICKS, false)
        analogExtraEnabled.value = MainActivityRuntime.prefs.getBoolean(KEY_ANALOG_EXTRA, false)
        analogExtraKeycode.intValue = MainActivityRuntime.prefs.getInt(KEY_ANALOG_EXTRA_CODE, 96)
        analogExtraDistance.floatValue =
            MainActivityRuntime.prefs.getFloat(KEY_ANALOG_EXTRA_DIST, 0.35f).coerceIn(0.1f, 1.5f)
        gridSnap.value = MainActivityRuntime.prefs.getBoolean(KEY_GRID_SNAP, false)
        visibilityMode.intValue = MainActivityRuntime.prefs.getInt(KEY_VIS_MODE, 11).coerceIn(0, 11)
        if (visibilityMode.intValue == 0) visible.value = false
        // #357: show/hide became tap-to-reveal (inverted). Seed the new pref from the old one so
        // anyone who had the button hidden keeps it hidden — now as tap-to-reveal, which still
        // opens the menu. Defaults chain to false (glyph visible) for everyone else.
        pauseTapToReveal.value = MainActivityRuntime.prefs.getBoolean(
            KEY_PAUSE_TAP_REVEAL, !MainActivityRuntime.prefs.getBoolean(KEY_SHOW_PAUSE, true))

        // One-shot 2.4.7 defaults migration: existing users have saved prefs/layouts
        // that predate the new defaults (multi-touch was off, the Pressure button was
        // visible), so the default flips above don't reach them. Apply once; after this
        // the user's own choices stick.
        if (!MainActivityRuntime.prefs.getBoolean(KEY_DEFAULTS_MIGRATED_247, false)) {
            faceMultiTouch.value = true
            fun hidePressure(layout: TouchLayout): TouchLayout = layout.copy(
                buttons = layout.buttons.map {
                    if (it.id.kind == TouchButtonId.Kind.PRESSURE) it.copy(enabled = false) else it
                }
            )
            for (i in profiles.indices) profiles[i] = profiles[i].copy(layout = hidePressure(profiles[i].layout))
            activeLayout.value = hidePressure(activeLayout.value)
            MainActivityRuntime.prefs.edit { putBoolean(KEY_DEFAULTS_MIGRATED_247, true) }
            persist()
        }

        // #357 one-shot (pass 2): the pause hotspot moved from an invisible dead-centre long-press
        // zone to a visible top-right single-tap button. Pass 1 only rewrote the GLOBAL profiles,
        // so every per-game / per-CRC / per-orientation layout (touch.layout.game.*, loaded lazily
        // when a game boots) kept the old centred hotspot — and now that it draws a real glyph it
        // landed a big ⏸ in the middle of the screen. This pass sweeps every stored layout. A
        // widget the user deliberately moved keeps its spot; new/reset layouts get default().
        if (!MainActivityRuntime.prefs.getBoolean(KEY_PAUSE_TOPRIGHT_MIGRATED_3, false)) {
            for (i in profiles.indices) profiles[i] = profiles[i].copy(layout = relocatePause(profiles[i].layout))
            activeLayout.value = relocatePause(activeLayout.value)

            val storedLayouts = MainActivityRuntime.prefs.all
                .filterKeys { it.startsWith(KEY_LAYOUT_GAME_PREFIX) }
            if (storedLayouts.isNotEmpty()) {
                MainActivityRuntime.prefs.edit {
                    for ((key, value) in storedLayouts) {
                        val raw = value as? String ?: continue
                        val fixed = runCatching {
                            relocatePause(TouchLayout.fromJson(JSONObject(raw))).toJson().toString()
                        }.getOrNull() ?: continue
                        putString(key, fixed)
                    }
                }
            }
            MainActivityRuntime.prefs.edit { putBoolean(KEY_PAUSE_TOPRIGHT_MIGRATED_3, true) }
            persist()
        }
    }

    /** Move a legacy pause widget off a spot we shipped and out of the old hotspot size. Shared
     *  by the one-shot migration across global profiles and every per-game layout blob. */
    private fun relocatePause(layout: TouchLayout): TouchLayout = layout.copy(
        buttons = layout.buttons.map {
            if (it.id != TouchButtonId.PAUSE) return@map it
            // Two positions we shipped and then moved off: the original invisible-hotspot spot
            // (dead centre, terrible once it draws a real glyph), and the first top-right attempt,
            // which landed on top of R2. Anywhere else is the user's own placement — left alone.
            val legacyCentre = kotlin.math.abs(it.xFrac - 0.48f) < 0.10f &&
                               kotlin.math.abs(it.yFrac - 0.50f) < 0.10f
            val overlappedR2Corner = kotlin.math.abs(it.xFrac - 0.955f) < 0.04f &&
                                     kotlin.math.abs(it.yFrac - 0.055f) < 0.04f
            when {
                legacyCentre || overlappedR2Corner ->
                    it.copy(xFrac = PAUSE_DEFAULT_X, yFrac = PAUSE_DEFAULT_Y, sizeDp = PAUSE_DEFAULT_DP)
                // Sized as an invisible touch target rather than an icon — shrink it to the
                // visible default wherever the user has since dragged it to.
                it.sizeDp > 72f -> it.copy(sizeDp = PAUSE_DEFAULT_DP)
                else -> it
            }
        }
    )

    private fun persist() {
        val arr = JSONArray()
        for (p in profiles) arr.put(p.toJson())
        MainActivityRuntime.prefs.edit {
            putString(KEY_PROFILES, arr.toString())
                .putString(KEY_ACTIVE, activeProfileName.value)
                .putFloat(KEY_OPACITY, opacity.floatValue)
                .putBoolean(KEY_FACE_MULTI, faceMultiTouch.value)
                .putBoolean(KEY_GESTURE_ON, gestureEnabled.value)
                .putInt(KEY_GESTURE_UP, gestureSwipeUp.intValue)
                .putInt(KEY_GESTURE_DOWN, gestureSwipeDown.intValue)
                .putInt(KEY_GESTURE_LEFT, gestureSwipeLeft.intValue)
                .putInt(KEY_GESTURE_RIGHT, gestureSwipeRight.intValue)
                .putFloat(KEY_GESTURE_SENS, gestureSwipeSensitivity.floatValue)
                .putInt(KEY_GESTURE_DTAP, gestureDoubleTap.intValue)
                .putBoolean(KEY_GESTURE_DTAP_HOLD, gestureDoubleTapHold.value)
                .putBoolean(KEY_TOUCH_GLIDING, touchGliding.value)
                .putBoolean(KEY_TOUCH_HAPTICS, touchHaptics.value)
                .putFloat(KEY_MULTI_RADIUS, multiTouchRadius.floatValue)
                .putFloat(KEY_DPAD_SPACING, dpadSpacing.floatValue)
                .putBoolean(KEY_FLOATING_STICK, floatingStick.value)
                .putBoolean(KEY_FULL_HALF_STICKS, fullHalfSticks.value)
                .putBoolean(KEY_GRID_SNAP, gridSnap.value)
                .putInt(KEY_VIS_MODE, visibilityMode.intValue)
                .putBoolean(KEY_PAUSE_TAP_REVEAL, pauseTapToReveal.value)
        }
        syncFolder()
    }

    /** Set the on-screen controls visibility mode (see [visibilityMode]). */
    fun setVisibilityMode(mode: Int) {
        visibilityMode.intValue = mode.coerceIn(0, 11)
        // Reflect immediately: Never hides; any other mode shows.
        visible.value = visibilityMode.intValue != 0
        interactionTick.intValue++
        persist()
    }

    /** Note a touch interaction (screen tap or on-screen button press): show
     *  the controls (unless disabled) and restart the auto-hide timer. */
    fun noteTouchInteraction() {
        if (visibilityMode.intValue == 0) return
        if (!visible.value) visible.value = true
        interactionTick.intValue++
    }

    /**
     * How many on-screen controls are held right now. The auto-hide timer must not fire while
     * this is non-zero.
     *
     * Every handler bumped [interactionTick] on press-DOWN and then sat in a hold loop that never
     * ticked again, so holding a button for longer than the timeout hid the controls with the
     * user's finger still on them — mid-fight, mid-corner. "Auto-hide after N seconds" is
     * supposed to mean N seconds without a touch, and a held button IS a touch.
     *
     * Balanced with try/finally at every call site so a cancelled gesture cannot strand a count
     * and pin the controls on for the rest of the session.
     */
    val activeHolds = mutableIntStateOf(0)

    fun beginTouchHold() {
        if (visibilityMode.intValue == 0) return
        activeHolds.intValue++
        noteTouchInteraction()
    }

    fun endTouchHold() {
        if (activeHolds.intValue > 0) activeHolds.intValue--
        // Restart the countdown from the moment of RELEASE, not from the press: the user was
        // still using the pad for the whole hold.
        noteTouchInteraction()
    }

    /** Commit the live edit. When a game is running, store the edited layout as
     *  that game's OWN per-serial layout (touch.layout.game.<serial>) so it is
     *  isolated from every other game and from the shared profiles — this is what
     *  stops one game's edit from bleeding into the next. When no game is running
     *  (library/global edit), fall back to overwriting the active profile so the
     *  global Default still reflects the edit. */
    /** Re-read the active PROFILE for the current orientation.
     *
     *  The in-game path re-applies on rotate via applyForSerial, but that only runs while a VM is
     *  up — so a global-scope edit kept whichever orientation's layout happened to be loaded when
     *  the screen opened, and rotating showed the wrong one. */
    fun applyActiveProfileForOrientation() {
        val match = profiles.firstOrNull { it.name == activeProfileName.value } ?: return
        activeLayout.value = match.layoutFor(portrait.value).copy()
        selectedButton.value = null
    }

    fun saveLiveLayoutToActive() {
        // gameIsRunning() is the OUTER gate. The per-serial/CRC isolation paths
        // must only run when a VM is actually up — keying off a merely-non-null
        // serial was wrong because MainActivityRuntime.currentGame (hence runningSerial()) stays
        // stale after Close Game, so a Global-Default edit from the main menu
        // would silently write into the LAST-PLAYED game's per-serial key.
        if (gameIsRunning()) {
            val serial = runningSerial()
            if (serial != null) {
                // In-game with a resolved serial -> isolated per-serial layout.
                // Never touches any shared profile.
                MainActivityRuntime.prefs.edit {
                    putString(
                        orient(KEY_LAYOUT_GAME_PREFIX + serial),
                        activeLayout.value.toJson().toString(),
                    )
                }
            } else {
                // In-game but no serial (homebrew / BIOS / serial-less disc).
                // Key by disc CRC so it stays isolated; if even the CRC is
                // unknown, no-op with a warning rather than corrupting Default.
                val crc = runCatching { NativeApp.getGameCRC() }.getOrNull()
                    ?.trim()?.uppercase()?.takeIf { it.isNotEmpty() && it != "00000000" }
                if (crc != null) {
                    MainActivityRuntime.prefs.edit {
                        putString(
                            orient(KEY_LAYOUT_GAME_PREFIX + "crc." + crc),
                            activeLayout.value.toJson().toString(),
                        )
                    }
                } else {
                    println(
                        "@@ARMSX2_TOUCH@@ refusing to save in-game layout to global " +
                            "Default: no serial/CRC resolved for running game"
                    )
                }
            }
        } else {
            // No game running (library / Global Default edit) -> overwrite the
            // active profile.
            val idx = profiles.indexOfFirst { it.name == activeProfileName.value }
            if (idx >= 0) {
                // Only the orientation currently being edited. Writing .copy(layout = ...) here
                // unconditionally is what made landscape and portrait clobber each other.
                profiles[idx] = profiles[idx].withLayoutFor(portrait.value, activeLayout.value.copy())
                persist()
            }
        }
        selectedButton.value = null
    }

    /** Persist the live edit state under a new profile name. If the name
     *  collides, the existing profile is overwritten. Switches to the new
     *  profile. */
    fun saveAsNewProfile(name: String) {
        val trimmed = name.trim().ifEmpty { return }
        val newProf = TouchProfile(trimmed, activeLayout.value.copy())
        val existing = profiles.indexOfFirst { it.name == trimmed }
        if (existing >= 0) profiles[existing] = newProf
        else profiles.add(newProf)
        activeProfileName.value = trimmed
        // Bind the new profile to the game currently running so it only applies
        // to THAT game (otherwise it becomes the globally-active profile and
        // bleeds into every other game's boot). No game running -> global only.
        // Gate on gameIsRunning() so a stale MainActivityRuntime.currentGame (after Close Game)
        // can't bind this profile to the last-played game from the library.
        val serial = if (gameIsRunning()) runningSerial() else null
        if (serial != null)
            MainActivityRuntime.prefs.edit { putString(KEY_ACTIVE_GAME_PREFIX + serial, trimmed) }
        persist()
    }

    fun switchProfile(name: String) {
        val match = profiles.firstOrNull { it.name == name } ?: return
        activeProfileName.value = name
        activeLayout.value = match.layout.copy()
        // When a game is running, remember this profile FOR that game so it
        // auto-applies on the next boot (per-game tier). With no game (library),
        // it's just the global default, persisted via KEY_ACTIVE in persist().
        // Gate on gameIsRunning() so a stale MainActivityRuntime.currentGame (after Close Game)
        // can't bind this from the library to the last-played game.
        val serial = if (gameIsRunning()) runningSerial() else null
        if (serial != null)
            MainActivityRuntime.prefs.edit { putString(KEY_ACTIVE_GAME_PREFIX + serial, name) }
        persist()
    }

    fun deleteProfile(name: String) {
        if (profiles.size <= 1) return  // never delete the last profile
        val idx = profiles.indexOfFirst { it.name == name }
        if (idx < 0) return
        profiles.removeAt(idx)
        if (activeProfileName.value == name) {
            val fallback = profiles.first()
            activeProfileName.value = fallback.name
            activeLayout.value = fallback.layout.copy()
        }
        // Drop any per-game overrides that pointed at the deleted profile.
        clearGameOverridesFor(name)
        persist()
    }

    fun resetActiveToDefault() {
        // Per-orientation: resetting in portrait must not hand back the landscape arrangement,
        // whose fractions put the sticks and face buttons over the picture.
        activeLayout.value = TouchLayout.defaultFor(portrait.value)
    }

    /** True when a VM is up (RUNNING or PAUSED) — i.e. an in-game edit, where we
     *  must NEVER fall back to overwriting the shared Default profile. */
    private fun gameIsRunning(): Boolean =
        MainActivityRuntime.eState.value == EmuState.RUNNING || MainActivityRuntime.eState.value == EmuState.PAUSED

    /** Authoritative serial of the booted disc straight from the core. Returns a
     *  clean "AAAA-NNNNN" with no CRC/paren formatting, regardless of how the
     *  game was launched (library card, raw path, file picker, external). Empty
     *  string from native = no disc loaded. */
    private fun coreSerial(): String? =
        runCatching { NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotEmpty() }

    /** Serial of the game currently running. Source order:
     *   1. launch-time GameInfo.serial (set before the VM starts) — lets per-game
     *      binding work from the first edit;
     *   2. the AUTHORITATIVE core disc serial (VMManager::GetDiscSerial via JNI),
     *      which knows the serial even when the filename had no serial token and
     *      the overlay never populated InGameOverlay.currentSerial;
     *   3. InGameOverlay.currentSerial as a last resort (may be a formatted
     *      "SERIAL (CRC)" string — only use if the cleaner sources are empty). */
    private fun runningSerial(): String? =
        MainActivityRuntime.currentGame.value?.serial?.takeIf { it.isNotEmpty() }
            ?: coreSerial()
            ?: InGameOverlay.currentSerial.value?.takeIf { it.isNotEmpty() }

    // ---- Per-game tier + portable inputprofiles/ folder ----

    /** Apply this game's touch layout on boot. Precedence:
     *   1. A per-serial CUSTOM layout saved by the drag-and-Save editor
     *      (touch.layout.game.<serial>) — fully isolated per game.
     *   2. A legacy per-game profile-NAME binding (touch.active.game.<serial>),
     *      written by the Profiles dialog — load that profile's layout.
     *   3. No binding at all -> reset to the "Default" profile so an un-customized
     *      game shows the default layout, NOT whatever named profile is globally
     *      active (otherwise a "GT4" profile bleeds into every other game).
     *  Does NOT persist (auto-apply must not overwrite the global selection). */
    fun applyForSerial(serial: String?) {
        // Fall back to the authoritative core serial when the caller's serial is
        // null/empty (filename had no serial token) so a per-serial layout still
        // re-applies regardless of launch path.
        val effSerial = serial?.takeIf { it.isNotEmpty() } ?: coreSerial()
        if (effSerial == null) {
            // No serial — try the per-disc CRC layout key used by the in-game save
            // safety net before giving up.
            val crc = runCatching { NativeApp.getGameCRC() }.getOrNull()
                ?.trim()?.uppercase()?.takeIf { it.isNotEmpty() && it != "00000000" }
            if (crc != null) {
                val rawCrc = readGameLayoutJson(KEY_LAYOUT_GAME_PREFIX + "crc." + crc)
                if (rawCrc != null) {
                    runCatching { TouchLayout.fromJson(JSONObject(rawCrc)) }.getOrNull()?.let {
                        activeProfileName.value = "Default"
                        activeLayout.value = it
                        return
                    }
                }
            }
            return
        }
        // (1) Per-serial custom layout.
        val rawLayout = readGameLayoutJson(KEY_LAYOUT_GAME_PREFIX + effSerial)
        if (rawLayout != null) {
            runCatching { TouchLayout.fromJson(JSONObject(rawLayout)) }.getOrNull()?.let {
                activeProfileName.value = "Default"
                activeLayout.value = it
                return
            }
        }
        // (2) Legacy per-game profile-name binding (Profiles dialog).
        val name = MainActivityRuntime.prefs.getString(KEY_ACTIVE_GAME_PREFIX + effSerial, null)
        if (name != null) {
            val match = profiles.firstOrNull { it.name == name }
            if (match != null) {
                activeProfileName.value = name
                activeLayout.value = match.layout.copy()
                return
            }
        }
        // (3) No per-game record: reset to the "Default" profile (NOT the globally
        //     active profile) so an un-customized game never inherits another
        //     game's named layout.
        val def = profiles.firstOrNull { it.name == "Default" } ?: profiles.firstOrNull()
        if (def != null) {
            activeProfileName.value = def.name
            activeLayout.value = def.layout.copy()
        } else {
            activeProfileName.value = "Default"
            activeLayout.value = TouchLayout.default()
        }
    }

    private fun clearGameOverridesFor(profileName: String) {
        MainActivityRuntime.prefs.edit {
            for ((k, v) in MainActivityRuntime.prefs.all) {
                if (k.startsWith(KEY_ACTIVE_GAME_PREFIX) && v == profileName) remove(k)
            }
        }
    }

    /** Clear any per-serial custom layout for [serial] so the game reverts to the
     *  active/Default profile on next boot (used by the editor's Reset chip). */
    fun clearGameLayout(serial: String?) {
        if (serial == null) return
        // Reset clears BOTH orientations for the game — a per-axis reset would be a
        // surprise ("I reset and the other orientation kept my old layout").
        MainActivityRuntime.prefs.edit {
            remove(KEY_LAYOUT_GAME_PREFIX + serial)
            remove(KEY_LAYOUT_GAME_PREFIX + serial + ".portrait")
        }
    }

    /** Reset chip: only clear the running game's per-serial layout when a VM is
     *  actually up. From the library (Global Default edit) there is no per-game
     *  key to clear, and MainActivityRuntime.currentGame may still point at the last-played
     *  game — so resolving a serial there would wrongly delete that game's
     *  custom layout. */
    fun clearGameLayoutIfRunning() {
        if (gameIsRunning()) clearGameLayout(runningSerial())
    }

    /** `<DataRoot>/inputprofiles/` (native creates it on init). Null when no
     *  system dir is configured yet. Shared with the controller-mapping profiles,
     *  which mirror into the same folder under a different suffix. */
    private fun profilesDir(): File? = MainActivityRuntime.inputProfilesDir()

    private fun fileNameFor(name: String): String =
        name.replace(Regex("[^A-Za-z0-9 _.-]"), "_") + PROFILE_FILE_SUFFIX

    /** Mirror the in-memory profiles to the portable folder: write each one and
     *  delete orphaned files (covers delete/rename). Best-effort. */
    private fun syncFolder() {
        val dir = profilesDir() ?: return
        runCatching {
            val want = HashMap<String, TouchProfile>()
            for (p in profiles) want[fileNameFor(p.name)] = p
            for ((fn, p) in want) File(dir, fn).writeText(p.toJson().toString())
            dir.listFiles { f -> f.name.endsWith(PROFILE_FILE_SUFFIX) }?.forEach { f ->
                if (f.name !in want) runCatching { f.delete() }
            }
        }
    }

    /** Add portable folder profiles not already present (by name) into [list]. */
    private fun importFolderProfilesInto(list: MutableList<TouchProfile>) {
        val dir = profilesDir() ?: return
        runCatching {
            val have = list.map { it.name }.toHashSet()
            dir.listFiles { f -> f.name.endsWith(PROFILE_FILE_SUFFIX) }
                ?.sortedBy { it.name }
                ?.forEach { f ->
                    runCatching {
                        val prof = TouchProfile.fromJson(JSONObject(f.readText()))
                        if (prof.name !in have) { list.add(prof); have.add(prof.name) }
                    }
                }
        }
    }

    /** Reload the live layout from the saved active profile, discarding
     *  any unsaved edits. */
    fun discardEdits() {
        val match = profiles.firstOrNull { it.name == activeProfileName.value }
        if (match != null) activeLayout.value = match.layout.copy()
        selectedButton.value = null
    }

    fun setOpacity(o: Float) {
        opacity.floatValue = o.coerceIn(0.0f, 1.0f)
        persist()
    }

    fun setPauseTapToReveal(reveal: Boolean) {
        pauseTapToReveal.value = reveal
        persist()
    }

    fun setFaceMultiTouch(enabled: Boolean) {
        faceMultiTouch.value = enabled
        persist()
    }

    // ---- Gesture control setters ---------------------------------------------
    fun setGestureEnabled(enabled: Boolean) {
        gestureEnabled.value = enabled
        persist()
    }

    /** dir: 0=up 1=down 2=left 3=right. code 0 clears the assignment. */
    fun setGestureSwipe(dir: Int, code: Int) {
        when (dir) {
            0 -> gestureSwipeUp.intValue = code
            1 -> gestureSwipeDown.intValue = code
            2 -> gestureSwipeLeft.intValue = code
            3 -> gestureSwipeRight.intValue = code
        }
        persist()
    }

    fun setGestureSensitivity(v: Float) {
        gestureSwipeSensitivity.floatValue = v.coerceIn(0.05f, 0.60f)
        persist()
    }

    fun setGestureDoubleTap(code: Int) {
        gestureDoubleTap.intValue = code
        persist()
    }

    fun setGestureDoubleTapHold(hold: Boolean) {
        gestureDoubleTapHold.value = hold
        persist()
    }

    /**
     * Buttons a gesture may fire, as (keycode, label).
     *
     * Digital only — FACE, SHOULDER and START/SELECT. A swipe cannot meaningfully drive a stick or
     * a d-pad direction (those want a held magnitude, not a pulse), and the menu/state widgets are
     * app actions rather than PS2 buttons.
     */
    fun gestureAssignableButtons(): List<Pair<Int, String>> =
        TouchButtonId.values()
            .filter { it.kind == TouchButtonId.Kind.FACE || it.kind == TouchButtonId.Kind.SHOULDER }
            .map { it.keycode to it.label }

    fun setTouchGliding(enabled: Boolean) {
        touchGliding.value = enabled
        persist()
    }

    fun setTouchHaptics(enabled: Boolean) {
        touchHaptics.value = enabled
        persist()
    }

    fun setMultiTouchRadius(v: Float) {
        multiTouchRadius.floatValue = v.coerceIn(0.50f, 0.95f)
        persist()
    }

    fun setDpadSpacing(v: Float) {
        dpadSpacing.floatValue = v.coerceIn(0.0f, 0.35f)
        persist()
    }

    fun setFloatingStick(enabled: Boolean) {
        floatingStick.value = enabled
        persist()
    }

    fun setFullHalfSticks(enabled: Boolean) {
        fullHalfSticks.value = enabled
        persist()
    }

    fun setGridSnap(enabled: Boolean) {
        gridSnap.value = enabled
        persist()
    }

    /** Update a single button in the live layout. */
    fun updateButton(id: TouchButtonId, transform: (TouchButtonCfg) -> TouchButtonCfg) {
        val current = activeLayout.value
        val newButtons = current.buttons.map { if (it.id == id) transform(it) else it }
        activeLayout.value = current.copy(buttons = newButtons)
    }

    /** Latched off the touch controls when a controller key/axis fires.
     *  Only in "Auto" mode (11) — when an auto-hide timeout is set (1..10) the
     *  timer owns hiding, and "Never" (0) is already hidden. Idempotent. */
    fun onControllerInputDetected() {
        if (visibilityMode.intValue == 11 && visible.value) visible.value = false
    }

    /** Latched on by any pointer-down on the surface so a controller user
     *  who touches the screen sees the controls again (and restarts the
     *  auto-hide timer). No-op when controls are disabled. */
    fun onSurfaceTouched() {
        noteTouchInteraction()
    }
}

/** Stable id for a touch widget. The keycode is the canonical primary
 *  keycode the widget emits (digital buttons emit one code; the DPad +
 *  sticks emit four codes derived from the four cardinal directions —
 *  the keycode here is the "up" / first code for serialization id
 *  purposes only, the rendering layer maps internally). */
enum class TouchButtonId(val label: String, val keycode: Int, val kind: Kind) {
    CROSS("✕", KeyEvent.KEYCODE_BUTTON_A, Kind.FACE),
    CIRCLE("○", KeyEvent.KEYCODE_BUTTON_B, Kind.FACE),
    SQUARE("□", KeyEvent.KEYCODE_BUTTON_X, Kind.FACE),
    TRIANGLE("△", KeyEvent.KEYCODE_BUTTON_Y, Kind.FACE),
    L1("L1", KeyEvent.KEYCODE_BUTTON_L1, Kind.SHOULDER),
    R1("R1", KeyEvent.KEYCODE_BUTTON_R1, Kind.SHOULDER),
    L2("L2", KeyEvent.KEYCODE_BUTTON_L2, Kind.SHOULDER),
    R2("R2", KeyEvent.KEYCODE_BUTTON_R2, Kind.SHOULDER),
    START("Start", KeyEvent.KEYCODE_BUTTON_START, Kind.MENU),
    SELECT("Select", KeyEvent.KEYCODE_BUTTON_SELECT, Kind.MENU),
    // L3 / R3 — separate stick-CLICK buttons (the press-the-thumbstick
    // action). The L_STICK / R_STICK widgets only emit axis movement;
    // these emit the THUMBL / THUMBR keycodes.
    L3("L3", KeyEvent.KEYCODE_BUTTON_THUMBL, Kind.MENU),
    R3("R3", KeyEvent.KEYCODE_BUTTON_THUMBR, Kind.MENU),
    DPAD("D-Pad", KeyEvent.KEYCODE_DPAD_UP, Kind.DPAD),
    L_STICK("L-Stick", 110, Kind.STICK),
    R_STICK("R-Stick", 120, Kind.STICK),
    // Invisible long-press hotspot that opens the in-game pause overlay.
    // Replaced the old long-press-anywhere-on-the-surface gesture, which
    // fired on accidental presses in empty space mid-game. Emits no pad
    // keycode (0 = unused); renders nothing in play mode, shows an
    // outlined "PAUSE" box in edit mode so it can be moved/resized.
    PAUSE("Pause", 0, Kind.PAUSE),

    // On-screen fast-forward (Turbo) toggle. Emits no PS2 keycode; tapping it
    // toggles locked fast-forward via MainActivityRuntime.toggleFastForward() — the same action
    // as the FAST_FORWARD_TOGGLE hotkey. Opt-in: disabled in the default layout.
    // Rendered by FastForwardWidget.
    FAST_FORWARD("▶▶", 0, Kind.FASTFORWARD),

    // On-screen quick save-state / load-state buttons (to the active slot). Emit no
    // PS2 keycode; tapping calls MainActivityRuntime.saveState() / MainActivityRuntime.loadState() — the same actions
    // as the SAVE_STATE/LOAD_STATE hotkeys. Opt-in (disabled in the default layout).
    SAVE_STATE("SAVE", 0, Kind.STATEACTION),
    LOAD_STATE("LOAD", 0, Kind.STATEACTION),
    // Screenshot, same shape as the save/load buttons. Lives here rather than in the pause menu:
    // the core writes the PNG and confirms on the OSD, which is hidden while the menu is up, so a
    // menu entry left you staring at nothing until you backed out and wondered if it had worked.
    // On the overlay the confirmation appears immediately, where you are already looking.
    SCREENSHOT("SHOT", 0, Kind.STATEACTION),

    // Macro / combo buttons: each fires a user-chosen SET of pad buttons at once
    // (e.g. R1+R2+R3). Emits no keycode of its own; the set is configured per macro
    // (TouchControls.macroButtons) and dispatched by MacroWidget. Opt-in (disabled in
    // the default layout).
    MACRO1("M1", 0, Kind.MACRO),
    MACRO2("M2", 0, Kind.MACRO),
    MACRO3("M3", 0, Kind.MACRO),
    MACRO4("M4", 0, Kind.MACRO),

    // Pressure-sensitivity modifier. Emits no PS2 keycode; while held it sets
    // TouchControls.pressureModifierHeld so pressure-capable buttons report a
    // soft (~50%) press. Rendered by PressureButtonWidget.
    PRESSURE("P½", 0, Kind.PRESSURE),

    // Extra button that rides with the left analog stick (sprint / jump / glide).
    // It is a FULL layout widget — dragged and resized in the touch editor like any
    // other control — rather than a satellite positioned by angle/distance sliders,
    // which is what it used to be and which nobody could find or adjust. The stick
    // additionally hit-tests this widget's own circle so a thumb that glides up off
    // the stick latches it without lifting; see StickWidget. Its keycode is chosen in
    // Pad settings (analogExtraKeycode), so the enum entry carries none.
    ANALOG_EXTRA("Extra", 0, Kind.ANALOGEXTRA);

    enum class Kind { FACE, SHOULDER, MENU, DPAD, STICK, PAUSE, PRESSURE, FASTFORWARD, MACRO, STATEACTION, ANALOGEXTRA }
}

/** Position + size for a single widget. xFrac / yFrac are anchor-point
 *  fractions of screen width/height (0..1, 0,0 = top-left). sizeDp is
 *  the widget's outer diameter / largest side. */
data class TouchButtonCfg(
    val id: TouchButtonId,
    val xFrac: Float,
    val yFrac: Float,
    val sizeDp: Float,
    val enabled: Boolean = true,
    /** Tap-to-hold / latch: a tap toggles the button held (stays pressed until
     *  tapped again) instead of momentary press. Per-button, opt-in. */
    val tapToHold: Boolean = false,
    /** Rapid-fire while held (#619), in frames between toggles; 0 = off, which stays the
     *  default. Same unit and machinery as a macro's Frequency, because it is the same idea
     *  applied to one button: physical buttons already had turbo and the on-screen ones did
     *  not, which is exactly the asymmetry the request was about. Composes with [tapToHold] —
     *  set both and a tap starts the autofire and the next tap stops it. */
    val turbo: Int = 0,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id.name)
        put("x", xFrac.toDouble())
        put("y", yFrac.toDouble())
        put("size", sizeDp.toDouble())
        put("on", enabled)
        put("hold", tapToHold)
        put("turbo", turbo)
    }

    companion object {
        fun fromJson(json: JSONObject): TouchButtonCfg? {
            val idName = json.optString("id", "") ?: ""
            val id = runCatching { TouchButtonId.valueOf(idName) }.getOrNull() ?: return null
            return TouchButtonCfg(
                id = id,
                xFrac = json.optDouble("x", 0.5).toFloat().coerceIn(0f, 1f),
                yFrac = json.optDouble("y", 0.5).toFloat().coerceIn(0f, 1f),
                sizeDp = json.optDouble("size", 64.0).toFloat().coerceIn(28f, 220f),
                enabled = json.optBoolean("on", true),
                tapToHold = json.optBoolean("hold", false),
                turbo = json.optInt("turbo", 0).coerceIn(0, TouchControls.MACRO_FREQ_MAX),
            )
        }
    }
}

data class TouchLayout(val buttons: List<TouchButtonCfg>) {
    fun toJson(): JSONObject = JSONObject().apply {
        val arr = JSONArray()
        for (b in buttons) arr.put(b.toJson())
        put("buttons", arr)
    }

    companion object {
        fun fromJson(json: JSONObject): TouchLayout {
            val arr = json.optJSONArray("buttons") ?: return default()
            val list = mutableListOf<TouchButtonCfg>()
            for (i in 0 until arr.length()) {
                val obj = arr.optJSONObject(i) ?: continue
                TouchButtonCfg.fromJson(obj)?.let { list.add(it) }
            }
            // If the persisted layout is missing any new buttons added
            // since it was saved, splice in their defaults so the user
            // gets the new widget without having to reset. Matches
            // Settings.kt's optKey + fallback pattern.
            val have = list.map { it.id }.toSet()
            val merged = list + default().buttons.filter { it.id !in have }
            return TouchLayout(merged)
        }

        /** The default for [portrait], or the landscape one otherwise. Positions are screen
         *  fractions, so the landscape layout does not merely look cramped in portrait — it lands
         *  in the wrong half of a much taller window, on top of the game. */
        fun defaultFor(portrait: Boolean): TouchLayout =
            if (portrait) defaultPortrait() else default()

        /**
         * Portrait default: controls in the lower ~60% with the game above them.
         *
         * Laid out to a proposal from Isshin — shoulders paired at the top of the control area,
         * left stick and the face diamond in the middle band, D-pad and right stick along the
         * bottom, Select/Start centred between them and L3/R3 in the outer corners. Reproducing a
         * real pad's geography rather than compressing the landscape arrangement, which put the
         * sticks and face buttons over the picture.
         *
         * Everything sits at y >= 0.35 so nothing overlaps a top-aligned portrait render, and
         * nothing goes past y 0.95, which is where the system gesture bar lives.
         */
        fun defaultPortrait(): TouchLayout = TouchLayout(
            buttons = listOf(
                // Shoulders: paired top-left and top-right of the control area, L2/R2 above L1/R1.
                TouchButtonCfg(TouchButtonId.L2,       0.13f, 0.37f, 56f),
                TouchButtonCfg(TouchButtonId.L1,       0.13f, 0.46f, 56f),
                TouchButtonCfg(TouchButtonId.R2,       0.87f, 0.37f, 56f),
                TouchButtonCfg(TouchButtonId.R1,       0.87f, 0.46f, 56f),
                // Left stick mid-left; face diamond mid-right.
                TouchButtonCfg(TouchButtonId.L_STICK,  0.19f, 0.62f, 150f),
                TouchButtonCfg(TouchButtonId.TRIANGLE, 0.75f, 0.55f, 58f),
                TouchButtonCfg(TouchButtonId.SQUARE,   0.61f, 0.62f, 58f),
                TouchButtonCfg(TouchButtonId.CIRCLE,   0.89f, 0.62f, 58f),
                TouchButtonCfg(TouchButtonId.CROSS,    0.75f, 0.69f, 58f),
                // D-pad bottom-left, right stick bottom-right — the reverse of the band above, so
                // neither thumb has to cross the other.
                TouchButtonCfg(TouchButtonId.DPAD,     0.21f, 0.81f, 150f),
                TouchButtonCfg(TouchButtonId.R_STICK,  0.72f, 0.81f, 150f),
                // Select / Start centred at the bottom, L3 / R3 tucked into the outer corners.
                TouchButtonCfg(TouchButtonId.SELECT,   0.41f, 0.94f, 48f),
                TouchButtonCfg(TouchButtonId.START,    0.57f, 0.94f, 48f),
                TouchButtonCfg(TouchButtonId.L3,       0.10f, 0.94f, 44f),
                TouchButtonCfg(TouchButtonId.R3,       0.90f, 0.94f, 44f),
                // Opt-in extras keep the landscape default's disabled state and park in the gap
                // between the render and the shoulders, where they are grabbable in the editor.
                TouchButtonCfg(TouchButtonId.FAST_FORWARD, 0.30f, 0.36f, 44f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO1, 0.40f, 0.36f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO2, 0.48f, 0.36f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO3, 0.56f, 0.36f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO4, 0.64f, 0.36f, 42f, enabled = false),
                // Extra analog button. Portrait cannot copy the landscape placement: here the stick
                // sits directly ABOVE the D-pad and the shoulders above that, so the whole left
                // column is taken and "above the D-pad" would land inside the stick. Park it just
                // clear of the D-pad's right edge (which reaches x 0.40) instead, still an easy left
                // thumb reach and in open space. Drag it wherever you like — it is a normal widget.
                TouchButtonCfg(TouchButtonId.ANALOG_EXTRA, 0.44f, 0.72f, 48f),
            ).let { placed ->
                // Splice in anything the landscape default has that this list does not (pause,
                // pressure, save/load-state buttons...) so a new widget never goes missing in
                // portrait just because this table was written before it existed.
                val have = placed.map { it.id }.toSet()
                placed + default().buttons.filter { it.id !in have }
            },
        )

        /** Landscape-tuned default. Coordinates assume a 16:9-ish layout
         *  and are clamped to safe areas on edges. The user can drag in
         *  edit mode to fit their device — this is just the starting
         *  point. */
        fun default(): TouchLayout = TouchLayout(
            buttons = listOf(
                // DPad cluster — lower left of screen, above L-stick room
                TouchButtonCfg(TouchButtonId.DPAD,     0.10f, 0.55f, 150f),
                // Face button diamond — lower right
                TouchButtonCfg(TouchButtonId.TRIANGLE, 0.86f, 0.45f, 58f),
                TouchButtonCfg(TouchButtonId.SQUARE,   0.80f, 0.55f, 58f),
                TouchButtonCfg(TouchButtonId.CIRCLE,   0.92f, 0.55f, 58f),
                TouchButtonCfg(TouchButtonId.CROSS,    0.86f, 0.65f, 58f),
                // Shoulders stacked vertically on each side: L2 / R2 on
                // top (further trigger), L1 / R1 directly below them.
                // Gap is ~16% of screen height — on a 390dp landscape
                // height that's 62dp center-to-center, ~6dp visible gap
                // between the 56dp buttons. Tight enough to read as a
                // pair without overlapping.
                TouchButtonCfg(TouchButtonId.L2,       0.08f, 0.10f, 56f),
                TouchButtonCfg(TouchButtonId.L1,       0.08f, 0.23f, 56f),
                TouchButtonCfg(TouchButtonId.R2,       0.92f, 0.10f, 56f),
                TouchButtonCfg(TouchButtonId.R1,       0.92f, 0.23f, 56f),
                // Start / Select centered at the bottom
                TouchButtonCfg(TouchButtonId.SELECT,   0.45f, 0.92f, 48f),
                TouchButtonCfg(TouchButtonId.START,    0.55f, 0.92f, 48f),
                // Fast-forward + macro buttons — OPT-IN (disabled by default, so they
                // splice into existing layouts without changing them). Parked in a row
                // across the upper-middle (y 0.40) — clear of the edit-mode toolbar at
                // the top (so they're reachable to grab/enable) and above the main
                // controls. Users enable + reposition them in the editor; configure each
                // macro's button set in Pad settings → Touch Macros.
                TouchButtonCfg(TouchButtonId.FAST_FORWARD, 0.30f, 0.40f, 44f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO1, 0.40f, 0.40f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO2, 0.48f, 0.40f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO3, 0.56f, 0.40f, 42f, enabled = false),
                TouchButtonCfg(TouchButtonId.MACRO4, 0.64f, 0.40f, 42f, enabled = false),
                // Quick save/load-state buttons — also OPT-IN (disabled). Second row.
                TouchButtonCfg(TouchButtonId.SAVE_STATE, 0.30f, 0.54f, 44f, enabled = false),
                TouchButtonCfg(TouchButtonId.LOAD_STATE, 0.38f, 0.54f, 44f, enabled = false),
                TouchButtonCfg(TouchButtonId.SCREENSHOT, 0.46f, 0.54f, 44f, enabled = false),
                // Extra analog button, parked directly above the D-PAD (0.10, 0.55) — not above the
                // left stick, which is where it first went. The button is for sprint/jump held while
                // you keep moving, so it belongs above whichever control the thumb is already on,
                // and it sits in the gap between the shoulder column and the D-pad rather than out
                // in the middle of the screen. Visibility is owned by the Pad-settings toggle, not
                // this flag, so it is `enabled` here — see the ANALOG_EXTRA gate in the overlay's
                // widget loop.
                TouchButtonCfg(TouchButtonId.ANALOG_EXTRA, 0.10f, 0.34f, 48f),
                // Analog sticks — bottom inside, between DPad/face cluster
                // and the center, so thumb travel is short.
                TouchButtonCfg(TouchButtonId.L_STICK,  0.28f, 0.80f, 130f),
                TouchButtonCfg(TouchButtonId.R_STICK,  0.72f, 0.80f, 130f),
                // L3 / R3 stick-click buttons, anchored at the lower
                // outside corner of each thumbstick (away from the
                // screen center so the user's hand resting on the stick
                // doesn't accidentally press them).
                TouchButtonCfg(TouchButtonId.L3,       0.18f, 0.93f, 42f),
                TouchButtonCfg(TouchButtonId.R3,       0.82f, 0.93f, 42f),
                // Pause button (#357): visible single-tap ⏸ in the TOP-RIGHT corner, Nether-style.
                // Small and cornered (clear of every gameplay control) so a stray tap can't hit it
                // mid-game, and drawn transparent so it rides over the OSD without obscuring it.
                // "Show pause button" can hide the glyph — the tap zone stays live either way.
                TouchButtonCfg(TouchButtonId.PAUSE,    TouchControls.PAUSE_DEFAULT_X, TouchControls.PAUSE_DEFAULT_Y, TouchControls.PAUSE_DEFAULT_DP),
                // Pressure-modifier button — tucked under the D-pad (left side),
                // clear of the action. Hold it, then press a face/shoulder/d-pad
                // button for a ~50% (soft) press. Movable in overlay edit mode.
                TouchButtonCfg(TouchButtonId.PRESSURE, 0.10f, 0.78f, 44f, enabled = false),
            ),
        )
    }
}

/** [layout] is the LANDSCAPE authoring; [portraitLayout] is portrait's own, null until the user
 *  saves one. Per-game layouts got this via the ".portrait" key suffix (see orient()), but global
 *  profiles held a single layout — so editing in one orientation overwrote the other, which is
 *  exactly the "they are connected when they should be independent" report from Piixel and Isshin,
 *  and why it worked per-game and not globally.
 *
 *  Null portraitLayout falls back to landscape when read, matching readGameLayoutJson's seeding: a
 *  profile authored before this still shows something sensible in portrait, and the first portrait
 *  Save splits them for good. Absent from the JSON when null, so old profiles load unchanged. */
data class TouchProfile(
    val name: String,
    val layout: TouchLayout,
    val portraitLayout: TouchLayout? = null,
) {
    /** The authoring for [portrait], falling back to landscape when portrait has none yet. */
    fun layoutFor(portrait: Boolean): TouchLayout =
        if (portrait) (portraitLayout ?: layout) else layout

    /** Replace only the orientation being edited, leaving the other one alone. */
    fun withLayoutFor(portrait: Boolean, updated: TouchLayout): TouchProfile =
        if (portrait) copy(portraitLayout = updated) else copy(layout = updated)

    fun toJson(): JSONObject = JSONObject().apply {
        put("name", name)
        put("layout", layout.toJson())
        portraitLayout?.let { put("portraitLayout", it.toJson()) }
    }

    companion object {
        fun fromJson(json: JSONObject): TouchProfile {
            return TouchProfile(
                name = json.optString("name", "Profile"),
                layout = json.optJSONObject("layout")?.let { TouchLayout.fromJson(it) }
                    ?: TouchLayout.default(),
                portraitLayout = json.optJSONObject("portraitLayout")?.let { TouchLayout.fromJson(it) },
            )
        }
    }
}
