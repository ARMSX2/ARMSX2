package com.armsx2.ui.settings

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.AutoTuneStore
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.common.ConfirmOverlay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * What automatic performance tuning changed for THIS game, and a way to put it back.
 *
 * The core records each setting it changed in the game's settings file, with what the setting was before;
 * the list is read from there. Reset restores those values (the core edits the file, [AutoTuneStore.reset]
 * does the same to the app's own copy of the game's settings) and forgets what it learned about the game.
 * A setting the player changed since is left as it is.
 *
 * Only in per-game scope, like [GameDbSection]: with no game there is nothing recorded.
 */
@Composable
internal fun AutoTuneSection(state: MutableState<Settings>) {
    val settingsKey = InGameOverlay.currentSerial.value?.takeIf { it.isNotBlank() } ?: return
    if (InGameOverlay.settingsScope.value != SettingsScope.Game) return

    // Bumped after a reset: the record lives in a file, not in `state`, so it has to be read again.
    var version by remember(settingsKey) { mutableIntStateOf(0) }
    val lines by produceState(initialValue = emptyList<String>(), settingsKey, version) {
        value = withContext(Dispatchers.IO) { AutoTuneStore.describe(settingsKey) }
    }
    var confirming by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val context = LocalContext.current

    Column(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp)) {
        Text(
            str("autotune.changes.title"),
            color = MaterialTheme.colorScheme.primary,
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
        )
        Spacer(Modifier.height(6.dp))
        if (lines.isEmpty()) {
            Text(str("autotune.changes.none"), color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 14.sp, lineHeight = 20.sp)
        } else {
            lines.forEach { line ->
                Text(line, color = MaterialTheme.colorScheme.onSurface, fontSize = 15.sp, lineHeight = 22.sp)
            }
        }
    }
    if (lines.isNotEmpty()) {
        val ask = { confirming = true }
        Surface(
            onClick = ask,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 5.dp)
                .controllerFocusable(controllerId = "autotune:reset", onConfirm = ask),
            shape = RoundedCornerShape(22.dp),
            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth().defaultMinSize(minHeight = 64.dp).padding(horizontal = 16.dp, vertical = 12.dp),
            ) {
                Text(
                    str("autotune.reset.label"),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 18.sp,
                    lineHeight = 23.sp,
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }
    }
    if (confirming) {
        // ConfirmOverlay, not AlertDialog: a Compose dialog is its own window and swallows controller keys.
        ConfirmOverlay(
            title = str("autotune.reset.label"),
            message = str("autotune.reset.confirm"),
            confirmLabel = str("action.reset"),
            destructive = true,
            idPrefix = "autotune-reset",
            onConfirm = {
                confirming = false
                scope.launch {
                    val outcome = AutoTuneStore.reset(settingsKey)
                    // What the screens show is the stored settings again, without the values auto-tune put there.
                    val shown = withContext(Dispatchers.IO) { ConfigStore.resolveForDisplay(settingsKey) }
                    state.value = shown
                    InGameOverlay.settingsState.value = shown
                    version++
                    val text = when {
                        outcome == null || !outcome.hadRecord -> I18n.get("autotune.reset.nothing")
                        outcome.leftAlone > 0 -> I18n.get("autotune.reset.doneLeftAlone").format(outcome.restored, outcome.leftAlone)
                        else -> I18n.get("autotune.reset.done").format(outcome.restored)
                    }
                    android.widget.Toast.makeText(context, text, android.widget.Toast.LENGTH_LONG).show()
                }
            },
            onDismiss = { confirming = false },
        )
    }
}
