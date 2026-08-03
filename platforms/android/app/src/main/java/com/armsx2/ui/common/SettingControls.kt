package com.armsx2.ui.common

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.ui.settings.controllerFocusable

@Composable
fun SettingSwitchRow(
    title: String,
    description: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    // Match ToggleRow: menu SFX on every flip, and a "toggle:$title" nav id so the controller can
    // reach the row (the updater switches were touch-only before) and settings-search can jump to it.
    val emit: (Boolean) -> Unit = {
        com.armsx2.MenuSfx.play(if (it) com.armsx2.MenuSfx.Event.TOGGLE_ON else com.armsx2.MenuSfx.Event.TOGGLE_OFF)
        onCheckedChange(it)
    }
    Surface(
        onClick = { emit(!checked) },
        modifier = modifier.fillMaxWidth()
            .controllerFocusable(
                controllerId = "toggle:$title",
                shape = RoundedCornerShape(22.dp),
                onConfirm = { emit(!checked) },
                onLeft = { if (checked) emit(false) },
                onRight = { if (!checked) emit(true) },
            ),
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
    ) {
        Row(
            Modifier.defaultMinSize(minHeight = 78.dp).padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                    // The shared one. This file used to carry its own copy, which meant the
                    // scroll-and-cap fix for long descriptions only ever landed on half the
                    // app's settings rows — and silently, since both looked identical closed.
                    com.armsx2.ui.settings.InfoHint(title, description)
                }
                Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(16.dp))
            Switch(checked = checked, onCheckedChange = emit)
        }
    }
}
