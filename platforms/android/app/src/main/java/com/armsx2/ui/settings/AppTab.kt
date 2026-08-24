package com.armsx2.ui.settings

import androidx.compose.foundation.BorderStroke
import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.graphics.Color
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.widget.Toast
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.navigation.AppRoute
import com.armsx2.navigation.UiNavigator
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.theme.BootLogoPreferences
import com.armsx2.ui.theme.ThemeMode
import com.armsx2.ui.theme.ThemePreferences
import com.armsx2.ui.theme.LauncherOrientationPreferences
import com.armsx2.ui.theme.ToolbarPositionPreferences
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.graphics.Brush
import com.armsx2.ui.theme.LibraryBackgroundColorPreferences
import com.armsx2.ui.theme.LibraryChromePreferences
import java.io.File

@Composable
fun AppTab() {
    val currentLanguage = I18n.languages.firstOrNull { it.code == I18n.current }
    val appContext = LocalContext.current

    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        // In-app GitHub-release updater. Github (sideload) flavor only: the play flavor's
        // UpdaterEntry is a no-op stub and IN_APP_UPDATER is false, so no updater code or
        // REQUEST_INSTALL_PACKAGES ships in the AAB (build-play-aab.sh also fails closed).
        if (com.armsx2.BuildConfig.IN_APP_UPDATER) {
            com.armsx2.update.UpdaterEntry()
        }
        Surface(
            onClick = { UiNavigator.navigate(AppRoute.Language) },
            modifier = Modifier.fillMaxWidth()
                .controllerFocusable("app.language", RoundedCornerShape(20.dp), onConfirm = { UiNavigator.navigate(AppRoute.Language) }),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Surface(
                    modifier = Modifier.size(46.dp),
                    shape = RoundedCornerShape(14.dp),
                    color = MaterialTheme.colorScheme.primaryContainer,
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
                        Text("◎", color = MaterialTheme.colorScheme.primary, fontSize = 23.sp, fontWeight = FontWeight.Bold)
                    }
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(str("app.language"), style = MaterialTheme.typography.titleMedium)
                    Text(
                        if (I18n.selected == I18n.SYSTEM_CODE) str("app.language.system") else currentLanguage?.nativeName ?: "English",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text("›", color = MaterialTheme.colorScheme.primary, fontSize = 26.sp)
            }
        }

        // Theme picker. NOT a SegmentedRow: that's a fixed-width Box, so eleven options would
        // squeeze into unreadable slivers — this wraps instead. Driven straight off the enum so
        // adding a colour needs no index bookkeeping; the old version mapped index<->mode by hand
        // in two separate places, which is precisely how such pairs drift out of sync.
        Column(Modifier.fillMaxWidth().padding(vertical = 5.dp)) {
            Text(str("app.theme"), style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(7.dp),
                verticalArrangement = Arrangement.spacedBy(7.dp),
            ) {
                // Material You needs Android 12. Hide it below that rather than letting it fall
                // back silently — picking a theme and getting a different one reads as a bug.
                ThemeMode.entries.filter {
                    !it.requiresDynamicColor || Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                }.forEach { theme ->
                    val apply = { ThemePreferences.set(theme) }
                    FilterChip(
                        selected = ThemePreferences.mode.value == theme,
                        onClick = apply,
                        label = { Text(str("app.theme.${theme.name.lowercase()}")) },
                        shape = RoundedCornerShape(11.dp),
                        modifier = Modifier.controllerFocusable(
                            "app.theme.${theme.name}",
                            RoundedCornerShape(11.dp),
                            onConfirm = apply,
                        ),
                    )
                }
            }

            // OLED black is a MODIFIER on whichever theme is chosen above, not a theme of its own,
            // so "OLED + purple" / "OLED + yellow" are possible (the standalone OLED chip stays as
            // the neutral blue-accent preset). A chip rather than a ToggleRow so it lives with the
            // theme chips and keeps the controllerFocusable registration pad navigation needs.
            Spacer(Modifier.height(10.dp))
            run {
                val toggleOled = { ThemePreferences.setOledBase(!ThemePreferences.oledBase.value) }
                FilterChip(
                    selected = ThemePreferences.oledBase.value,
                    onClick = toggleOled,
                    label = { Text(str("app.theme.oledBase")) },
                    shape = RoundedCornerShape(11.dp),
                    modifier = Modifier.controllerFocusable(
                        "app.theme.oledBase",
                        RoundedCornerShape(11.dp),
                        onConfirm = toggleOled,
                    ),
                )
            }

            // RGB picker, only while Custom is the active theme. The scheme is derived from
            // this colour's hue with saturation/brightness clamped (see customScheme), so the
            // accent stays recognisably what was picked without any channel combination being
            // able to produce unreadable chrome.
            if (ThemePreferences.mode.value == ThemeMode.Custom) {
                val argb = ThemePreferences.customColor.value
                Spacer(Modifier.height(10.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Surface(
                        modifier = Modifier.size(34.dp),
                        shape = RoundedCornerShape(9.dp),
                        color = Color(argb),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
                    ) {}
                    Spacer(Modifier.width(10.dp))
                    Text(
                        String.format("#%06X", 0xFFFFFF and argb),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                listOf(
                    Triple("app.theme.custom.r", 16, android.graphics.Color.red(argb)),
                    Triple("app.theme.custom.g", 8, android.graphics.Color.green(argb)),
                    Triple("app.theme.custom.b", 0, android.graphics.Color.blue(argb)),
                ).forEach { (labelKey, shift, value) ->
                    IntSliderRow(
                        label = str(labelKey),
                        value = value,
                        min = 0,
                        max = 255,
                        onChange = { channel ->
                            // Replace just this channel, keeping alpha opaque.
                            val cleared = argb and (0xFF shl shift).inv()
                            ThemePreferences.setCustomColor(cleared or (channel shl shift) or (0xFF shl 24))
                        },
                    )
                }
            }
        }

        // Library background (wave) color. Separate from the theme accent above — that only tints
        // the UI chrome; this recolors the animated backdrop itself, with the white waves riding
        // over it. Unset = the built-in blue. Live: XmbGlView's GL thread reads the new color on
        // its next frame (~33ms), so the backdrop updates as the sliders move.
        Column(Modifier.fillMaxWidth().padding(vertical = 5.dp)) {
            Text(str("app.bgColor"), style = MaterialTheme.typography.titleMedium)
            Text(
                str("app.bgColor.desc"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            // Opt into the lightweight 2D animated backdrop everywhere (the same one older Mali /
            // GL-fail devices already get) instead of the GLES3 XMB wave. The colour options below
            // apply to it too. No effect if a custom background image is set.
            ToggleRow(
                label = str("app.bg.simple"),
                value = com.armsx2.ui.home.LibraryBackground.animated2D.value,
                description = str("app.bg.simple.desc"),
                onChange = { com.armsx2.ui.home.LibraryBackground.setAnimated2D(it) },
            )
            // Flurry: Calum Robinson's 2002 screensaver, ported from ARMSX3 and offered as the
            // backdrop.
            //
            // Off by default and said plainly in the description, because it is a live particle
            // simulation rather than a still: this library shipped a looping video once and lost
            // it in 2.5.9 when the continuous decode turned out to cost real performance. Opt-in
            // for the same reason.
            ToggleRow(
                label = str("app.bg.flurry"),
                value = com.armsx2.ui.home.LibraryBackground.flurry.value,
                description = str("app.bg.flurry.desc"),
                onChange = { com.armsx2.ui.home.LibraryBackground.setFlurry(it) },
            )
            if (com.armsx2.ui.home.LibraryBackground.flurry.value) {
                // Which saver runs. They share one on/off above because only one background can
                // draw at a time, and "animated background: on" reads better than seven switches.
                // Flurry is Calum Robinson's (BSD-3-clause); the rest are Terry Welsh's Really
                // Slick Screensavers (GPL-2.0-or-later).
                val kind = com.armsx2.ui.home.LibraryBackground.saverKind.value
                SegmentedGridRow(
                    label = str("app.bg.saver"),
                    options = listOf("Flurry", "Flux", "Plasma", "SolarWinds", "Hyperspace", "Lattice", "Skyrocket"),
                    selectedIndex = kind,
                    columns = 4,
                    onChange = { com.armsx2.ui.home.LibraryBackground.setSaverKind(it) },
                )
                if (kind == 0) {
                    // Values are Flurry's own preset enum; -1 is "insane" upstream and 99 is this
                    // port's "pick one each time", so the list is not an index range.
                    val presetValues = listOf(99, 0, 1, 2, 3, 4, 5, -1)
                    SegmentedGridRow(
                        label = str("app.bg.flurry.preset"),
                        options = listOf(
                            str("app.bg.flurry.random"), "Water", "Fire", "Psychedelic",
                            "RGB", "Binary", "Classic", "Insane",
                        ),
                        selectedIndex = presetValues
                            .indexOf(com.armsx2.ui.home.LibraryBackground.flurryPreset.value)
                            .coerceAtLeast(0),
                        columns = 4,
                        onChange = {
                            com.armsx2.ui.home.LibraryBackground.setFlurryPreset(presetValues[it])
                        },
                    )
                } else if (kind != 4 && kind != 6) {
                    // Hyperspace and Skyrocket ship no presets upstream, so they show no picker.
                    // Six presets plus this port's 99 for "pick one each time", so not an index
                    // range. Flux and SolarWinds ship their own named defaults; Plasma had none
                    // upstream, so those are built from the settings its config dialog exposed.
                    val rssValues = listOf(99, 1, 2, 3, 4, 5, 6)
                    val names = when (kind) {
                        1 -> listOf("Regular", "Hypnotic", "Insane", "Sparklers", "Paradigm", "Galactic")
                        2 -> listOf("Classic", "Tight", "Wide", "Fast", "Slow drift", "Coarse")
                        5 -> listOf("Regular", "Chainmail", "Brass Mesh", "Computer", "Slick", "Tasty")
                        else -> listOf("Regular", "Cosmic Strings", "Cold Pricklies", "Space Fur", "Jiggly", "Undertow")
                    }
                    SegmentedGridRow(
                        label = str("app.bg.flux.preset"),
                        options = listOf(str("app.bg.flurry.random")) + names,
                        selectedIndex = rssValues
                            .indexOf(com.armsx2.ui.home.LibraryBackground.rssPreset.value)
                            .coerceAtLeast(0),
                        columns = 4,
                        onChange = {
                            com.armsx2.ui.home.LibraryBackground.setRssPreset(rssValues[it])
                        },
                    )
                }
            }
            // Colour of the BAR itself (the rounded header pill), as opposed to the animated
            // backdrop the rest of this section controls. Requested because the background picker
            // is labelled "Library Bar Color" but recolours the background — so there was no way to
            // colour the actual bar. "Default" hands it back to the theme.
            run {
                val barArgb = com.armsx2.ui.theme.LibraryChromePreferences.barColor.value
                Spacer(Modifier.height(10.dp))
                Text(str("app.barColor"), style = MaterialTheme.typography.titleMedium)
                Text(
                    str("app.barColor.desc"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(8.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(7.dp),
                    verticalArrangement = Arrangement.spacedBy(7.dp),
                ) {
                    val clearBar = { com.armsx2.ui.theme.LibraryChromePreferences.setBarColor(0) }
                    FilterChip(
                        selected = barArgb == 0,
                        onClick = clearBar,
                        label = { Text(str("app.barColor.default")) },
                        shape = RoundedCornerShape(11.dp),
                        modifier = Modifier.controllerFocusable(
                            "app.barColor.default", RoundedCornerShape(11.dp), onConfirm = clearBar,
                        ),
                    )
                    com.armsx2.ui.theme.LibraryChromePreferences.BAR_PRESETS.forEach { preset ->
                        val pick = { com.armsx2.ui.theme.LibraryChromePreferences.setBarColor(preset) }
                        val selected = barArgb == preset
                        Surface(
                            onClick = pick,
                            modifier = Modifier.size(34.dp)
                                .controllerFocusable("app.barColor.$preset", RoundedCornerShape(9.dp), onConfirm = pick),
                            shape = RoundedCornerShape(9.dp),
                            color = Color(preset),
                            border = BorderStroke(
                                if (selected) 3.dp else 1.dp,
                                if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline,
                            ),
                        ) {}
                    }
                }
            }
            // Continuous RGB hue-cycle — same idea as the theme's RGB mode. While on, the fixed
            // color (presets + sliders) doesn't apply, so it's hidden.
            ToggleRow(
                label = str("app.bgColor.rgb"),
                value = LibraryBackgroundColorPreferences.rgbCycle.value,
                description = str("app.bgColor.rgb.desc"),
                onChange = { LibraryBackgroundColorPreferences.setRgbCycle(it) },
            )
            if (!LibraryBackgroundColorPreferences.rgbCycle.value) {
            val customized = LibraryBackgroundColorPreferences.color.value != 0
            val argb = if (customized) LibraryBackgroundColorPreferences.color.value
                       else LibraryBackgroundColorPreferences.DefaultDisplayColor
            val r = android.graphics.Color.red(argb)
            val g = android.graphics.Color.green(argb)
            val b = android.graphics.Color.blue(argb)
            // Custom-slider mode: open when a non-preset colour is active, or the user taps "Custom".
            val customOpen = remember {
                mutableStateOf(customized && LibraryBackgroundColorPreferences.PRESETS.none { it == argb })
            }
            // Quick-pick presets (XMB palette) + a "Custom" chip that reveals the RGB sliders — the
            // same shape as the theme colour picker. The active swatch (or Custom) is ringed.
            Spacer(Modifier.height(9.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(7.dp),
                verticalArrangement = Arrangement.spacedBy(7.dp),
            ) {
                LibraryBackgroundColorPreferences.PRESETS.forEach { preset ->
                    val selected = !customOpen.value && preset == argb
                    val pick = { customOpen.value = false; LibraryBackgroundColorPreferences.set(preset) }
                    Surface(
                        onClick = pick,
                        modifier = Modifier.size(34.dp)
                            .controllerFocusable("app.bgColor.preset.$preset", RoundedCornerShape(9.dp), onConfirm = pick),
                        shape = RoundedCornerShape(9.dp),
                        color = Color(preset),
                        border = BorderStroke(
                            if (selected) 3.dp else 1.dp,
                            if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline,
                        ),
                    ) {}
                }
                val pickCustom = {
                    customOpen.value = true
                    if (!customized) LibraryBackgroundColorPreferences.set(LibraryBackgroundColorPreferences.DefaultDisplayColor)
                }
                FilterChip(
                    selected = customOpen.value,
                    onClick = pickCustom,
                    label = { Text(str("app.theme.custom")) },
                    shape = RoundedCornerShape(9.dp),
                    modifier = Modifier.controllerFocusable("app.bgColor.custom", RoundedCornerShape(9.dp), onConfirm = pickCustom),
                )
            }
            // Preview + RGB sliders only in Custom mode (mirrors the theme colour picker).
            if (customOpen.value) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Surface(
                        modifier = Modifier.size(width = 66.dp, height = 34.dp),
                        shape = RoundedCornerShape(9.dp),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
                    ) {
                        Box(
                            Modifier.fillMaxSize().background(
                                Brush.verticalGradient(
                                    listOf(
                                        Color(r * 0.20f / 255f, g * 0.20f / 255f, b * 0.20f / 255f, 1f),
                                        Color(argb),
                                    )
                                )
                            )
                        )
                    }
                    Spacer(Modifier.width(10.dp))
                    Text(
                        String.format("#%06X", 0xFFFFFF and argb),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                listOf(
                    Triple("app.theme.custom.r", 16, r),
                    Triple("app.theme.custom.g", 8, g),
                    Triple("app.theme.custom.b", 0, b),
                ).forEach { (labelKey, shift, value) ->
                    IntSliderRow(
                        label = str(labelKey),
                        value = value,
                        min = 0,
                        max = 255,
                        onChange = { channel ->
                            val cleared = argb and (0xFF shl shift).inv()
                            LibraryBackgroundColorPreferences.set(cleared or (channel shl shift) or (0xFF shl 24))
                        },
                    )
                }
            }
            if (customized) {
                Spacer(Modifier.height(4.dp))
                val reset = {
                    customOpen.value = false
                    LibraryBackgroundColorPreferences.reset()
                }
                OutlinedButton(
                    onClick = reset,
                    modifier = Modifier.controllerFocusable("app.bgColor.reset", onConfirm = reset),
                ) { Text(str("action.reset")) }
            }
            }
        }

        ToggleRow(
            label = str("app.bootLogo"),
            value = BootLogoPreferences.enabled.value,
            description = str("app.bootLogo.desc"),
            onChange = { BootLogoPreferences.set(it) },
        )

        BackupRestoreRows()

        ToggleRow(
            label = str("app.blockHome"),
            value = com.armsx2.ui.ScreenPinning.enabled.value,
            description = str("app.blockHome.desc"),
            onChange = { com.armsx2.ui.ScreenPinning.set(it) },
        )

        ToggleRow(
            label = str("secondScreen.label"),
            value = com.armsx2.SecondScreen.enabled.value,
            description = str("secondScreen.desc"),
            onChange = { com.armsx2.SecondScreen.set(appContext, it) },
        )

        if (com.armsx2.SecondScreen.enabled.value) {
            ToggleRow(
                label = str("secondScreen.moveOsd"),
                value = com.armsx2.SecondScreen.moveOsd.value,
                description = str("secondScreen.moveOsd.desc"),
                onChange = { com.armsx2.SecondScreen.setMoveOsd(it) },
            )

            // The panel now takes its colours from whichever theme is selected, so this is only
            // about the GROUND behind the tiles: the theme's own, the library's backdrop for
            // continuity with the screen it sits beside, or black for an OLED second display.
            if (com.armsx2.SecondScreen.ignoredDisplays.value.isNotEmpty()) {
                Row(
                    Modifier.fillMaxWidth().padding(top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val clear = { com.armsx2.SecondScreen.clearIgnoredDisplays() }
                    OutlinedButton(
                        onClick = clear,
                        modifier = Modifier.controllerFocusable("secondScreen.displays.reset", onConfirm = clear),
                    ) {
                        Text(
                            str("secondScreen.displays.reset") +
                                " (" + com.armsx2.SecondScreen.ignoredDisplays.value.size + ")",
                        )
                    }
                }
            }

            ToggleRow(
                label = str("secondScreen.topBar"),
                value = com.armsx2.SecondScreen.topBar.value,
                description = str("secondScreen.topBar.desc"),
                onChange = { com.armsx2.SecondScreen.setTopBar(it) },
            )

            val panelBgPicker = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { picked -> picked?.let { com.armsx2.SecondScreen.setBackgroundImage(appContext, it) } }

            SegmentedRow(
                label = str("secondScreen.background"),
                options = listOf(
                    str("secondScreen.background.theme"),
                    str("secondScreen.background.library"),
                    str("secondScreen.background.black"),
                    str("secondScreen.background.custom"),
                ),
                selectedIndex = com.armsx2.SecondScreen.background.value,
                onChange = {
                    // Picking "Custom" with nothing chosen yet opens the picker rather than
                    // selecting a mode that would render as the theme ground and look broken.
                    if (it == com.armsx2.SecondScreen.BG_CUSTOM &&
                        com.armsx2.SecondScreen.backgroundUri.value == null
                    ) {
                        panelBgPicker.launch(arrayOf("image/*"))
                    } else {
                        com.armsx2.SecondScreen.setBackground(it)
                    }
                },
            )
            if (com.armsx2.SecondScreen.background.value == com.armsx2.SecondScreen.BG_CUSTOM) {
                Row(
                    Modifier.fillMaxWidth().padding(top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val pick = { panelBgPicker.launch(arrayOf("image/*")) }
                    OutlinedButton(
                        onClick = pick,
                        modifier = Modifier.controllerFocusable("secondScreen.background.choose", onConfirm = pick),
                    ) { Text(str("secondScreen.background.choose")) }
                }
            }

            // Only worth showing once a thermal tile is actually on the panel — otherwise it is
            // a control over something invisible.
            if (com.armsx2.SecondScreenLayout.tiles().any {
                    it == com.armsx2.SecondScreenTile.CPU_TEMP ||
                        it == com.armsx2.SecondScreenTile.GPU_TEMP ||
                        it == com.armsx2.SecondScreenTile.BATTERY_TEMP
                }
            ) {
                val seconds = listOf(1, 2, 3, 5)
                SegmentedRow(
                    label = str("secondScreen.tempInterval"),
                    options = seconds.map { "${it}s" },
                    selectedIndex = seconds.indexOf(com.armsx2.SecondScreen.tempIntervalSec.value)
                        .coerceAtLeast(0),
                    onChange = { com.armsx2.SecondScreen.setTempInterval(seconds[it]) },
                )
            }

            // Panel layout editor. Chips for what is on the panel, arrows for the order, a column
            // count for the shape of the grid. Asked for as "a grid which can be filled with boxes
            // containing the things one need" (NiceRon) — the point is that the panel is different
            // for a save-scummer and for someone watching frame pacing, so it cannot be one fixed
            // arrangement.
            //
            // Reading the generation subscribes this whole block, so a toggle re-renders the chips
            // AND the order list without either one owning the state.
            com.armsx2.SecondScreenLayout.generation.intValue
            val placed = com.armsx2.SecondScreenLayout.tiles()
            Text(
                str("secondScreen.layout"),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 12.dp),
            )
            Text(
                str("secondScreen.layout.desc"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 8.dp),
            )
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                com.armsx2.SecondScreenTile.entries.forEach { tile ->
                    val toggle = {
                        com.armsx2.SecondScreenLayout.toggle(tile)
                        com.armsx2.SecondScreen.rebuild()
                    }
                    FilterChip(
                        selected = tile in placed,
                        onClick = toggle,
                        label = { Text(str(tile.labelKey)) },
                        shape = RoundedCornerShape(11.dp),
                        modifier = Modifier.controllerFocusable(
                            "secondScreen.tile.${tile.id}",
                            RoundedCornerShape(11.dp),
                            onConfirm = toggle,
                        ),
                    )
                }
            }
            // Order, one row per placed tile. Only shown once there is something to order.
            if (placed.size > 1) {
                placed.forEachIndexed { index, tile ->
                    Row(
                        Modifier.fillMaxWidth().padding(vertical = 2.dp),
                        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                    ) {
                        Text(
                            "${index + 1}. ${str(tile.labelKey)}",
                            style = MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.weight(1f),
                        )
                        val up = {
                            com.armsx2.SecondScreenLayout.move(tile, -1)
                            com.armsx2.SecondScreen.rebuild()
                        }
                        val down = {
                            com.armsx2.SecondScreenLayout.move(tile, 1)
                            com.armsx2.SecondScreen.rebuild()
                        }
                        OutlinedButton(
                            onClick = up,
                            enabled = index > 0,
                            modifier = Modifier.controllerFocusable(
                                "secondScreen.up.${tile.id}",
                                RoundedCornerShape(11.dp),
                                onConfirm = up,
                            ),
                        ) { Text("▲") }
                        Spacer(Modifier.width(6.dp))
                        OutlinedButton(
                            onClick = down,
                            enabled = index < placed.lastIndex,
                            modifier = Modifier.controllerFocusable(
                                "secondScreen.down.${tile.id}",
                                RoundedCornerShape(11.dp),
                                onConfirm = down,
                            ),
                        ) { Text("▼") }
                    }
                }
            }
            IntSliderRow(
                label = str("secondScreen.layout.columns"),
                value = com.armsx2.SecondScreenLayout.columns(),
                min = 1,
                max = 6,
                onChange = {
                    com.armsx2.SecondScreenLayout.setColumns(it)
                    com.armsx2.SecondScreen.rebuild()
                },
            )
            // Columns already decide width (tiles split the row equally), so this is the other
            // axis. 0 keeps the old behaviour of being exactly as tall as the text.
            IntSliderRow(
                label = str("secondScreen.layout.tileHeight"),
                value = com.armsx2.SecondScreenLayout.tileHeight(),
                min = 0,
                max = 160,
                onChange = {
                    com.armsx2.SecondScreenLayout.setTileHeight(it)
                    com.armsx2.SecondScreen.rebuild()
                },
            )
            val resetLayout = {
                com.armsx2.SecondScreenLayout.reset()
                com.armsx2.SecondScreen.rebuild()
            }
            OutlinedButton(
                onClick = resetLayout,
                modifier = Modifier
                    .padding(top = 4.dp)
                    .controllerFocusable(
                        "secondScreen.layout.reset",
                        RoundedCornerShape(11.dp),
                        onConfirm = resetLayout,
                    ),
            ) { Text(str("secondScreen.layout.reset")) }
        }

        // Companion-app access to the recently-played list, over the RecentGamesContentProvider.
        // Off by default and deliberately so: the provider is exported without a permission (it
        // has to be, for a third-party companion to reach it), so while this is on, any app on
        // the device can read the list — including the file URIs, which carry your folder layout.
        run {
            val shareKey = com.armsx2.data.library.RecentGamesContentProvider.KEY_SHARE_ENABLED
            val shareRecent = remember {
                mutableStateOf(
                    com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean(shareKey, false),
                )
            }
            ToggleRow(
                label = str("app.shareRecentGames"),
                value = shareRecent.value,
                description = str("app.shareRecentGames.desc"),
            ) { on ->
                shareRecent.value = on
                // commit(), not apply(): the reader is a DIFFERENT process that can be queried
                // the moment this returns, and apply() only guarantees the in-memory value.
                runCatching {
                    com.armsx2.runtime.MainActivityRuntime.prefs.edit()
                        .putBoolean(shareKey, on).commit()
                }
            }
        }

        ToggleRow(
            label = str("app.batteryWarnings"),
            value = com.armsx2.BatteryWatcher.enabled.value,
            description = str("app.batteryWarnings.desc"),
            onChange = { com.armsx2.BatteryWatcher.set(it) },
        )

        ToggleRow(
            label = str("app.libraryMusic"),
            value = com.armsx2.LibraryMusic.enabled.value,
            description = str("app.libraryMusic.desc"),
            onChange = { com.armsx2.LibraryMusic.set(appContext, it) },
        )
        if (com.armsx2.LibraryMusic.enabled.value) {
            IntSliderRow(
                label = str("app.libraryMusic.volume"),
                value = com.armsx2.LibraryMusic.volumePercent.value,
                min = 0,
                max = 100,
                valueFormatter = { "$it%" },
                onChange = { com.armsx2.LibraryMusic.setVolume(it) },
            )
            // Custom track: plays a file the user picked from their own device. The app never
            // ships or redistributes it — same model as importing a texture pack or skin.
            val musicPicker = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { uri ->
                if (uri != null) {
                    val name = androidx.documentfile.provider.DocumentFile
                        .fromSingleUri(appContext, uri)?.name ?: "Custom track"
                    com.armsx2.LibraryMusic.setCustomTrack(appContext, uri, name)
                }
            }
            val custom = com.armsx2.LibraryMusic.customName.value
            Text(
                if (custom != null) str("app.libraryMusic.current").format(custom)
                else str("app.libraryMusic.default"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp, top = 2.dp),
            )
            Row(
                Modifier.fillMaxWidth().padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val pick = { musicPicker.launch(arrayOf("audio/*")) }
                OutlinedButton(
                    onClick = pick,
                    modifier = Modifier.controllerFocusable("app.libraryMusic.choose", onConfirm = pick),
                ) { Text(str("app.libraryMusic.choose")) }
                if (custom != null) {
                    val reset = { com.armsx2.LibraryMusic.clearCustomTrack(appContext) }
                    OutlinedButton(
                        onClick = reset,
                        modifier = Modifier.controllerFocusable("app.libraryMusic.reset", onConfirm = reset),
                    ) { Text(str("app.libraryMusic.reset")) }
                }
            }
        }

        // In-game pause music. Separate track and separate toggle from the library's: the pause
        // menu was silent, and people sit in it browsing settings mid-game. Off by default —
        // audio starting when you open a menu is startling if you didn't ask for it.
        ToggleRow(
            label = str("app.pauseMusic"),
            value = com.armsx2.PauseMusic.enabled.value,
            description = str("app.pauseMusic.desc"),
            onChange = { com.armsx2.PauseMusic.set(appContext, it) },
        )
        if (com.armsx2.PauseMusic.enabled.value) {
            IntSliderRow(
                label = str("app.pauseMusic.volume"),
                value = com.armsx2.PauseMusic.volumePercent.value,
                min = 0,
                max = 100,
                valueFormatter = { "$it%" },
                onChange = { com.armsx2.PauseMusic.setVolume(it) },
            )
            val pausePicker = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { uri ->
                if (uri != null) {
                    val name = androidx.documentfile.provider.DocumentFile
                        .fromSingleUri(appContext, uri)?.name ?: "Custom track"
                    com.armsx2.PauseMusic.setCustomTrack(appContext, uri, name)
                }
            }
            val pauseCustom = com.armsx2.PauseMusic.customName.value
            Text(
                if (pauseCustom != null) str("app.pauseMusic.current").format(pauseCustom)
                else str("app.pauseMusic.default"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp, top = 2.dp),
            )
            Row(
                Modifier.fillMaxWidth().padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val pickPause = { pausePicker.launch(arrayOf("audio/*")) }
                OutlinedButton(
                    onClick = pickPause,
                    modifier = Modifier.controllerFocusable("app.pauseMusic.choose", onConfirm = pickPause),
                ) { Text(str("app.pauseMusic.choose")) }
                if (pauseCustom != null) {
                    val resetPause = { com.armsx2.PauseMusic.clearCustomTrack(appContext) }
                    OutlinedButton(
                        onClick = resetPause,
                        modifier = Modifier.controllerFocusable("app.pauseMusic.reset", onConfirm = resetPause),
                    ) { Text(str("app.pauseMusic.reset")) }
                }
            }
        }

        // Menu sound effects. User-provided like the custom track above — the app ships no sounds;
        // the user imports a folder of named clips (select/back/menu/toggle_on/toggle_off/reset/
        // slider). Opt-in (off by default) since there are no bundled defaults to fall back on.
        ToggleRow(
            label = str("app.menuSfx"),
            value = com.armsx2.MenuSfx.enabled.value,
            description = str("app.menuSfx.desc"),
            onChange = { com.armsx2.MenuSfx.set(appContext, it) },
        )
        if (com.armsx2.MenuSfx.enabled.value) {
            IntSliderRow(
                label = str("app.menuSfx.volume"),
                value = com.armsx2.MenuSfx.volumePercent.value,
                min = 0,
                max = 100,
                valueFormatter = { "$it%" },
                onChange = { com.armsx2.MenuSfx.setVolume(it) },
            )
            val sfxPicker = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocumentTree()
            ) { uri ->
                if (uri != null) {
                    val n = com.armsx2.MenuSfx.importFromTree(appContext, uri)
                    Toast.makeText(
                        appContext,
                        if (n > 0) I18n.get("app.menuSfx.imported").format(n)
                        else I18n.get("app.menuSfx.importNone"),
                        Toast.LENGTH_LONG,
                    ).show()
                }
            }
            val pack = com.armsx2.MenuSfx.packName.value
            Text(
                if (pack != null) str("app.menuSfx.current").format(pack) else str("app.menuSfx.none"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp, top = 2.dp),
            )
            Text(
                str("app.menuSfx.hint"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp, top = 2.dp),
            )
            Row(
                Modifier.fillMaxWidth().padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val pick = { sfxPicker.launch(null) }
                OutlinedButton(
                    onClick = pick,
                    modifier = Modifier.controllerFocusable("app.menuSfx.choose", onConfirm = pick),
                ) { Text(str("app.menuSfx.choose")) }
                if (pack != null) {
                    val clear = { com.armsx2.MenuSfx.clear(appContext) }
                    OutlinedButton(
                        onClick = clear,
                        modifier = Modifier.controllerFocusable("app.menuSfx.reset", onConfirm = clear),
                    ) { Text(str("app.menuSfx.reset")) }
                }
            }
        }

        SegmentedRow(
            label = str("app.toolbarPosition"),
            options = listOf(str("app.toolbarPosition.top"), str("app.toolbarPosition.bottom")),
            selectedIndex = if (ToolbarPositionPreferences.atBottom.value) 1 else 0,
            onChange = { ToolbarPositionPreferences.set(it == 1) },
        )

        // Launcher/library rotation — independent of the per-game renderer rotation (Renderer tab).
        SegmentedRow(
            label = str("app.launcherRotation"),
            options = listOf(
                str("app.launcherRotation.device"),
                str("app.launcherRotation.landscape"),
                str("app.launcherRotation.portrait"),
                str("app.launcherRotation.auto"),
            ),
            selectedIndex = LauncherOrientationPreferences.mode.value.coerceIn(0, 3),
            description = str("app.launcherRotation.desc"),
            onChange = {
                LauncherOrientationPreferences.set(it)
                MainActivityRuntime.instance?.applyEmulationOrientation()  // live-apply (no game running)
            },
        )

        ToggleRow(
            label = str("app.library.search"),
            value = LibraryChromePreferences.showSearch.value,
            description = str("app.library.search.desc"),
            onChange = LibraryChromePreferences::setShowSearch,
        )

        // Text entry: our own on-screen keyboard (default, gamepad-navigable) vs the Android IME.
        // Seeded via refreshUseSystemIme() because this row can compose before the keyboard has
        // ever been opened, which is the only other place the preference gets read.
        run {
            remember { com.armsx2.ui.home.LibraryKeyboard.refreshUseSystemIme() }
            ToggleRow(
                label = str("app.keyboard.systemIme"),
                value = com.armsx2.ui.home.LibraryKeyboard.useSystemIme.value,
                description = str("app.keyboard.systemIme.desc"),
                onChange = com.armsx2.ui.home.LibraryKeyboard::setUseSystemIme,
            )
        }

        ToggleRow(
            label = str("app.library.recents"),
            value = LibraryChromePreferences.showRecents.value,
            description = str("app.library.recents.desc"),
            onChange = LibraryChromePreferences::setShowRecents,
        )

        // Moved off the library overflow menu, where it was the odd one out: every other
        // library-appearance preference already lives here beside cover size and opacity.
        ToggleRow(
            label = str("games.overflow.gridNames"),
            value = com.armsx2.GridLabels.show.value,
            description = str("app.library.gridNames.desc"),
            onChange = { com.armsx2.GridLabels.set(it) },
        )

        IntSliderRow(
            label = str("app.library.coverSize"),
            value = (com.armsx2.ui.UiScale.coverScale.value * 100f).toInt().coerceIn(75, 250),
            min = 75,
            max = 250,
            valueFormatter = { "$it%" },
            onChange = { com.armsx2.ui.UiScale.setCoverScale(it / 100f) },
        )

        IntSliderRow(
            label = str("app.library.opacity"),
            value = LibraryChromePreferences.libraryOpacity.value,
            min = 20,
            max = 100,
            valueFormatter = { "$it%" },
            onChange = LibraryChromePreferences::setLibraryOpacity,
        )

        ClearCacheRow()
    }
}

/** Clear cached, regenerable data: compiled shader/pipeline caches (Vulkan + GL) and
 *  the cover-art image cache. All of it rebuilds automatically, so this only frees space
 *  and forces a clean rebuild — handy after a driver change or if a cache looks corrupt. */
@Composable
private fun ClearCacheRow() {
    val context = LocalContext.current
    var status by remember { mutableStateOf("") }
    Surface(
        onClick = { status = clearAppCaches(context) },
        modifier = Modifier.fillMaxWidth()
            .controllerFocusable("app.clearCache", RoundedCornerShape(20.dp), onConfirm = { status = clearAppCaches(context) }),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Surface(
                modifier = Modifier.size(46.dp),
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.primaryContainer,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
                    Text("🧹", fontSize = 21.sp)
                }
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(str("app.clearCache"), style = MaterialTheme.typography.titleMedium)
                Text(
                    status.ifEmpty { str("app.clearCache.desc") },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** Export / import everything a reinstall would destroy: save states, memory cards, artwork,
 *  per-game settings, controller profiles, patches and every preference. ROMs and BIOS are left
 *  out — those live outside the app and survive on their own. See [com.armsx2.BackupManager]. */
@Composable
private fun BackupRestoreRows() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }

    // Both directions run on IO: a full data root is tens to hundreds of MB and would jank (or ANR)
    // on the main thread. `busy` blocks a second tap while one is in flight.
    val exporter = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip")
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        busy = true
        status = I18n.get("app.backup.working")
        scope.launch(Dispatchers.IO) {
            val r = runCatching {
                context.contentResolver.openOutputStream(uri)?.use {
                    com.armsx2.BackupManager.export(context, it)
                } ?: com.armsx2.BackupManager.BackupResult(false, "could not open destination")
            }.getOrElse { com.armsx2.BackupManager.BackupResult(false, it.message ?: "failed") }
            withContext(Dispatchers.Main) {
                busy = false
                status = if (r.ok) I18n.get("app.backup.exported").replace("%s", r.detail)
                         else I18n.get("app.backup.failed").replace("%s", r.detail)
                Toast.makeText(context, status, Toast.LENGTH_LONG).show()
            }
        }
    }
    val importer = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        busy = true
        status = I18n.get("app.backup.working")
        scope.launch(Dispatchers.IO) {
            val r = runCatching {
                context.contentResolver.openInputStream(uri)?.use {
                    com.armsx2.BackupManager.restore(context, it)
                } ?: com.armsx2.BackupManager.BackupResult(false, "could not open file")
            }.getOrElse { com.armsx2.BackupManager.BackupResult(false, it.message ?: "failed") }
            withContext(Dispatchers.Main) {
                busy = false
                status = if (r.ok) I18n.get("app.backup.imported").replace("%s", r.detail)
                         else I18n.get("app.backup.failed").replace("%s", r.detail)
                Toast.makeText(context, status, Toast.LENGTH_LONG).show()
                // Preferences are read once at startup, so this process would keep serving the old
                // values and then overwrite the restored XML on its next write. Restart to adopt.
                if (r.ok) MainActivityRuntime.restartApp(context)
            }
        }
    }

    val doExport = { if (!busy) exporter.launch(com.armsx2.BackupManager.suggestedName(context)) }
    val doImport = {
        if (!busy) importer.launch(arrayOf("application/zip", "application/octet-stream"))
    }

    BackupActionRow("💾", "app.backup.export", "app.backup.export.desc", status, busy, doExport)
    BackupActionRow("📥", "app.backup.import", "app.backup.import.desc", "", busy, doImport)

    // Factory reset. Sits with Backup/Restore because Export is the thing to do first — the
    // prompt says so. Routed through GlobalConfirm rather than a local overlay: this row is
    // inside a scrolling tab, so a scrim drawn here would clip to the row's bounds.
    val doReset = {
        if (!busy) {
            com.armsx2.ui.common.GlobalConfirm.ask(
                title = I18n.get("app.reset.title"),
                message = I18n.get("app.reset.message"),
                confirmLabel = I18n.get("app.reset.confirm"),
                destructive = true,
            ) { MainActivityRuntime.resetAppToDefaults(context) }
        }
    }
    BackupActionRow("♻️", "app.reset", "app.reset.desc", "", busy, doReset)
}

@Composable
private fun BackupActionRow(
    emoji: String,
    labelKey: String,
    descKey: String,
    status: String,
    busy: Boolean,
    onClick: () -> Unit,
) {
    Surface(
        onClick = onClick,
        enabled = !busy,
        modifier = Modifier.fillMaxWidth()
            .controllerFocusable(labelKey, RoundedCornerShape(20.dp), onConfirm = onClick),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Surface(
                modifier = Modifier.size(46.dp),
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.primaryContainer,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
                    Text(emoji, fontSize = 21.sp)
                }
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(str(labelKey), style = MaterialTheme.typography.titleMedium)
                Text(
                    status.ifEmpty { str(descKey) },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** Delete shader/pipeline caches (assetCopyRoot/cache) + the OS cache dir (Coil image
 *  cache, temp files). Returns a human-readable summary for the row + a toast. */
private fun clearAppCaches(context: android.content.Context): String {
    var removed = 0
    var bytes = 0L
    fun wipe(dir: File?) {
        val entries = dir?.listFiles() ?: return
        for (f in entries) {
            val size = if (f.isFile) f.length() else 0L
            val ok = if (f.isDirectory) f.deleteRecursively() else runCatching { f.delete() }.getOrDefault(false)
            if (ok) { removed++; bytes += size }
        }
    }
    // Compiled shader / pipeline caches live under the app-private asset-copy root.
    wipe(File(MainActivityRuntime.assetCopyRoot(context), "cache"))
    // Coil cover-art cache + any transient files in the OS-managed cache dir.
    wipe(context.cacheDir)
    val summary = if (removed > 0) {
        val mb = bytes / (1024.0 * 1024.0)
        if (mb >= 0.1) I18n.get("app.clearCache.done").replace("%s", String.format("%.1f MB", mb))
        else I18n.get("app.clearCache.doneSmall")
    } else {
        I18n.get("app.clearCache.empty")
    }
    Toast.makeText(context, summary, Toast.LENGTH_SHORT).show()
    return summary
}
