package com.armsx2.ui.settings

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Surface
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.Colors
import com.armsx2.ui.touch.TouchButtonId
import com.armsx2.ui.touch.TouchControls
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

@Composable
fun PadTab(@Suppress("UNUSED_PARAMETER") state: MutableState<Settings>) {
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)
    val capture = remember { mutableStateOf<ControllerMappings.Action?>(null) }
    // Local co-op: which player's mapping this tab is editing (0 = P1, 1 = P2).
    val editPlayer = remember { mutableIntStateOf(0) }
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
    val refreshToken = remember { mutableIntStateOf(0) }
    val focusRequester = remember { FocusRequester() }
    // Which macro is capturing a physical-controller trigger button (null = none).
    val macroCapture = remember { mutableStateOf<TouchButtonId?>(null) }

    val stickCapture = ControllerMappings.captureStickDir
    LaunchedEffect(capture.value, stickCapture.value, macroCapture.value) {
        // Tell MainActivityRuntime.dispatchKeyEvent to stop intercepting controller buttons for
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
                    refreshToken.intValue++
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
                        else ControllerMappings.stickCodeForPhysical(ncode, editPlayer.intValue)
                    if (target != null) {
                        ControllerMappings.setCustomStickCode(sc.first, sc.second, target, editPlayer.intValue, liveEditSerial())
                        ControllerMappings.endStickCapture()
                        refreshToken.intValue++
                    }
                    // If the pressed button isn't mapped to any pad Action or hotkey,
                    // keep waiting (swallow) rather than binding nothing.
                    return@onPreviewKeyEvent true
                }
                // Regular button capture — the menu button is captured in
                // MainActivityRuntime.dispatchKeyEvent so it can grab BACK / back-paddle keys.
                val action = capture.value ?: return@onPreviewKeyEvent false
                if (event.type != KeyEventType.KeyDown)
                    return@onPreviewKeyEvent true
                val nativeKeyCode = event.key.nativeKeyCode
                if (nativeKeyCode == android.view.KeyEvent.KEYCODE_UNKNOWN)
                    return@onPreviewKeyEvent true
                ControllerMappings.bind(action, nativeKeyCode, editPlayer.intValue, liveEditSerial())
                capture.value = null
                refreshToken.intValue++
                true
            },
    ) {
        Text(
            str("pad.instruction.tapThenPress"),
            color = Color(0xFFBBBBBB),
            fontSize = 15.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 6.dp),
        )
        // Per-game scope hint (#246): the buttons / sticks below follow the
        // Global/Game toggle at the top of the menu. Spell out the current tier
        // here since this tab scrolls far from that toggle.
        Text(
            when {
                editSerial != null -> "● Editing controls for THIS GAME ($editSerial) — switch to Global up top to change all games."
                padSerial != null -> str("pad.scopeHint.globalWithGameHint")
                else -> str("pad.scopeHint.global")
            },
            color = if (editSerial != null) Colors.pasx2_blue else Color(0xFF9A9A9A),
            fontSize = 14.sp,
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
                .height(56.dp)
                .clip(RoundedCornerShape(16.dp))
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
                str("pad.editTouchLayout"),
                color = Colors.pasx2_blue, fontSize = 16.sp, fontWeight = FontWeight.Bold,
            )
        }
        SettingsDivider()
        // Loose state reads kept at the top of the grouped area so they always
        // recompose the tab regardless of which sections are collapsed.
        @Suppress("UNUSED_EXPRESSION") TouchControls.macroBindTick.value
        @Suppress("UNUSED_EXPRESSION")
        refreshToken.intValue
        // Also recompose when the mappings change externally (the Controls tab's Reset calls
        // ControllerMappings.resetAllControls, which bumps this) so the feel sliders / stick
        // modes refresh without re-opening the tab.
        @Suppress("UNUSED_EXPRESSION")
        ControllerMappings.stickBindTick.value
        // Macros section — extracted so the in-game Controls tab can reuse it. Here in the
        // full Pad tab we pass the physical-trigger capture host, so the "Bind" column is live.
        MacrosSection(
            macroCapture = macroCapture,
            onArmCapture = { mid ->
                macroCapture.value = if (macroCapture.value == mid) null else mid
                capture.value = null
                ControllerMappings.captureStickDir.value = null
            },
        )
        CollapsibleSection(str("pad.section.playerRumble"), initiallyExpanded = false) {
            // Local co-op: pick which player's buttons / stick mode you're editing. P2 is
            // the second controller to press a button in-game (auto-assigned). Stick
            // feel (deadzone / sensitivity / acceleration) and the D-pad-as-stick toggle
            // below are shared by both players.
            SegmentedRow(
                label = str("pad.editing.label"),
                options = listOf(str("pad.player1"), str("pad.player2")),
                selectedIndex = editPlayer.intValue,
                description = str("pad.editing.description"),
                onChange = {
                    editPlayer.intValue = it
                    capture.value = null
                    ControllerMappings.captureStickDir.value = null
                    refreshToken.intValue++
                },
            )
            // Master rumble / vibration enable — gates controller motors AND the device-haptic
            // fallback (NativeApp.onPadRumble). Off = no haptics anywhere.
            ToggleRow(
                str("pad.rumble.label"),
                ControllerMappings.rumbleEnabled(),
                description = str("pad.rumble.description"),
            ) {
                ControllerMappings.setRumbleEnabled(it)
                refreshToken.intValue++
            }
            // Vibration strength: one multiplier over BOTH controller rumble and on-screen touch
            // haptics (they share the motor path), so an over-eager motor can be tamed or a weak
            // one boosted. 100% = as authored; 0% = off.
            IntSliderRow(
                label = str("pad.hapticStrength.label"),
                value = ControllerMappings.hapticIntensity(),
                min = 0,
                max = 200,
                description = str("pad.hapticStrength.description"),
                valueFormatter = { if (it == 0) "Off" else "${it}%" },
                onChange = { ControllerMappings.setHapticIntensity(it); refreshToken.intValue++ },
            )
            SettingsDivider()
            // How hard the DS2 pressure modifier presses. There was a PRESSURE button (on-screen
            // and bindable as "Pressure Modifier (hold)") but no way to choose the amount, so it
            // was permanently stuck at the hardcoded 50%. Range is deliberately 5..95: 0 collides
            // with the "full press" sentinel and 100 is just a normal press.
            IntSliderRow(
                label = str("pad.pressureAmount.label"),
                value = com.armsx2.ui.touch.TouchControls.pressurePercent.intValue,
                min = 5,
                max = 95,
                description = str("pad.pressureAmount.description"),
                valueFormatter = { "${it}%" },
                onChange = { com.armsx2.ui.touch.TouchControls.setPressurePercent(it) },
            )
            SettingsDivider()
            // PS2 Multitap: route up to 8 controllers (both ports become 4-slot taps).
            // The pref drives PadRouter's slot count + the boot-time native arming; when a
            // game is already running we also arm it live. setMultitap parks the VM, so it
            // must run off the UI thread (and is a safe no-op when no VM is active).
            ToggleRow(
                str("pad.multitap.label"),
                ControllerMappings.multitapEnabled(),
                description = str("pad.multitap.description"),
            ) { on ->
                ControllerMappings.setMultitapEnabled(on)
                refreshToken.intValue++
            }
            SettingsDivider()
            // Which physical controller is which player, and where its rumble goes.
            //
            // Slots are otherwise claimed first-to-press, which cannot express "the DualSense is
            // player 1 and the built-in pad is player 2" -- and on a handheld the built-in pad is
            // usually whatever presses something first. Pins are stored per controller (by
            // descriptor, so they survive reconnects) and set once, rather than raced for at the
            // start of every session.
            val pads = remember(refreshToken.intValue) { com.armsx2.input.PadRouter.connectedPads() }
            if (pads.isNotEmpty()) {
                HelpText(str("pad.assign.help"))
                // Only the slots Multitap actually arms. Offering player 3-8 with Multitap off
                // would let the user pin a pad at an un-armed PS2 port, where its input goes
                // nowhere at all -- the router ignores such a pin, so the picker must not show it.
                val slotCount =
                    if (ControllerMappings.multitapEnabled()) com.armsx2.input.PadRouter.MAX_PADS else 2
                val slotLabels = listOf(str("pad.assign.auto")) +
                    (0 until slotCount).map { str("pad.player${it + 1}") }
                val rumbleModes = com.armsx2.input.PadRouter.RumbleMode.entries
                val rumbleLabels = listOf(
                    str("pad.assign.auto"),
                    str("pad.assign.rumble.controller"),
                    str("pad.assign.rumble.device"),
                    str("pad.assign.rumble.off"),
                )
                pads.forEach { pad ->
                    val pinnedPort = com.armsx2.input.PadRouter.pins()[pad.descriptor]
                    SegmentedRow(
                        label = pad.name,
                        options = slotLabels,
                        selectedIndex = (pinnedPort?.plus(1) ?: 0).coerceIn(0, slotLabels.lastIndex),
                        onChange = { index ->
                            com.armsx2.input.PadRouter.setPin(
                                pad.descriptor,
                                if (index == 0) null else index - 1,
                            )
                            refreshToken.intValue++
                        },
                    )
                    // Where THIS pad's rumble goes. A controller can report motors it never
                    // drives -- a handheld bridging an external pad through its own HID node
                    // does exactly that -- and no API call can tell that apart from a working
                    // motor, so the fallback has to be selectable rather than detected.
                    SegmentedRow(
                        label = pad.name + " — " + str("pad.assign.rumble"),
                        options = rumbleLabels,
                        selectedIndex = rumbleModes.indexOf(
                            com.armsx2.input.PadRouter.rumbleMode(pad.descriptor),
                        ).coerceAtLeast(0),
                        onChange = { index ->
                            com.armsx2.input.PadRouter.setRumbleMode(pad.descriptor, rumbleModes[index])
                            refreshToken.intValue++
                        },
                    )
                }
                SettingsDivider()
                // Taking the pad over on USB is the only way to reach a PlayStation controller's
                // motors when the platform's own vibrator for it does nothing. It claims the
                // pad's single HID interface, so input has to come through us too -- which is
                // why it is a switch and not something done quietly on the user's behalf.
                ToggleRow(
                    str("pad.usbTakeover.label"),
                    com.armsx2.input.UsbRumble.takeover,
                    description = str("pad.usbTakeover.description"),
                ) {
                    com.armsx2.input.UsbRumble.setTakeover(it)
                    refreshToken.intValue++
                }
                SettingsDivider()
            }
            // Escape hatch for pads Android will not drive. #433 stopped the phone buzzing for
            // an external pad; #646 (same reporter) is the other half of that trade -- their
            // Xbox pad exposes no motor, so suppressing the fallback left them with nothing.
            // A handheld's own built-in pad is not external and never took this path.
            ToggleRow(
                str("pad.rumbleFallback.label"),
                ControllerMappings.rumbleFallbackExternal(),
                description = str("pad.rumbleFallback.description"),
            ) { on ->
                ControllerMappings.setRumbleFallbackExternal(on)
                refreshToken.intValue++
            }
            SettingsDivider()
            // Buzz the selected player's controller and report whether Android can drive
            // its rumble — separates a routing problem from a pad whose haptics simply
            // aren't exposed to Android (common for DualSense/DS4 over Bluetooth).
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(56.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(rowAura())
                    .clickable {
                        NativeApp.testRumble(editPlayer.intValue)
                        android.widget.Toast.makeText(
                            ctx, NativeApp.rumbleStatusForPort(editPlayer.intValue),
                            android.widget.Toast.LENGTH_LONG,
                        ).show()
                    }
                    .controllerFocusable(
                        controllerId = "pad-test-rumble",
                        onConfirm = {
                            NativeApp.testRumble(editPlayer.intValue)
                            android.widget.Toast.makeText(
                                ctx, NativeApp.rumbleStatusForPort(editPlayer.intValue),
                                android.widget.Toast.LENGTH_LONG,
                            ).show()
                        },
                    )
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                Text(
                    if (editPlayer.intValue == 0) str("pad.testRumble.player1") else str("pad.testRumble.player2"),
                    color = Colors.pasx2_blue, fontSize = 16.sp, fontWeight = FontWeight.Bold,
                )
            }
            SettingsDivider()
        }
        AnalogSticksSection(editPlayer, refreshToken, editSerial)
        // Motion / gyroscope controls. Shared with the in-game pause menu's Controls tab
        // (com.armsx2.ui.settings.GyroSection). Here it follows the Pad tab's Global/Game
        // scope (editSerial) and shares the tab's refreshToken so it re-reads live.
        GyroSection(editSerial = editSerial, externalRefresh = refreshToken)
        CollapsibleSection(str("pad.section.buttonMapping"), initiallyExpanded = false) {
            ControllerMappings.actions.forEach { action ->
                val physical = ControllerMappings.physicalForScope(action, editPlayer.intValue, editSerial)
                PadBindingRow(
                    action = action,
                    physical = physical,
                    capturing = capture.value == action,
                    onClick = { capture.value = action },
                    onClear = {
                        ControllerMappings.clearAction(action, editPlayer.intValue, editSerial)
                        if (capture.value == action) capture.value = null
                        refreshToken.intValue++
                    },
                )
                // Turbo / rapid-fire toggle — only meaningful once the button is
                // bound to a physical controller button.
                if (physical != android.view.KeyEvent.KEYCODE_UNKNOWN) {
                    val turbo = remember(action.id, editPlayer.intValue, refreshToken.intValue) {
                        mutableStateOf(ControllerMappings.isTurboAction(action, editPlayer.intValue))
                    }
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable {
                                val nv = !turbo.value
                                turbo.value = nv
                                ControllerMappings.setTurboAction(action, editPlayer.intValue, nv)
                            }
                            .padding(start = 18.dp, end = 10.dp, top = 2.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            "↳ Turbo (rapid-fire while held)",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 14.sp,
                            modifier = Modifier.weight(1f),
                        )
                        Text(
                            if (turbo.value) "ON" else "OFF",
                            color = if (turbo.value) Color(0xFF4DA3FF)
                            else Color(0xFF808080),
                            fontSize = 15.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                    // Tap to hold (#612) — the on-screen buttons have always had this; an
                    // accessibility request brought it to physical ones, for games that expect a
                    // button held while another control is worked. Sits with Turbo because both
                    // change what holding the button means, and both need a binding to act on.
                    val latch = remember(action.id, editPlayer.intValue, refreshToken.intValue) {
                        mutableStateOf(ControllerMappings.isLatchAction(action, editPlayer.intValue))
                    }
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable {
                                val nv = !latch.value
                                latch.value = nv
                                ControllerMappings.setLatchAction(action, editPlayer.intValue, nv)
                            }
                            .padding(start = 18.dp, end = 10.dp, top = 2.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            "\u21b3 Tap to hold (press once to hold, again to release)",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 14.sp,
                            modifier = Modifier.weight(1f),
                        )
                        Text(
                            if (latch.value) "ON" else "OFF",
                            color = if (latch.value) Color(0xFF4DA3FF)
                            else Color(0xFF808080),
                            fontSize = 15.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
                SettingsDivider()
            }
            // Reset clears this scope's binds: Global wipes the global map; Game
            // removes this game's per-game overrides (button binds AND stick maps),
            // reverting it to the global map.
            val resetMappings: () -> Unit = {
                if (editSerial != null) ControllerMappings.clearGameOverrides(editSerial, editPlayer.intValue)
                else ControllerMappings.reset(editPlayer.intValue)
                capture.value = null
                refreshToken.intValue++
            }
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(56.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(rowAura())
                    .clickable { resetMappings() }
                    .controllerFocusable(
                        controllerId = "pad-reset",
                        onConfirm = resetMappings,
                    )
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                val who = str(if (editPlayer.intValue == 0) "pad.player1" else "pad.player2")
                Text(
                    "${str("action.reset")} · $who${if (editSerial != null) " · ${str("scope.game")}" else ""}",
                    color = Colors.pasx2_blue, fontSize = 16.sp, fontWeight = FontWeight.Bold,
                )
            }
        }
        // Named mapping profiles (#186). Sits right under the mapping rows because it
        // acts on exactly what they show: the CURRENT player and the CURRENT scope.
        // Save snapshots that map; applying a profile overwrites it. Feel settings
        // (deadzone/sensitivity/rumble) are deliberately not part of a profile — see
        // ControllerMappings' profile section for why.
        CollapsibleSection(str("pad.section.padProfiles"), initiallyExpanded = false) {
            SettingsDivider()
            HelpText(str("pad.padProfiles.info"))
            val newName = remember { mutableStateOf("") }
            val profiles = remember { mutableStateOf<List<String>>(emptyList()) }
            // listProfiles() reads the portable inputprofiles/ folder, so it rides an IO
            // hop rather than landing in composition. Re-runs when a profile is
            // added/removed (padProfileTick).
            LaunchedEffect(ControllerMappings.padProfileTick.value) {
                profiles.value = withContext(Dispatchers.IO) { ControllerMappings.listProfiles() }
            }
            if (profiles.value.isEmpty()) {
                HelpText(str("pad.padProfiles.none"))
            }
            profiles.value.forEach { name ->
                val apply: () -> Unit = {
                    ControllerMappings.applyProfile(name, editPlayer.intValue, liveEditSerial())
                    capture.value = null
                    // The binding rows above read straight from prefs, so they only show
                    // the applied map once this bumps.
                    refreshToken.intValue++
                }
                Row(
                    Modifier
                        .fillMaxWidth()
                        .height(52.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(rowAura())
                        .clickable { apply() }
                        .controllerFocusable(
                            controllerId = "pad-profile:$name",
                            onConfirm = apply,
                        )
                        .padding(horizontal = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        name,
                        color = Color.White,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        str("action.delete"),
                        color = Color(0xFFFF6B6B),
                        fontSize = 12.sp,
                        modifier = Modifier
                            .clickable { ControllerMappings.deleteProfile(name) }
                            .padding(horizontal = 8.dp, vertical = 6.dp),
                    )
                }
                Spacer(Modifier.height(6.dp))
            }
            Spacer(Modifier.height(4.dp))
            Text(str("pad.padProfiles.saveNewLabel"), color = Color(0xFFAAAAAA), fontSize = 12.sp)
            Spacer(Modifier.height(6.dp))
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                // Inline field, never a dialog: a Dialog is its own focused window and
                // swallows the pad keys this whole tab exists to configure.
                OutlinedTextField(
                    value = newName.value,
                    onValueChange = { newName.value = it },
                    singleLine = true,
                    placeholder = { Text(str("pad.padProfiles.namePlaceholder"), color = Color(0xFF888888)) },
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedTextColor = Color.White,
                        unfocusedTextColor = Color.White,
                        focusedBorderColor = Colors.pasx2_blue,
                        unfocusedBorderColor = Color(0xFF444455),
                    ),
                    modifier = Modifier.weight(1f),
                )
                val save: () -> Unit = {
                    if (ControllerMappings.saveProfile(newName.value, editPlayer.intValue, liveEditSerial()))
                        newName.value = ""
                }
                Box(
                    Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(Colors.pasx2_blue)
                        .clickable(enabled = newName.value.isNotBlank()) { save() }
                        .controllerFocusable(
                            controllerId = "pad-profile-save",
                            onConfirm = save,
                        )
                        .padding(horizontal = 14.dp, vertical = 12.dp),
                ) {
                    Text(
                        str("pad.padProfiles.saveAs"),
                        color = Color.White,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
        }
        CollapsibleSection(str("pad.section.onScreenControls"), initiallyExpanded = false) {
            // Controller hotkeys now live in their own dedicated "Hotkeys" tab
            // (see HotkeysTab) so they're easier to find than buried under Pad.
            SettingsDivider()
            val visibilityOff = str("setup.toggle.off")
            val visibilityAuto = str("backend.renderer.auto")
            IntSliderRow(
                label = str("pad.onScreenControls.label"),
                value = TouchControls.visibilityMode.value,
                min = 0,
                max = 11,
                description = str("pad.onScreenControls.description"),
                valueFormatter = { when (it) { 0 -> visibilityOff; 11 -> visibilityAuto; else -> "${it}s" } },
                onChange = { TouchControls.setVisibilityMode(it) },
            )
            SettingsDivider()
            // Touch Haptics (#247): vibrate on on-screen button presses.
            ToggleRow(
                str("pad.touchHaptics.label"),
                TouchControls.touchHaptics.value,
                description = str("pad.touchHaptics.description"),
            ) { TouchControls.setTouchHaptics(it) }
            SettingsDivider()
            // Multi-touch reach: how far from a button's center a touch still counts.
            // Higher = press adjacent buttons together with more space between them.
            IntSliderRow(
                label = str("touch.editor.multiTouchOn"),
                value = (TouchControls.multiTouchRadius.value * 100f).toInt(),
                min = 50,
                max = 95,
                description = str("pad.multiTouch.description"),
                valueFormatter = { "${it}%" },
                onChange = { TouchControls.setMultiTouchRadius(it / 100f) },
            )
            // D-Pad key spacing lives in the Touch Layout editor now: open the editor,
            // tap the D-Pad to select it, and use the "D-Pad spacing" slider to spread
            // the four directions apart (NetherSX2-style) with a live preview.
            SettingsDivider()
            GestureControlSection(refreshToken)
            SettingsDivider()
            UsbDeviceSection(refreshToken)
        }
    }
}

/**
 * Emulated USB devices, one per port.
 *
 * The list comes from the core's own registry (18 devices — Buzz, Rock Band kit, Keyboardmania,
 * DJ turntable, Printer, EyeToy, GunCon 2, ...) so it cannot drift from what this build supports.
 * Buttons need no extra mapping: native mirrors each pad press onto the attached device via the
 * generic binding it declares. Restart-required, and said so plainly.
 */
@Composable
private fun UsbDeviceSection(refreshToken: MutableState<Int>) {
    @Suppress("UNUSED_EXPRESSION")
    refreshToken.value
    val devices = remember { com.armsx2.input.UsbDevices.available() }
    CollapsibleSection(str("pad.usb.section"), initiallyExpanded = false) {
        HelpText(str("pad.usb.help"))
        for (port in 0..1) {
            SettingsDivider()
            val current = com.armsx2.input.UsbDevices.portType[port].value
            // A plain list of rows rather than a segmented strip: 19 entries would be unusable as
            // chips, and this mirrors the radio list other emulators use for the same choice.
            Text(
                "${str("pad.usb.port")} ${port + 1}  ·  ${com.armsx2.input.UsbDevices.displayName(current)}",
                style = MaterialTheme.typography.labelMedium,
                color = Colors.pasx2_blue,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
            )
            UsbDeviceRow(str("pad.usb.none"), current == com.armsx2.input.UsbDevices.NONE) {
                com.armsx2.input.UsbDevices.setType(port, com.armsx2.input.UsbDevices.NONE)
                refreshToken.value++
            }
            devices.forEach { d ->
                UsbDeviceRow(d.display, current == d.type) {
                    com.armsx2.input.UsbDevices.setType(port, d.type)
                    refreshToken.value++
                }
            }
            // Subtypes only exist for a few devices (different wheels, different turntables).
            val subs = devices.firstOrNull { it.type == current }?.subtypes.orEmpty()
            if (subs.size > 1) {
                SegmentedRow(
                    label = str("pad.usb.subtype"),
                    options = subs,
                    selectedIndex = com.armsx2.input.UsbDevices.portSubtype[port].value.coerceIn(0, subs.lastIndex),
                    onChange = { com.armsx2.input.UsbDevices.setSubtype(port, it); refreshToken.value++ },
                )
            }
            // Aiming is the one thing a button bridge cannot provide, so the gun gets its own note.
            if (current == "guncon2") HelpText(str("pad.lightgun.help"))
        }
    }
}

/** One device choice. Radio-style: exactly one device per port. */
@Composable
private fun UsbDeviceRow(label: String, selected: Boolean, onPick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onPick)
            .controllerFocusable("usb.dev.$label", onConfirm = onPick)
            .padding(vertical = 7.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (selected) "\u25c9" else "\u25cb",
            color = if (selected) Colors.pasx2_blue else MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 15.sp,
        )
        Spacer(Modifier.width(10.dp))
        Text(
            label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}


/**
 * Gesture control (PPSSPP-style): swipes and a double-tap on EMPTY screen area fire PS2 buttons.
 *
 * Only fires where no control is — a finger that lands on a button, stick or d-pad belongs to that
 * widget (see GestureLayer). Off by default, so nothing changes unless someone opts in.
 */
@Composable
private fun GestureControlSection(refreshToken: MutableState<Int>) {
    @Suppress("UNUSED_EXPRESSION")
    refreshToken.value
    CollapsibleSection(str("pad.gesture.section"), initiallyExpanded = false) {
        ToggleRow(
            str("pad.gesture.enable.label"),
            TouchControls.gestureEnabled.value,
            description = str("pad.gesture.enable.description"),
        ) { TouchControls.setGestureEnabled(it); refreshToken.value++ }

        if (!TouchControls.gestureEnabled.value) return@CollapsibleSection

        SettingsDivider()
        // Each direction picks a PS2 button (or None). Same code space as the on-screen buttons.
        GestureButtonRow("pad.gesture.up", TouchControls.gestureSwipeUp.intValue) {
            TouchControls.setGestureSwipe(0, it); refreshToken.value++
        }
        GestureButtonRow("pad.gesture.down", TouchControls.gestureSwipeDown.intValue) {
            TouchControls.setGestureSwipe(1, it); refreshToken.value++
        }
        GestureButtonRow("pad.gesture.left", TouchControls.gestureSwipeLeft.intValue) {
            TouchControls.setGestureSwipe(2, it); refreshToken.value++
        }
        GestureButtonRow("pad.gesture.right", TouchControls.gestureSwipeRight.intValue) {
            TouchControls.setGestureSwipe(3, it); refreshToken.value++
        }
        SettingsDivider()
        IntSliderRow(
            label = str("pad.gesture.sensitivity.label"),
            // Percent of the shorter screen edge the finger must travel.
            value = (TouchControls.gestureSwipeSensitivity.floatValue * 100f).toInt(),
            min = 5,
            max = 60,
            description = str("pad.gesture.sensitivity.description"),
            valueFormatter = { "${it}%" },
            onChange = { TouchControls.setGestureSensitivity(it / 100f); refreshToken.value++ },
        )
        SettingsDivider()
        GestureButtonRow("pad.gesture.doubleTap", TouchControls.gestureDoubleTap.intValue) {
            TouchControls.setGestureDoubleTap(it); refreshToken.value++
        }
        // The tap-vs-hold choice SNAKEATER specifically asked for: Tap suits a one-shot (NFS
        // nitro), Hold suits something you want to stay on (ARPG camera lock).
        SegmentedRow(
            label = str("pad.gesture.doubleTapMode.label"),
            options = listOf(str("pad.gesture.doubleTapMode.tap"), str("pad.gesture.doubleTapMode.hold")),
            selectedIndex = if (TouchControls.gestureDoubleTapHold.value) 1 else 0,
            description = str("pad.gesture.doubleTapMode.description"),
            onChange = { TouchControls.setGestureDoubleTapHold(it == 1); refreshToken.value++ },
        )
    }
}

/** PS2-button chooser for one gesture, including a None entry. */
@Composable
private fun GestureButtonRow(labelKey: String, current: Int, onPick: (Int) -> Unit) {
    val choices = TouchControls.gestureAssignableButtons()
    val labels = listOf(str("pad.gesture.none")) + choices.map { it.second }
    val codes = listOf(0) + choices.map { it.first }
    SegmentedRow(
        label = str(labelKey),
        options = labels,
        selectedIndex = codes.indexOf(current).coerceAtLeast(0),
        onChange = { onPick(codes.getOrElse(it) { 0 }) },
    )
}

/** The five stick-FEEL sliders (deadzone / outer / anti-deadzone / sensitivity /
 *  acceleration) for ONE stick. Rendered twice — Left and Right — since every
 *  feel tunable is per-stick (a camera-stick sensitivity tweak must not slow the
 *  walk stick). Values migrate from the old shared keys on first read. */
@Composable
private fun StickFeelSliders(left: Boolean, title: String, refreshToken: MutableState<Int>) {
    // Subscribe this composable to the token. Each slider's `value` is read from a
    // raw pref (ControllerMappings.stick*), which Compose can't observe — so without
    // this read a bump (fired in every onChange) wouldn't recompose StickFeelSliders
    // (its params are unchanged, so Compose skips it) and the thumb/number would only
    // catch up on menu re-entry. Reading .value here puts the whole function in the
    // token's restart scope, so each drag step refreshes the displayed value live.
    @Suppress("UNUSED_EXPRESSION")
    refreshToken.value
    CollapsibleSection(title, initiallyExpanded = false) {
        SegmentedRow(
            label = str("pad.stickFeel.responseCurve.label"),
            options = listOf(
                str("pad.stickFeel.curve.linear"), str("pad.stickFeel.curve.light"),
                str("pad.stickFeel.curve.medium"), str("pad.stickFeel.curve.strong"),
            ),
            selectedIndex = ControllerMappings.stickResponseCurve(left),
            description = str("pad.stickFeel.responseCurve.description"),
            onChange = { ControllerMappings.setStickResponseCurve(left, it); refreshToken.value++ },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.stickFeel.deadzone.label"),
            value = (ControllerMappings.stickDeadzone(left) * 100f).toInt(), // 0.0..0.4 -> 0..40
            min = 0,
            max = (ControllerMappings.STICK_DZ_MAX * 100f).toInt(),
            description = str("pad.stickFeel.deadzone.description"),
            valueFormatter = { if (it == 0) "Off" else "${it}%" },
            onChange = { ControllerMappings.setStickDeadzone(left, it / 100f); refreshToken.value++ },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.stickFeel.outerDeadzone.label"),
            value = (ControllerMappings.stickOuterDeadzone(left) * 100f).toInt(), // 0.0..0.4 -> 0..40
            min = 0,
            max = (ControllerMappings.STICK_OUTER_MAX * 100f).toInt(),
            description = str("pad.stickFeel.outerDeadzone.description"),
            valueFormatter = { if (it == 0) "Off" else "${it}%" },
            onChange = { ControllerMappings.setStickOuterDeadzone(left, it / 100f); refreshToken.value++ },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.stickFeel.antiDeadzone.label"),
            value = (ControllerMappings.stickAntiDeadzone(left) * 100f).toInt(), // 0.0..0.6 -> 0..60
            min = 0,
            max = (ControllerMappings.STICK_ANTIDZ_MAX * 100f).toInt(),
            description = str("pad.stickFeel.antiDeadzone.description"),
            valueFormatter = { if (it == 0) "Off" else "${it}%" },
            onChange = { ControllerMappings.setStickAntiDeadzone(left, it / 100f); refreshToken.value++ },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.stickFeel.sensitivity.label"),
            value = (ControllerMappings.stickSensitivity(left) * 20f).toInt(), // 0.5..2.0 -> 10..40
            min = 10,
            max = 40,
            description = str("pad.stickFeel.sensitivity.description"),
            valueFormatter = { "${it * 5}%" },
            onChange = { ControllerMappings.setStickSensitivity(left, it / 20f); refreshToken.value++ },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.stickFeel.acceleration.label"),
            value = (ControllerMappings.stickAcceleration(left) * 10f).toInt(), // 0.0..2.0 -> 0..20
            min = 0,
            max = 20,
            description = str("pad.stickFeel.acceleration.description"),
            valueFormatter = { if (it == 0) "Off (linear)" else "+%.1f".format(it / 10f) },
            onChange = { ControllerMappings.setStickAcceleration(left, it / 10f); refreshToken.value++ },
        )
        SettingsDivider()
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
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
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
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.weight(1f))
        Text(
            str("pad.action.clear"),
            color = Color(0xFFE57373),
            fontSize = 15.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .clickable { clear() }
                .padding(horizontal = 8.dp, vertical = 2.dp),
        )
        Spacer(Modifier.width(4.dp))
        Text(
            ControllerMappings.stickTargetLabel(code),
            color = Color(0xFFCCCCCC),
            fontSize = 15.sp,
            fontWeight = FontWeight.Bold,
        )
    }
    if (showPicker.value) {
        StickTargetPickerDialog(
            title = "${str(if (leftStick) "pad.leftStick.label" else "pad.rightStick.label")} — ${dir.id.replaceFirstChar { it.uppercase() }}",
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
    val layer = "stick-target-picker"
    // A plain scrolling Column, NOT a LazyColumn: the nav registry only knows about rows that
    // are actually composed, so a lazy list hides everything past the viewport from the pad.
    // This list is bounded (a fixed button set plus the hotkey enum), so composing it all is fine.
    com.armsx2.ui.common.PadModal(key = layer, onDismiss = onDismiss) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 420.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            // Capped to the display so the weighted list above has a bounded height to share
            // out; without this the Column is unbounded and weight() changes nothing.
            Column(
                Modifier
                    .padding(20.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.82f).dp),
            ) {
                Text(title, color = MaterialTheme.colorScheme.onSurface, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(8.dp))
                Column(
                    Modifier
                        // Bounded by the SCREEN, not a fixed 360dp. In landscape the display is
                        // shorter than 360dp plus a title plus a button row, so the list took
                        // more than there was and pushed Save/Cancel off the bottom -- with no
                        // way to commit or dismiss (reported for the macro editor). weight()
                        // lets the buttons claim their height first and gives the list the rest.
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState()),
                ) {
                    Text(
                        str("pad.stickTarget.intro"),
                        color = Color(0xFFBBBBBB), fontSize = 15.sp,
                    )
                    Spacer(Modifier.height(8.dp))
                    StickPickItem(str("pad.stickTarget.analogDefault"), current in 110..123, "$layer.analog") { onPick(null) }
                    Spacer(Modifier.height(6.dp))
                    Text(str("pad.stickTarget.ps2Buttons"), color = Colors.pasx2_blue, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                    ControllerMappings.stickTargets.forEach { t ->
                        StickPickItem(t.label, current == t.code, "$layer.btn.${t.code}") { onPick(t.code) }
                    }
                    Spacer(Modifier.height(6.dp))
                    Text(str("pad.stickTarget.hotkeys"), color = Colors.pasx2_blue, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                    ControllerMappings.SysHotkey.entries.forEach { h ->
                        val hc = ControllerMappings.stickCodeForHotkey(h)
                        StickPickItem("Hotkey: ${h.label}", current == hc, "$layer.hk.${h.name}") { onPick(hc) }
                    }
                }
                Spacer(Modifier.height(14.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    PickerButton(str("action.cancel"), "$layer.cancel", onDismiss)
                }
            }
        }
    }
}

/** Shared footer button for the two pickers below. */
@Composable
private fun PickerButton(label: String, id: String, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.controllerFocusable(
            controllerId = id,
            shape = RoundedCornerShape(14.dp),
            onConfirm = onClick,
        ),
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun StickPickItem(label: String, selected: Boolean, id: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .controllerFocusable(controllerId = id, shape = RoundedCornerShape(10.dp), onConfirm = onClick)
            .padding(vertical = 8.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (selected) "●  " else "○  ",
            color = if (selected) Colors.pasx2_blue else Color(0xFF777777),
            fontSize = 16.sp,
        )
        Text(
            label,
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
        )
    }
}
@androidx.compose.runtime.Composable
/**
 * Analog-stick settings, extracted so the in-game quick menu can host them too.
 *
 * Same arrangement as [GyroSection] and [MacrosSection] and for the same reason: some games
 * have no in-game invert option, so needing All Settings to flip Y mid-session meant leaving
 * the game (Sizor). The section itself is unchanged — it is called from both places.
 */
internal fun AnalogSticksSection(
    editPlayer: androidx.compose.runtime.MutableIntState,
    refreshToken: androidx.compose.runtime.MutableIntState,
    editSerial: String? = null,
) {
    CollapsibleSection(str("pad.section.analogSticks"), initiallyExpanded = false) {
        // Extra button on the ON-SCREEN left stick: a sprint/jump button just above it that
        // you can reach by GLIDING the same thumb up off the stick, without lifting off and
        // losing your heading (GTA / Silent Hill sprint, GoW / KH jump).
        ToggleRow(
            str("pad.analogExtra.label"),
            com.armsx2.ui.touch.TouchControls.analogExtraEnabled.value,
            description = str("pad.analogExtra.description"),
        ) { com.armsx2.ui.touch.TouchControls.setAnalogExtraEnabled(it) }
        if (com.armsx2.ui.touch.TouchControls.analogExtraEnabled.value) {
            run {
                // Reuse the macro target table — it is exactly "every PS2 button a control may
                // fire", already ordered for display and already translated.
                val targets = com.armsx2.ui.touch.TouchControls.macroAssignableTargets
                    .filter { it.code in 0..199 }
                val idx = targets.indexOfFirst {
                    it.code == com.armsx2.ui.touch.TouchControls.analogExtraKeycode.intValue
                }.coerceAtLeast(0)
                SegmentedGridRow(
                    label = str("pad.analogExtra.button"),
                    options = targets.map { it.label },
                    selectedIndex = idx,
                    columns = 4,
                    description = str("pad.analogExtra.button.description"),
                    onChange = {
                        com.armsx2.ui.touch.TouchControls.setAnalogExtraKeycode(targets[it].code)
                    },
                )
            }
            SettingsDivider()
        }
        // Analog stick remapping — make a physical stick act as the D-pad or the
        // face buttons (great for fighting games on analog-centric controllers).
        run {
            val stickOpts = ControllerMappings.StickMode.entries.map { it.label }
            SegmentedRow(
                label = str("pad.leftStick.label"),
                options = stickOpts,
                selectedIndex = ControllerMappings.leftStickModeScope(editPlayer.intValue, editSerial).ordinal,
                description = str("pad.leftStick.description"),
                onChange = {
                    ControllerMappings.setLeftStickMode(ControllerMappings.StickMode.entries[it], editPlayer.intValue, editSerial)
                    refreshToken.intValue++
                },
            )
            SettingsDivider()
            if (ControllerMappings.leftStickModeScope(editPlayer.intValue, editSerial) == ControllerMappings.StickMode.CUSTOM) {
                ControllerMappings.StickDir.entries.forEach { dir ->
                    StickDirPickerRow(leftStick = true, dir = dir, player = editPlayer.intValue, serial = editSerial, onChanged = { refreshToken.intValue++ })
                    SettingsDivider()
                }
            }
            // Axis correction for the LEFT stick — fixes pads that read mirrored/rotated.
            ToggleRow(str("pad.leftStick.swapXY.label"), ControllerMappings.stickSwapXYScope(true, editSerial),
                description = str("pad.leftStick.swapXY.description")) {
                ControllerMappings.setStickSwapXY(true, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            ToggleRow(str("pad.leftStick.invertX.label"), ControllerMappings.stickInvertXScope(true, editSerial),
                description = str("pad.leftStick.invertX.description")) {
                ControllerMappings.setStickInvertX(true, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            ToggleRow(str("pad.leftStick.invertY.label"), ControllerMappings.stickInvertYScope(true, editSerial),
                description = str("pad.leftStick.invertY.description")) {
                ControllerMappings.setStickInvertY(true, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            SegmentedRow(
                label = str("pad.rightStick.label"),
                options = stickOpts,
                selectedIndex = ControllerMappings.rightStickModeScope(editPlayer.intValue, editSerial).ordinal,
                description = str("pad.rightStick.description"),
                onChange = {
                    ControllerMappings.setRightStickMode(ControllerMappings.StickMode.entries[it], editPlayer.intValue, editSerial)
                    refreshToken.intValue++
                },
            )
            SettingsDivider()
            if (ControllerMappings.rightStickModeScope(editPlayer.intValue, editSerial) == ControllerMappings.StickMode.CUSTOM) {
                ControllerMappings.StickDir.entries.forEach { dir ->
                    StickDirPickerRow(leftStick = false, dir = dir, player = editPlayer.intValue, serial = editSerial, onChanged = { refreshToken.intValue++ })
                    SettingsDivider()
                }
            }
            // Axis correction for the RIGHT stick — e.g. the tester's "down is up, left is right".
            ToggleRow(str("pad.rightStick.swapXY.label"), ControllerMappings.stickSwapXYScope(false, editSerial),
                description = str("pad.rightStick.swapXY.description")) {
                ControllerMappings.setStickSwapXY(false, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            ToggleRow(str("pad.rightStick.invertX.label"), ControllerMappings.stickInvertXScope(false, editSerial),
                description = str("pad.rightStick.invertX.description")) {
                ControllerMappings.setStickInvertX(false, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            ToggleRow(str("pad.rightStick.invertY.label"), ControllerMappings.stickInvertYScope(false, editSerial),
                description = str("pad.rightStick.invertY.description")) {
                ControllerMappings.setStickInvertY(false, it, editSerial); refreshToken.intValue++
            }
            SettingsDivider()
            ToggleRow(
                str("pad.dpadAsLeftStick.label"),
                ControllerMappings.dpadAsLeftStickScope(editSerial),
                description = str("pad.dpadAsLeftStick.description"),
            ) {
                ControllerMappings.setDpadAsLeftStick(it, editSerial)
                refreshToken.intValue++
            }
            SettingsDivider()
            // Stick FEEL is per-stick now (tester: lowering sensitivity for
            // camera aim also slowed walking). Existing single-value settings
            // migrate to both sticks automatically.
            StickFeelSliders(left = true, title = str("pad.leftStickFeel.title"), refreshToken = refreshToken)
            StickFeelSliders(left = false, title = str("pad.rightStickFeel.title"), refreshToken = refreshToken)
        }
    }
}


/**
 * Motion / gyroscope controls, shared by the Pad settings tab and the in-game pause
 * menu's Controls tab. [editSerial] selects Global (null) vs per-game scope. Pass
 * [externalRefresh] to share a parent's live-refresh token (the Pad tab does so its
 * scope toggle refreshes every section together); otherwise an internal token drives
 * live re-reads of the raw-pref values Compose can't observe on its own.
 */
@Composable
internal fun GyroSection(
    editSerial: String? = null,
    externalRefresh: androidx.compose.runtime.MutableIntState? = null,
) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val localToken = remember { androidx.compose.runtime.mutableIntStateOf(0) }
    val refreshToken = externalRefresh ?: localToken
    CollapsibleSection(str("pad.gyro.section"), initiallyExpanded = false) {
        @Suppress("UNUSED_EXPRESSION")
        refreshToken.intValue
        val gyroMode = ControllerMappings.gyroModeScope(editSerial)
        SegmentedRow(
            label = str("pad.gyro.mode.label"),
            options = listOf(
                str("pad.gyro.mode.off"),
                str("pad.gyro.mode.aim"),
                str("pad.gyro.mode.steering"),
            ),
            selectedIndex = gyroMode,
            onChange = {
                ControllerMappings.setGyroMode(it, editSerial)
                refreshToken.intValue++
            },
        )
        // Which analog stick Aim mode drives — Right for most FPS, Left for games that
        // aim with the left stick (e.g. Resident Evil 4). Only shown in Aim mode.
        if (gyroMode == ControllerMappings.GYRO_AIM) {
            SegmentedRow(
                label = str("pad.gyro.aimStick.label"),
                options = listOf(str("pad.gyro.aimStick.right"), str("pad.gyro.aimStick.left")),
                selectedIndex = ControllerMappings.gyroAimStickScope(editSerial),
                onChange = {
                    ControllerMappings.setGyroAimStick(it, editSerial)
                    refreshToken.intValue++
                },
            )
        }
        // Report which sensor the mode will actually use. Aim prefers a real gyroscope and
        // steering the game rotation vector, but both fall back to the accelerometer, which
        // essentially every device has — so "unavailable" is now genuinely rare. Say when
        // we're on the fallback, because tilt behaves differently from a gyro (absolute
        // angle rather than turn rate, and no yaw at all). The manifest declares the
        // gyroscope feature not-required, so gyro-less devices still install.
        if (gyroMode != 0) {
            when (com.armsx2.input.AndroidGyroscopeInput.resolveKind(ctx, gyroMode)) {
                com.armsx2.input.AndroidGyroscopeInput.KIND_NONE ->
                    HelpText(str("pad.gyro.unavailable"))
                com.armsx2.input.AndroidGyroscopeInput.KIND_TILT ->
                    HelpText(
                        if (gyroMode == ControllerMappings.GYRO_AIM)
                            str("pad.gyro.tiltFallback.aim")
                        else
                            str("pad.gyro.tiltFallback.steering")
                    )
                else -> Unit
            }
        }
        SettingsDivider()
        IntSliderRow(
            label = str("pad.gyro.sensitivity.label"),
            value = ControllerMappings.gyroSensitivityScope(editSerial),
            min = 25,
            max = 300,
            valueFormatter = { "${it}%" },
            onChange = {
                ControllerMappings.setGyroSensitivity(it, editSerial)
                refreshToken.intValue++
            },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("pad.gyro.smoothing.label"),
            value = ControllerMappings.gyroSmoothingScope(editSerial),
            min = 0,
            max = 90,
            valueFormatter = { "${it}%" },
            onChange = {
                ControllerMappings.setGyroSmoothing(it, editSerial)
                refreshToken.intValue++
            },
        )
        SettingsDivider()
        ToggleRow(
            str("pad.gyro.invertX.label"),
            ControllerMappings.gyroInvertXScope(editSerial),
        ) {
            ControllerMappings.setGyroInvertX(it, editSerial)
            refreshToken.intValue++
        }
        SettingsDivider()
        ToggleRow(
            str("pad.gyro.invertY.label"),
            ControllerMappings.gyroInvertYScope(editSerial),
        ) {
            ControllerMappings.setGyroInvertY(it, editSerial)
            refreshToken.intValue++
        }
        SettingsDivider()
    }
}

/**
 * Macros — 4 combo buttons, each firing a chosen SET of pad buttons at once (e.g. R1+R2+R3).
 * Shared between the full Pad settings tab and the in-game Controls tab. Tap a row to pick
 * its buttons (the M1-M4 on-screen buttons + any physical trigger fire that set).
 *
 * Physical-trigger binding needs a capture host (the Pad tab's root key listener), so the
 * "Bind"/"Clear" column only renders when [onArmCapture] is supplied. In the in-game quick
 * menu both params are null: you can still edit each macro's button set, just not bind a
 * controller button to it (do that from All Settings › Controls).
 */
@Composable
internal fun MacrosSection(
    macroCapture: MutableState<TouchButtonId?>? = null,
    onArmCapture: ((TouchButtonId) -> Unit)? = null,
) {
    val macroDialogFor = remember { mutableStateOf<TouchButtonId?>(null) }
    // Recompose when any macro's button set / physical bind changes.
    @Suppress("UNUSED_EXPRESSION")
    TouchControls.macroBindTick.value
    val physicalSupported = onArmCapture != null
    CollapsibleSection(str("pad.section.macros"), initiallyExpanded = false) {
        Text(
            str("pad.macros.header"),
            color = Color(0xFFBBBBBB),
            fontSize = 15.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
        )
        listOf(TouchButtonId.MACRO1, TouchButtonId.MACRO2, TouchButtonId.MACRO3, TouchButtonId.MACRO4).forEach { mid ->
            val buttons = TouchControls.macroCodes(mid)
            val summary = if (buttons.isEmpty()) str("pad.macro.notSet")
            else buttons.joinToString(" + ") { TouchControls.macroTargetFor(it)?.label ?: "?" }
            val physCode = TouchControls.macroPhysicalCode(mid)
            val capturingThis = macroCapture?.value == mid
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(72.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(rowAura())
                    .clickable { macroDialogFor.value = mid }
                    .controllerFocusable(
                        controllerId = "pad-macro-${mid.name}",
                        onConfirm = { macroDialogFor.value = mid },
                    )
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(mid.label, color = Colors.pasx2_blue, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(summary, color = Color(0xFFCCCCCC), fontSize = 15.sp)
                    if (physicalSupported) {
                        Text(
                            when {
                                capturingThis -> str("pad.pressControllerButton")
                                physCode != android.view.KeyEvent.KEYCODE_UNKNOWN ->
                                    "Controller: ${ControllerMappings.labelForKey(physCode)}"
                                else -> str("pad.controller.notBound")
                            },
                            color = if (capturingThis) Color(0xFFFFD33A) else Color(0xFF999999),
                            fontSize = 14.sp,
                        )
                    }
                }
                if (physicalSupported) {
                    if (physCode != android.view.KeyEvent.KEYCODE_UNKNOWN && !capturingThis) {
                        Text(
                            str("pad.action.clear"),
                            color = Color(0xFFFF6B6B), fontSize = 14.sp, fontWeight = FontWeight.Bold,
                            modifier = Modifier
                                .clickable { TouchControls.clearMacroPhysicalCode(mid) }
                                .padding(end = 10.dp),
                        )
                    }
                    Text(
                        if (capturingThis) str("action.cancel") else str("pad.action.bind"),
                        color = Colors.pasx2_blue, fontSize = 14.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable { onArmCapture?.invoke(mid) }
                            .padding(end = 10.dp),
                    )
                }
                Text(str("pad.action.edit"), color = Colors.pasx2_blue, fontSize = 14.sp, fontWeight = FontWeight.Bold)
            }
            // Frequency (turbo). Only once the macro fires something — a rate for a macro
            // with no buttons is a row that does nothing. Lives HERE rather than in the
            // edit dialog because a Dialog is its own focused window and swallows the pad;
            // as a normal row it's reachable with a controller like everything else.
            if (buttons.isNotEmpty()) {
                val freq = TouchControls.macroFrequency(mid)
                // str() is @Composable and valueFormatter is a plain lambda, so resolve
                // both up-front rather than calling into it from there.
                val holdLabel = str("pad.macro.frequency.hold")
                val everyLabel = str("pad.macro.frequency.every")
                IntSliderRow(
                    label = str("pad.macro.frequency.label"),
                    value = freq,
                    min = 0,
                    max = TouchControls.MACRO_FREQ_MAX,
                    description = str("pad.macro.frequency.description"),
                    valueFormatter = { if (it == 0) holdLabel else everyLabel.format(it) },
                    onReset = if (freq == 0) null else ({ TouchControls.setMacroFrequency(mid, 0) }),
                    onChange = { TouchControls.setMacroFrequency(mid, it) },
                )
            }
            SettingsDivider()
        }
        macroDialogFor.value?.let { mid ->
            MacroConfigDialog(
                macroId = mid,
                onSaved = { },
                onDismiss = { macroDialogFor.value = null },
            )
        }
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
        mutableStateListOf<Int>().apply { addAll(TouchControls.macroCodes(macroId)) }
    }
    val layer = "macro-config:${macroId.name}"
    val save = {
        TouchControls.setMacroCodes(macroId, selected.toList())
        onSaved()
        onDismiss()
    }
    com.armsx2.ui.common.PadModal(key = layer, onDismiss = onDismiss) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 420.dp),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            // Capped to the display so the weighted list above has a bounded height to share
            // out; without this the Column is unbounded and weight() changes nothing.
            Column(
                Modifier
                    .padding(20.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.82f).dp),
            ) {
                Text(
                    "${str("pad.action.edit")}: ${macroId.label}",
                    color = MaterialTheme.colorScheme.onSurface,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(8.dp))
                // Plain Column, not Lazy — see the note on the stick picker.
                Column(
                    Modifier
                        // Bounded by the SCREEN, not a fixed 360dp. In landscape the display is
                        // shorter than 360dp plus a title plus a button row, so the list took
                        // more than there was and pushed Save/Cancel off the bottom -- with no
                        // way to commit or dismiss (reported for the macro editor). weight()
                        // lets the buttons claim their height first and gives the list the rest.
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState()),
                ) {
                    Text(
                        str("pad.macroConfig.intro"),
                        color = Color(0xFFBBBBBB), fontSize = 15.sp,
                    )
                    Spacer(Modifier.height(8.dp))
                    TouchControls.macroAssignableTargets.forEach { t ->
                        val on = t.code in selected
                        val toggle = { if (on) selected.remove(t.code) else selected.add(t.code); Unit }
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .height(52.dp)
                                .clickable { toggle() }
                                // Left/Right clear and set, matching every other toggle in the app,
                                // so the row behaves the same inside this panel as outside it.
                                .controllerFocusable(
                                    controllerId = "$layer.${t.code}",
                                    shape = RoundedCornerShape(10.dp),
                                    onConfirm = toggle,
                                    onLeft = { if (on) selected.remove(t.code) },
                                    onRight = { if (!on) selected.add(t.code) },
                                )
                                .padding(horizontal = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                if (on) "☑" else "☐",
                                color = if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                                fontSize = 16.sp,
                            )
                            Spacer(Modifier.width(12.dp))
                            Text(t.label, color = MaterialTheme.colorScheme.onSurface, fontSize = 14.sp)
                        }
                    }
                }
                Spacer(Modifier.height(14.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    PickerButton(str("action.cancel"), "$layer.cancel", onDismiss)
                    Spacer(Modifier.width(10.dp))
                    PickerButton(str("action.save"), "$layer.save", save)
                }
            }
        }
    }
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
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable(onClick = onClick)
            .controllerFocusable(
                controllerId = "pad:${action.id}",
                onConfirm = onClick,
            )
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(action.label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.weight(1f))
        // "Clear" unbinds the button (leaves it blank, free to assign as a hotkey) —
        // mirrors the Hotkeys tab. Shown only when bound and not mid-capture.
        if (!capturing && physical != android.view.KeyEvent.KEYCODE_UNKNOWN) {
            Text(
                str("pad.action.clear"),
                color = Color(0xFFFF6B6B),
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier
                    .clickable(onClick = onClear)
                    .padding(end = 10.dp),
            )
        }
        Text(
            if (capturing) str("pad.pressButton") else ControllerMappings.labelForKey(physical),
            color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
            fontSize = 15.sp,
            fontWeight = FontWeight.Bold,
        )
    }
}
