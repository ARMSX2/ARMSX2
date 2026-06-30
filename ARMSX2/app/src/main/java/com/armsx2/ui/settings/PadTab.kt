package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.Colors
import com.armsx2.ui.touch.TouchButtonId
import com.armsx2.ui.touch.TouchControls
import kr.co.iefriends.pcsx2.NativeApp

@Composable
fun PadTab(@Suppress("UNUSED_PARAMETER") state: MutableState<Settings>) {
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)
    val capture = remember { mutableStateOf<ControllerMappings.Action?>(null) }
    // Local co-op: which player's mapping this tab is editing (0 = P1, 1 = P2).
    val editPlayer = remember { mutableStateOf(0) }
    // Per-game controller settings (issue #246): the input-mapping layer (button
    // binds, stick modes, custom stick codes) follows the SAME Global/Game scope
    // the other tabs use — the ScopeToggle shown above this tab drives it. In Game
    // scope with a known serial, edits write per-game overrides; otherwise global.
    val padScope = com.armsx2.ui.InGameOverlay.settingsScope.value
    val padSerial = com.armsx2.ui.InGameOverlay.currentSerial.value?.takeIf { it.isNotEmpty() }
    val editSerial: String? = if (padScope == com.armsx2.config.SettingsScope.Game) padSerial else null
    // Bindings are CAPTURED asynchronously (arm, then press a button), so the write
    // tier is re-derived LIVE at capture time rather than from the composition-time
    // editSerial — guarantees a scope flip between arming and pressing still lands on
    // the tier shown. (Display rows use editSerial, which is correct at composition.)
    val liveEditSerial: () -> String? = {
        if (com.armsx2.ui.InGameOverlay.settingsScope.value == com.armsx2.config.SettingsScope.Game)
            com.armsx2.ui.InGameOverlay.currentSerial.value?.takeIf { it.isNotEmpty() } else null
    }
    val ctx = LocalContext.current
    val refreshToken = remember { mutableStateOf(0) }
    val focusRequester = remember { FocusRequester() }
    // Which macro's "configure button set" dialog is open (null = none).
    val macroDialogFor = remember { mutableStateOf<TouchButtonId?>(null) }
    // Which macro is capturing a physical-controller trigger button (null = none).
    val macroCapture = remember { mutableStateOf<TouchButtonId?>(null) }

    val stickCapture = ControllerMappings.captureStickDir
    LaunchedEffect(capture.value, stickCapture.value, macroCapture.value) {
        // Tell Main.dispatchKeyEvent to stop intercepting controller buttons for
        // overlay nav while we're capturing, so B/A/Y/etc. reach onPreviewKeyEvent
        // and bind instead of (e.g.) exiting the menu. An Action capture, a stick-
        // direction capture, or a macro physical-trigger capture arms the bypass.
        val capturingNow = capture.value != null || stickCapture.value != null || macroCapture.value != null
        ControllerMappings.padCapturing.value = capturingNow
        if (capturingNow)
            focusRequester.requestFocus()
    }
    // Safety: clear the bypass flag if the tab leaves composition mid-capture.
    DisposableEffect(Unit) {
        onDispose {
            ControllerMappings.padCapturing.value = false
            ControllerMappings.captureStickDir.value = null
        }
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .focusRequester(focusRequester)
            .focusable()
            .onPreviewKeyEvent { event ->
                // Macro physical-trigger capture: bind the pressed physical button to
                // fire this macro's button set. Captures the raw keycode (not a PS2
                // target) since macros override the button's normal mapping.
                val mc = macroCapture.value
                if (mc != null) {
                    if (event.type != KeyEventType.KeyDown)
                        return@onPreviewKeyEvent true
                    val ncode = event.key.nativeKeyCode
                    if (ncode == android.view.KeyEvent.KEYCODE_UNKNOWN)
                        return@onPreviewKeyEvent true
                    TouchControls.setMacroPhysicalCode(mc, ncode)
                    macroCapture.value = null
                    refreshToken.value++
                    return@onPreviewKeyEvent true
                }
                // Stick-direction CUSTOM capture: resolve the pressed physical
                // button to the PS2 code it drives (same physical->target lookup
                // as gameplay) and bind that direction. Shares this focusable and
                // the padCapturing bypass with the per-Action capture below.
                val sc = stickCapture.value
                if (sc != null) {
                    if (event.type != KeyEventType.KeyDown)
                        return@onPreviewKeyEvent true
                    val ncode = event.key.nativeKeyCode
                    if (ncode == android.view.KeyEvent.KEYCODE_UNKNOWN)
                        return@onPreviewKeyEvent true
                    // Prefer an ARMSX2 hotkey if the pressed button is already bound to
                    // one (Hotkeys tab) — that turns a freed-up stick direction into a
                    // Quick Save/Load State (etc.) trigger. Otherwise resolve to the PS2
                    // button the pressed control drives, exactly as before.
                    val hk = ControllerMappings.hotkeyFor(ncode)
                    val target = if (hk != null) ControllerMappings.stickCodeForHotkey(hk)
                        else ControllerMappings.stickCodeForPhysical(ncode, editPlayer.value)
                    if (target != null) {
                        ControllerMappings.setCustomStickCode(sc.first, sc.second, target, editPlayer.value, liveEditSerial())
                        ControllerMappings.endStickCapture()
                        refreshToken.value++
                    }
                    // If the pressed button isn't mapped to any pad Action or hotkey,
                    // keep waiting (swallow) rather than binding nothing.
                    return@onPreviewKeyEvent true
                }
                // Regular button capture — the menu button is captured in
                // Main.dispatchKeyEvent so it can grab BACK / back-paddle keys.
                val action = capture.value ?: return@onPreviewKeyEvent false
                if (event.type != KeyEventType.KeyDown)
                    return@onPreviewKeyEvent true
                val nativeKeyCode = event.key.nativeKeyCode
                if (nativeKeyCode == android.view.KeyEvent.KEYCODE_UNKNOWN)
                    return@onPreviewKeyEvent true
                ControllerMappings.bind(action, nativeKeyCode, editPlayer.value, liveEditSerial())
                capture.value = null
                refreshToken.value++
                true
            }
            .verticalScroll(scroll),
    ) {
        Text(
            "Tap an action, then press a physical controller button.",
            color = Color(0xFFBBBBBB),
            fontSize = 12.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 6.dp),
        )
        // Per-game scope hint (#246): the buttons / sticks below follow the
        // Global/Game toggle at the top of the menu. Spell out the current tier
        // here since this tab scrolls far from that toggle.
        Text(
            when {
                editSerial != null -> "● Editing controls for THIS GAME ($editSerial) — switch to Global up top to change all games."
                padSerial != null -> "○ Editing GLOBAL controls (all games). Switch to Game up top for a per-game map."
                else -> "○ Editing GLOBAL controls (all games)."
            },
            color = if (editSerial != null) Colors.pasx2_blue else Color(0xFF9A9A9A),
            fontSize = 11.sp,
            fontWeight = if (editSerial != null) FontWeight.Bold else FontWeight.Normal,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
        )
        SettingsDivider()
        // Open the on-screen touch-layout editor straight from here (no need to be
        // in-game). Closes the settings overlay and drops into edit mode over the
        // game/library. With no game running it edits the Global Default layout.
        Box(
            Modifier
                .fillMaxWidth()
                .height(34.dp)
                .background(rowAura())
                .clickable { com.armsx2.ui.InGameOverlay.editTouchLayout() }
                .controllerFocusable(
                    controllerId = "pad-edit-touch",
                    onConfirm = { com.armsx2.ui.InGameOverlay.editTouchLayout() },
                )
                .padding(horizontal = 6.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(
                "Edit On-Screen Touch Layout",
                color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold,
            )
        }
        SettingsDivider()
        // Loose state reads kept at the top of the grouped area so they always
        // recompose the tab regardless of which sections are collapsed.
        @Suppress("UNUSED_EXPRESSION") TouchControls.macroBindTick.value
        @Suppress("UNUSED_EXPRESSION")
        refreshToken.value
        // Also recompose when the mappings change externally (e.g. the global
        // "Reset to defaults" calls ControllerMappings.resetTunables, which bumps this)
        // so the feel sliders / stick modes refresh without re-opening the tab.
        @Suppress("UNUSED_EXPRESSION")
        ControllerMappings.stickBindTick.value
        CollapsibleSection("Macros", initiallyExpanded = false) {
            // Macros — 4 combo buttons, each firing a chosen SET of pad buttons at once
            // (e.g. R1+R2+R3). Tap a row to pick its buttons. Use them on-screen (enable +
            // position the M1-M4 buttons in the layout editor, off by default) and/or bind a
            // PHYSICAL controller button to fire the same macro ("Bind").
            Text(
                "Macros (combo buttons — touch + physical)",
                color = Color(0xFFBBBBBB),
                fontSize = 12.sp,
                modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
            )
            listOf(TouchButtonId.MACRO1, TouchButtonId.MACRO2, TouchButtonId.MACRO3, TouchButtonId.MACRO4).forEach { mid ->
                val buttons = TouchControls.macroButtons(mid)
                val summary = if (buttons.isEmpty()) "Not set" else buttons.joinToString(" + ") { it.label }
                val physCode = TouchControls.macroPhysicalCode(mid)
                val capturingThis = macroCapture.value == mid
                Row(
                    Modifier
                        .fillMaxWidth()
                        .height(44.dp)
                        .background(rowAura())
                        .clickable { macroDialogFor.value = mid }
                        .controllerFocusable(
                            controllerId = "pad-macro-${mid.name}",
                            onConfirm = { macroDialogFor.value = mid },
                        )
                        .padding(horizontal = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(mid.label, color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text(summary, color = Color(0xFFCCCCCC), fontSize = 12.sp)
                        Text(
                            when {
                                capturingThis -> "Press a controller button…"
                                physCode != android.view.KeyEvent.KEYCODE_UNKNOWN ->
                                    "Controller: ${ControllerMappings.labelForKey(physCode)}"
                                else -> "Controller: not bound"
                            },
                            color = if (capturingThis) Color(0xFFFFD33A) else Color(0xFF999999),
                            fontSize = 10.sp,
                        )
                    }
                    if (physCode != android.view.KeyEvent.KEYCODE_UNKNOWN && !capturingThis) {
                        Text(
                            "Clear",
                            color = Color(0xFFFF6B6B), fontSize = 11.sp, fontWeight = FontWeight.Bold,
                            modifier = Modifier
                                .clickable { TouchControls.clearMacroPhysicalCode(mid); refreshToken.value++ }
                                .padding(end = 10.dp),
                        )
                    }
                    Text(
                        if (capturingThis) "Cancel" else "Bind",
                        color = Colors.pasx2_blue, fontSize = 11.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable {
                                macroCapture.value = if (capturingThis) null else mid
                                capture.value = null
                                ControllerMappings.captureStickDir.value = null
                            }
                            .padding(end = 10.dp),
                    )
                    Text("Edit", color = Colors.pasx2_blue, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                }
                SettingsDivider()
            }
            macroDialogFor.value?.let { mid ->
                MacroConfigDialog(
                    macroId = mid,
                    onSaved = { refreshToken.value++ },
                    onDismiss = { macroDialogFor.value = null },
                )
            }
        }
        CollapsibleSection("Player & Rumble", initiallyExpanded = false) {
            // Local co-op: pick which player's buttons / stick mode you're editing. P2 is
            // the second controller to press a button in-game (auto-assigned). Stick
            // feel (deadzone / sensitivity / acceleration) and the D-pad-as-stick toggle
            // below are shared by both players.
            SegmentedRow(
                label = "Editing",
                options = listOf("Player 1", "Player 2"),
                selectedIndex = editPlayer.value,
                description = "Configure Player 1 or Player 2's button mapping. Player 2 = the 2nd controller that joins in-game.",
                onChange = {
                    editPlayer.value = it
                    capture.value = null
                    ControllerMappings.captureStickDir.value = null
                    refreshToken.value++
                },
            )
            // Master rumble / vibration enable — gates controller motors AND the device-haptic
            // fallback (NativeApp.onPadRumble). Off = no haptics anywhere.
            ToggleRow(
                "Rumble / Vibration",
                ControllerMappings.rumbleEnabled(),
                description = "Master switch for controller rumble and the device's built-in vibration. Turn off to silence all haptics.",
            ) {
                ControllerMappings.setRumbleEnabled(it)
                refreshToken.value++
            }
            SettingsDivider()
            // Buzz the selected player's controller and report whether Android can drive
            // its rumble — separates a routing problem from a pad whose haptics simply
            // aren't exposed to Android (common for DualSense/DS4 over Bluetooth).
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(30.dp)
                    .background(rowAura())
                    .clickable {
                        NativeApp.testRumble(editPlayer.value)
                        android.widget.Toast.makeText(
                            ctx, NativeApp.rumbleStatusForPort(editPlayer.value),
                            android.widget.Toast.LENGTH_LONG,
                        ).show()
                    }
                    .controllerFocusable(
                        controllerId = "pad-test-rumble",
                        onConfirm = {
                            NativeApp.testRumble(editPlayer.value)
                            android.widget.Toast.makeText(
                                ctx, NativeApp.rumbleStatusForPort(editPlayer.value),
                                android.widget.Toast.LENGTH_LONG,
                            ).show()
                        },
                    )
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                Text(
                    if (editPlayer.value == 0) "Test rumble — Player 1" else "Test rumble — Player 2",
                    color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                )
            }
            SettingsDivider()
        }
        CollapsibleSection("Analog Sticks", initiallyExpanded = false) {
            // Analog stick remapping — make a physical stick act as the D-pad or the
            // face buttons (great for fighting games on analog-centric controllers).
            run {
                val stickOpts = ControllerMappings.StickMode.values().map { it.label }
                SegmentedRow(
                    label = "Left Stick",
                    options = stickOpts,
                    selectedIndex = ControllerMappings.leftStickModeScope(editPlayer.value, editSerial).ordinal,
                    description = "What the left analog stick sends: Analog (default), Face, or Custom (bind each direction below).",
                    onChange = {
                        ControllerMappings.setLeftStickMode(ControllerMappings.StickMode.values()[it], editPlayer.value, editSerial)
                        refreshToken.value++
                    },
                )
                SettingsDivider()
                if (ControllerMappings.leftStickModeScope(editPlayer.value, editSerial) == ControllerMappings.StickMode.CUSTOM) {
                    ControllerMappings.StickDir.values().forEach { dir ->
                        StickDirPickerRow(leftStick = true, dir = dir, player = editPlayer.value, serial = editSerial, onChanged = { refreshToken.value++ })
                        SettingsDivider()
                    }
                }
                // Axis correction for the LEFT stick — fixes pads that read mirrored/rotated.
                ToggleRow("Left Stick — Swap X/Y", ControllerMappings.stickSwapXY(true),
                    description = "Swap the left stick's horizontal and vertical axes (for a stick that reads rotated 90°).") {
                    ControllerMappings.setStickSwapXY(true, it); refreshToken.value++
                }
                SettingsDivider()
                ToggleRow("Left Stick — Invert X", ControllerMappings.stickInvertX(true),
                    description = "Mirror the left stick horizontally — fixes \"left is right\".") {
                    ControllerMappings.setStickInvertX(true, it); refreshToken.value++
                }
                SettingsDivider()
                ToggleRow("Left Stick — Invert Y", ControllerMappings.stickInvertY(true),
                    description = "Mirror the left stick vertically — fixes \"down is up\".") {
                    ControllerMappings.setStickInvertY(true, it); refreshToken.value++
                }
                SettingsDivider()
                SegmentedRow(
                    label = "Right Stick",
                    options = stickOpts,
                    selectedIndex = ControllerMappings.rightStickModeScope(editPlayer.value, editSerial).ordinal,
                    description = "What the right analog stick sends: Analog (default), Face, or Custom (bind each direction below).",
                    onChange = {
                        ControllerMappings.setRightStickMode(ControllerMappings.StickMode.values()[it], editPlayer.value, editSerial)
                        refreshToken.value++
                    },
                )
                SettingsDivider()
                if (ControllerMappings.rightStickModeScope(editPlayer.value, editSerial) == ControllerMappings.StickMode.CUSTOM) {
                    ControllerMappings.StickDir.values().forEach { dir ->
                        StickDirPickerRow(leftStick = false, dir = dir, player = editPlayer.value, serial = editSerial, onChanged = { refreshToken.value++ })
                        SettingsDivider()
                    }
                }
                // Axis correction for the RIGHT stick — e.g. the tester's "down is up, left is right".
                ToggleRow("Right Stick — Swap X/Y", ControllerMappings.stickSwapXY(false),
                    description = "Swap the right stick's horizontal and vertical axes (for a stick that reads rotated 90°).") {
                    ControllerMappings.setStickSwapXY(false, it); refreshToken.value++
                }
                SettingsDivider()
                ToggleRow("Right Stick — Invert X", ControllerMappings.stickInvertX(false),
                    description = "Mirror the right stick horizontally — fixes \"left is right\".") {
                    ControllerMappings.setStickInvertX(false, it); refreshToken.value++
                }
                SettingsDivider()
                ToggleRow("Right Stick — Invert Y", ControllerMappings.stickInvertY(false),
                    description = "Mirror the right stick vertically — fixes \"down is up\".") {
                    ControllerMappings.setStickInvertY(false, it); refreshToken.value++
                }
                SettingsDivider()
                ToggleRow(
                    "D-pad acts as Left Stick",
                    ControllerMappings.dpadAsLeftStick(),
                    description = "Make the physical D-pad drive the left analog stick (full deflection) so it works in games that only read the analog stick. The D-pad stops sending digital presses while this is on.",
                ) {
                    ControllerMappings.setDpadAsLeftStick(it)
                    refreshToken.value++
                }
                SettingsDivider()
                IntSliderRow(
                    label = "Stick Deadzone",
                    value = (ControllerMappings.stickDeadzone() * 100f).toInt(), // 0.0..0.4 -> 0..40
                    min = 0,
                    max = (ControllerMappings.STICK_DZ_MAX * 100f).toInt(),
                    description = "Fraction of physical analog travel ignored near center. Lower = stick responds sooner (handheld sticks have little range); 0 = none. Movement re-normalizes past it, so no jump.",
                    valueFormatter = { if (it == 0) "Off" else "${it}%" },
                    onChange = { ControllerMappings.setStickDeadzone(it / 100f); refreshToken.value++ },
                )
                SettingsDivider()
                IntSliderRow(
                    label = "Stick Outer Deadzone",
                    value = (ControllerMappings.stickOuterDeadzone() * 100f).toInt(), // 0.0..0.4 -> 0..40
                    min = 0,
                    max = (ControllerMappings.STICK_OUTER_MAX * 100f).toInt(),
                    description = "Fraction of travel near the EDGE mapped to full output, so a stick that can't physically reach its corners still hits 100% (short-throw / handheld sticks like the Odin). 0 = off.",
                    valueFormatter = { if (it == 0) "Off" else "${it}%" },
                    onChange = { ControllerMappings.setStickOuterDeadzone(it / 100f); refreshToken.value++ },
                )
                SettingsDivider()
                IntSliderRow(
                    label = "Stick Anti-Deadzone",
                    value = (ControllerMappings.stickAntiDeadzone() * 100f).toInt(), // 0.0..0.6 -> 0..60
                    min = 0,
                    max = (ControllerMappings.STICK_ANTIDZ_MAX * 100f).toInt(),
                    description = "Smallest output sent to the game, to cancel a game's OWN built-in stick deadzone (e.g. Cold Fear / Area 51 ignore the stick until ~45%, then aim jumps). Set near the game's deadzone so any stick movement responds immediately and the full travel maps smoothly above it. 0 = off.",
                    valueFormatter = { if (it == 0) "Off" else "${it}%" },
                    onChange = { ControllerMappings.setStickAntiDeadzone(it / 100f); refreshToken.value++ },
                )
                SettingsDivider()
                IntSliderRow(
                    label = "Stick Sensitivity",
                    value = (ControllerMappings.stickSensitivity() * 20f).toInt(), // 0.5..2.0 -> 10..40
                    min = 10,
                    max = 40,
                    description = "Linear scale on the physical analog sticks (native Analog + Custom analog directions). Under 100% = finer/slower aim, over 100% = faster.",
                    valueFormatter = { "${it * 5}%" },
                    onChange = { ControllerMappings.setStickSensitivity(it / 20f); refreshToken.value++ },
                )
                SettingsDivider()
                IntSliderRow(
                    label = "Stick Acceleration",
                    value = (ControllerMappings.stickAcceleration() * 10f).toInt(), // 0.0..2.0 -> 0..20
                    min = 0,
                    max = 20,
                    description = "Non-linear response curve: small tilts stay precise for aiming, full tilt ramps up to full speed. 0 = linear (off); higher = more curve.",
                    valueFormatter = { if (it == 0) "Off (linear)" else "+%.1f".format(it / 10f) },
                    onChange = { ControllerMappings.setStickAcceleration(it / 10f); refreshToken.value++ },
                )
                SettingsDivider()
            }
        }
        CollapsibleSection("Button Mapping", initiallyExpanded = false) {
            ControllerMappings.actions.forEach { action ->
                val physical = ControllerMappings.physicalForScope(action, editPlayer.value, editSerial)
                PadBindingRow(
                    action = action,
                    physical = physical,
                    capturing = capture.value == action,
                    onClick = { capture.value = action },
                    onClear = {
                        ControllerMappings.clearAction(action, editPlayer.value, editSerial)
                        if (capture.value == action) capture.value = null
                        refreshToken.value++
                    },
                )
                SettingsDivider()
            }
            // Reset clears this scope's binds: Global wipes the global map; Game
            // removes this game's per-game overrides (button binds AND stick maps),
            // reverting it to the global map.
            val resetMappings: () -> Unit = {
                if (editSerial != null) ControllerMappings.clearGameOverrides(editSerial, editPlayer.value)
                else ControllerMappings.reset(editPlayer.value)
                capture.value = null
                refreshToken.value++
            }
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(30.dp)
                    .background(rowAura())
                    .clickable { resetMappings() }
                    .controllerFocusable(
                        controllerId = "pad-reset",
                        onConfirm = resetMappings,
                    )
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                val who = if (editPlayer.value == 0) "Player 1" else "Player 2"
                Text(
                    if (editSerial != null) "Reset $who Mappings for This Game"
                    else "Reset $who Mappings",
                    color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                )
            }
        }
        CollapsibleSection("On-Screen Controls", initiallyExpanded = false) {
            // Controller hotkeys now live in their own dedicated "Hotkeys" tab
            // (see HotkeysTab) so they're easier to find than buried under Pad.
            SettingsDivider()
            IntSliderRow(
                label = "On-screen controls",
                value = TouchControls.visibilityMode.value,
                min = 0,
                max = 11,
                description = "On-screen touch buttons. Never = always hidden (for physical-controls devices — also hides the settings cog so nothing overlaps R1). 1–10s = auto-hide after that long without a touch. Auto = show on touch, hide when you use a controller.",
                valueFormatter = { when (it) { 0 -> "Never"; 11 -> "Auto"; else -> "${it}s" } },
                onChange = { TouchControls.setVisibilityMode(it) },
            )
            SettingsDivider()
            // Touch Haptics (#247): vibrate on on-screen button presses.
            ToggleRow(
                "Touch Haptics",
                TouchControls.touchHaptics.value,
                description = "Vibrate briefly when you press an on-screen button (like PPSSPP / Azahar). Separate from controller rumble.",
            ) { TouchControls.setTouchHaptics(it) }
        }
    }
}

/** One CUSTOM-mode row: a stick direction on the left, its bound target on the
 *  right. Tap (or A) opens a PICKER to choose what the direction sends — a PS2
 *  button (incl. the D-pad), an ARMSX2 hotkey, or Analog (default). A direct
 *  picker (not "press a physical button") means you can assign a target even
 *  after you've UNBOUND that button elsewhere — the old capture resolved the
 *  pressed button through its live mapping, so an unbound D-pad couldn't be
 *  picked. "Clear" (or D-pad-left on a controller) resets to the analog default.
 *  Shown only when the stick is CUSTOM. */
@Composable
private fun StickDirPickerRow(
    leftStick: Boolean,
    dir: ControllerMappings.StickDir,
    player: Int,
    serial: String?,
    onChanged: () -> Unit,
) {
    @Suppress("UNUSED_EXPRESSION")
    ControllerMappings.stickBindTick.value // recompose after a bind/reset
    val code = ControllerMappings.customStickCodeScope(leftStick, dir, player, serial)
    val showPicker = remember { mutableStateOf(false) }
    fun clear() {
        ControllerMappings.resetStickCode(leftStick, dir, player, serial)
        onChanged()
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(30.dp)
            .background(rowAura())
            .clickable { showPicker.value = true }
            .controllerFocusable(
                controllerId = "stickdir:${if (leftStick) "l" else "r"}:${dir.id}",
                onConfirm = { showPicker.value = true },
                onLeft = { clear() },
            )
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            "    ${dir.id.replaceFirstChar { it.uppercase() }}",
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.weight(1f))
        Text(
            "Clear",
            color = Color(0xFFE57373),
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .clickable { clear() }
                .padding(horizontal = 8.dp, vertical = 2.dp),
        )
        Spacer(Modifier.width(4.dp))
        Text(
            ControllerMappings.stickTargetLabel(code),
            color = Color(0xFFCCCCCC),
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
        )
    }
    if (showPicker.value) {
        StickTargetPickerDialog(
            title = "${if (leftStick) "Left" else "Right"} Stick — ${dir.id.replaceFirstChar { it.uppercase() }}",
            current = code,
            onPick = { picked ->
                if (picked == null) ControllerMappings.resetStickCode(leftStick, dir, player, serial)
                else ControllerMappings.setCustomStickCode(leftStick, dir, picked, player, serial)
                showPicker.value = false
                onChanged()
            },
            onDismiss = { showPicker.value = false },
        )
    }
}

/** Direct target picker for a CUSTOM stick direction: PS2 buttons, ARMSX2
 *  hotkeys, or Analog (default). [onPick] receives the chosen setPadButton code,
 *  or null for Analog (default → clears the override). */
@Composable
private fun StickTargetPickerDialog(
    title: String,
    current: Int,
    onPick: (Int?) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Colors.surfaceColor,
        titleContentColor = Color.White,
        textContentColor = Color.White,
        title = { Text(title, color = Color.White, fontWeight = FontWeight.Bold) },
        text = {
            Column(Modifier.verticalScroll(remember { ScrollState(0) })) {
                Text(
                    "Choose what this stick direction sends. Works regardless of which physical buttons are bound.",
                    color = Color(0xFFBBBBBB), fontSize = 12.sp,
                )
                Spacer(Modifier.height(8.dp))
                StickPickItem("Analog (default)", current in 110..123) { onPick(null) }
                Spacer(Modifier.height(6.dp))
                Text("PS2 Buttons", color = Colors.pasx2_blue, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                ControllerMappings.stickTargets.forEach { t ->
                    StickPickItem(t.label, current == t.code) { onPick(t.code) }
                }
                Spacer(Modifier.height(6.dp))
                Text("Hotkeys", color = Colors.pasx2_blue, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                ControllerMappings.SysHotkey.values().forEach { h ->
                    val hc = ControllerMappings.stickCodeForHotkey(h)
                    StickPickItem("Hotkey: ${h.label}", current == hc) { onPick(hc) }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel", color = Colors.pasx2_blue) }
        },
    )
}

@Composable
private fun StickPickItem(label: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .padding(vertical = 8.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (selected) "●  " else "○  ",
            color = if (selected) Colors.pasx2_blue else Color(0xFF777777),
            fontSize = 13.sp,
        )
        Text(
            label,
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
        )
    }
}

/** Dialog to choose which pad buttons a macro fires together. */
@Composable
private fun MacroConfigDialog(
    macroId: TouchButtonId,
    onSaved: () -> Unit,
    onDismiss: () -> Unit,
) {
    val selected = remember(macroId) {
        mutableStateListOf<TouchButtonId>().apply { addAll(TouchControls.macroButtons(macroId)) }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Colors.surfaceColor,
        titleContentColor = Color.White,
        textContentColor = Color.White,
        title = { Text("Configure ${macroId.label}", color = Color.White, fontWeight = FontWeight.Bold) },
        text = {
            Column(Modifier.verticalScroll(remember { ScrollState(0) })) {
                Text(
                    "Tap the buttons this macro should press together.",
                    color = Color(0xFFBBBBBB), fontSize = 12.sp,
                )
                Spacer(Modifier.height(8.dp))
                TouchControls.macroAssignableButtons.forEach { b ->
                    val on = b in selected
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .height(36.dp)
                            .clickable { if (on) selected.remove(b) else selected.add(b) }
                            .padding(horizontal = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            if (on) "☑" else "☐",
                            color = if (on) Colors.pasx2_blue else Color(0xFF888888),
                            fontSize = 16.sp,
                        )
                        Spacer(Modifier.width(12.dp))
                        Text(b.label, color = Color.White, fontSize = 14.sp)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                TouchControls.setMacroButtons(macroId, selected.toList())
                onSaved()
                onDismiss()
            }) { Text("SAVE") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("CANCEL") } },
    )
}

@Composable
private fun PadBindingRow(
    action: ControllerMappings.Action,
    physical: Int,
    capturing: Boolean,
    onClick: () -> Unit,
    onClear: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(30.dp)
            .background(rowAura())
            .clickable(onClick = onClick)
            .controllerFocusable(
                controllerId = "pad:${action.id}",
                onConfirm = onClick,
            )
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(action.label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.weight(1f))
        // "Clear" unbinds the button (leaves it blank, free to assign as a hotkey) —
        // mirrors the Hotkeys tab. Shown only when bound and not mid-capture.
        if (!capturing && physical != android.view.KeyEvent.KEYCODE_UNKNOWN) {
            Text(
                "Clear",
                color = Color(0xFFFF6B6B),
                fontSize = 11.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier
                    .clickable(onClick = onClear)
                    .padding(end = 10.dp),
            )
        }
        Text(
            if (capturing) "Press a button..." else ControllerMappings.labelForKey(physical),
            color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
        )
    }
}
