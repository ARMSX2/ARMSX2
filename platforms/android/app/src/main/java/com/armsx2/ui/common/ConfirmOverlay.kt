package com.armsx2.ui.common

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.settings.controllerFocusable

/**
 * A yes/no confirmation that a controller can actually drive.
 *
 * Thin content on top of [PadModal] — that file carries the reasoning, including why this is
 * never a Compose `AlertDialog`. Everything this one adds is the card: title, message, and a
 * Cancel/Confirm pair with Cancel pre-focused, so the safe option is the one already under the
 * cursor on a destructive prompt.
 *
 * @param destructive tints Confirm as an error action — for irreversible things like a reset.
 * @param idPrefix distinguishes concurrent prompts' nav ids and layer.
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
    idPrefix: String = "confirm",
) {
    val layer = "confirm-overlay:$idPrefix"
    PadModal(
        key = layer,
        onDismiss = onDismiss,
        initialFocusId = "$layer.cancel",
    ) {
        Surface(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 420.dp),
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
                        onClick = onDismiss,
                        container = MaterialTheme.colorScheme.surfaceVariant,
                        content = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(10.dp))
                    ConfirmButton(
                        label = confirmLabel,
                        id = "$layer.confirm",
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
 * App-wide confirmation host, for prompts raised from code that has no composition of its own to
 * put a [ConfirmOverlay] in — a click handler deep in a view model, say.
 *
 * Call [ask] from anywhere; [Host] (composed once in WindowImpl) authors the prompt, and the
 * shared modal host renders it.
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
