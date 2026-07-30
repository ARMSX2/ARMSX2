package com.armsx2.ui.common

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable

/**
 * A yes/no confirmation that a controller can actually drive.
 *
 * NOT a Compose `Dialog`/`AlertDialog` on purpose. A Compose dialog is its own Android window and
 * grabs input focus, so controller KeyEvents are consumed there and never reach the Activity's
 * dispatchKeyEvent — which is where this app's D-pad navigation lives. A dialog therefore renders
 * a confirmation unreachable by pad (the same trap that made the old button-bind popup dead).
 * This draws inline in the caller's own Compose tree instead.
 *
 * While shown it claims [SettingsControllerNav.activeLayer], so only its own two buttons take
 * D-pad input and the selection can't escape into the screen behind the scrim.
 *
 * @param destructive tints Confirm as an error action — for irreversible things like a reset.
 */
@Composable
fun ConfirmOverlay(
    title: String,
    message: String,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
    confirmLabel: String = str("action.ok"),
    dismissLabel: String = str("action.cancel"),
    destructive: Boolean = false,
    /** Distinguishes concurrent overlays' nav ids. Only matters if two can ever be composed at once. */
    idPrefix: String = "confirm",
) {
    val layer = "confirm-overlay:$idPrefix"
    DisposableEffect(layer) {
        val previous = SettingsControllerNav.activeLayer.value
        SettingsControllerNav.activeLayer.value = layer
        // Drop the outside selection: that row is un-navigable now and would look stuck.
        SettingsControllerNav.clearSelection()
        onDispose {
            SettingsControllerNav.activeLayer.value = previous
            SettingsControllerNav.clearSelection()
        }
    }
    // Then take a selection of our own, once our buttons have registered (they register during
    // this composition, so it cannot happen in the DisposableEffect above). Required, not
    // cosmetic: the home screen routes the D-pad to the cover grid unless this registry reports
    // a selection, so without this the first press would move the library behind the scrim.
    // Cancel, so the safe option is pre-focused on a destructive prompt.
    LaunchedEffect(layer) {
        var attempts = 0
        while (!SettingsControllerNav.selectById("$layer.cancel") && attempts < 10) {
            attempts++
            withFrameNanos {}
        }
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.62f))
            // Swallow taps on the scrim so they can't reach the screen behind, and treat a tap
            // outside the card as Cancel. indication = null: a ripple across the whole scrim
            // looks like a bug.
            .clickable(
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
                onClick = onDismiss,
            ),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 420.dp)
                // Absorb taps on the card itself, or the scrim's dismiss fires through it.
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = {},
                ),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(20.dp)) {
                Text(
                    title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    message,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(18.dp))
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                ) {
                    // Cancel first so it is the leftmost / first-focused item — the safe default
                    // for a destructive prompt.
                    ConfirmButton(
                        label = dismissLabel,
                        id = "$layer.cancel",
                        layer = layer,
                        onClick = onDismiss,
                        container = MaterialTheme.colorScheme.surfaceVariant,
                        content = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(10.dp))
                    ConfirmButton(
                        label = confirmLabel,
                        id = "$layer.confirm",
                        layer = layer,
                        onClick = onConfirm,
                        container = if (destructive) MaterialTheme.colorScheme.errorContainer
                        else MaterialTheme.colorScheme.primaryContainer,
                        content = if (destructive) MaterialTheme.colorScheme.onErrorContainer
                        else MaterialTheme.colorScheme.onPrimaryContainer,
                    )
                }
            }
        }
    }
}

/**
 * App-wide confirmation host, for prompts raised from somewhere that cannot draw a full-screen
 * overlay itself — a row inside a scrolling settings tab would clip the scrim to its own bounds
 * and scroll away with the list.
 *
 * Call [ask] from anywhere; [Host] (mounted once in WindowImpl, above every surface) renders it.
 */
object GlobalConfirm {
    data class Request(
        val title: String,
        val message: String,
        val confirmLabel: String?,
        val destructive: Boolean,
        val onConfirm: () -> Unit,
    )

    val pending = mutableStateOf<Request?>(null)

    fun ask(
        title: String,
        message: String,
        confirmLabel: String? = null,
        destructive: Boolean = false,
        onConfirm: () -> Unit,
    ) {
        pending.value = Request(title, message, confirmLabel, destructive, onConfirm)
    }

    fun dismiss() {
        pending.value = null
    }

    @Composable
    fun Host() {
        val request = pending.value ?: return
        ConfirmOverlay(
            title = request.title,
            message = request.message,
            confirmLabel = request.confirmLabel ?: str("action.ok"),
            destructive = request.destructive,
            idPrefix = "global",
            onConfirm = {
                // Clear FIRST: the action may restart the process, and a surviving request would
                // otherwise re-prompt on the next launch.
                pending.value = null
                request.onConfirm()
            },
            onDismiss = { pending.value = null },
        )
    }
}

@Composable
private fun ConfirmButton(
    label: String,
    id: String,
    layer: String,
    onClick: () -> Unit,
    container: Color,
    content: Color,
) {
    Surface(
        onClick = onClick,
        modifier = Modifier.controllerFocusable(
            controllerId = id,
            shape = RoundedCornerShape(14.dp),
            onConfirm = onClick,
            layer = layer,
        ),
        shape = RoundedCornerShape(14.dp),
        color = container,
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 10.dp),
            style = MaterialTheme.typography.labelLarge,
            color = content,
        )
    }
}
