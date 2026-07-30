package com.armsx2.ui.common

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.delay

/**
 * Clock + battery readout for the library toolbar.
 *
 * All values come from the platform: [android.text.format.DateFormat] for the 12/24h preference,
 * and a sticky ACTION_BATTERY_CHANGED broadcast plus BatteryManager for charge and time-remaining.
 * Nothing is estimated here — if the OS won't say how long is left, the field is simply omitted
 * rather than guessed at.
 */

/** Battery level 0..100, charging flag, and OS-reported time remaining (or null). */
private data class BatteryInfo(
    val percent: Int,
    val charging: Boolean,
    val minutesRemaining: Long?,
)

@Composable
private fun batteryInfo(): BatteryInfo {
    val context = LocalContext.current
    var info by remember { mutableStateOf(BatteryInfo(-1, false, null)) }

    DisposableEffect(Unit) {
        fun read(intent: Intent?): BatteryInfo {
            val bm = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
            // BatteryManager first: it's the authoritative capacity. The broadcast's level/scale is
            // the fallback for devices where the property isn't populated.
            val pct = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
                ?.takeIf { it in 0..100 }
                ?: intent?.let {
                    val lvl = it.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                    val scale = it.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                    if (lvl >= 0 && scale > 0) lvl * 100 / scale else -1
                } ?: -1
            val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
            val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
            // computeChargeTimeRemaining() is charging-only and API 28+; discharge time has no
            // public API at all, so "time remaining" is only ever shown while charging. Reporting a
            // home-grown discharge estimate would be inventing data.
            val remaining = runCatching {
                if (charging && android.os.Build.VERSION.SDK_INT >= 28)
                    bm?.computeChargeTimeRemaining()?.takeIf { it > 0 }?.let { it / 60000L }
                else null
            }.getOrNull()
            return BatteryInfo(pct, charging, remaining)
        }

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                info = read(intent)
            }
        }
        // registerReceiver with a sticky action returns the last broadcast immediately, so this
        // both seeds the value and subscribes to changes in one call.
        val sticky = runCatching {
            context.registerReceiver(receiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull()
        info = read(sticky)
        onDispose { runCatching { context.unregisterReceiver(receiver) } }
    }
    return info
}

/**
 * Battery pill: a glyph that empties as the charge drops, plus the percentage.
 *
 * Green above 50%, amber above 20%, red below — and always green while charging, since a charging
 * phone at 10% is not a warning state.
 */
@Composable
fun BatteryIndicator(modifier: Modifier = Modifier) {
    val info = batteryInfo()
    if (info.percent < 0) return

    val fillColor = when {
        info.charging -> Color(0xFF4CAF50)
        info.percent > 50 -> Color(0xFF4CAF50)
        info.percent > 20 -> Color(0xFFFFC107)
        else -> Color(0xFFF44336)
    }
    val outline = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.65f)

    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Canvas(Modifier.size(width = 22.dp, height = 12.dp)) {
            val capW = size.width * 0.08f
            val bodyW = size.width - capW - 1.5f
            val stroke = 1.4f
            // Body outline
            drawRoundRect(
                color = outline,
                topLeft = Offset(0f, 0f),
                size = Size(bodyW, size.height),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(2.5f, 2.5f),
                style = Stroke(width = stroke),
            )
            // Positive terminal
            drawRoundRect(
                color = outline,
                topLeft = Offset(bodyW + 1.5f, size.height * 0.28f),
                size = Size(capW, size.height * 0.44f),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(1f, 1f),
            )
            // Fill, inset so it sits inside the outline
            val inset = stroke + 1f
            val maxFill = bodyW - inset * 2f
            val frac = (info.percent / 100f).coerceIn(0f, 1f)
            if (maxFill > 0f && frac > 0f) {
                drawRoundRect(
                    color = fillColor,
                    topLeft = Offset(inset, inset),
                    size = Size(maxFill * frac, size.height - inset * 2f),
                    cornerRadius = androidx.compose.ui.geometry.CornerRadius(1.5f, 1.5f),
                )
            }

            // Charging bolt, drawn over the fill and centred on the body. Outlined in the
            // background colour first so it stays legible whether it lands on the filled or the
            // empty part of the glyph — at 22x12dp a plain white bolt disappears against green.
            if (info.charging) {
                val cx = bodyW / 2f
                val cy = size.height / 2f
                val h = size.height * 0.72f
                val w = h * 0.42f
                val bolt = androidx.compose.ui.graphics.Path().apply {
                    moveTo(cx + w * 0.28f, cy - h / 2f)
                    lineTo(cx - w * 0.52f, cy + h * 0.10f)
                    lineTo(cx + w * 0.02f, cy + h * 0.10f)
                    lineTo(cx - w * 0.28f, cy + h / 2f)
                    lineTo(cx + w * 0.52f, cy - h * 0.12f)
                    lineTo(cx - w * 0.02f, cy - h * 0.12f)
                    close()
                }
                drawPath(bolt, color = Color(0xFF16240F), style = Stroke(width = 1.6f))
                drawPath(bolt, color = Color(0xFFFFF176))
            }
        }
        Spacer(Modifier.width(5.dp))
        Text(
            "${info.percent}%",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.9f),
        )
        // Only while charging, and only when the OS supplies it (see read()). No ⚡ prefix — the
        // bolt drawn inside the glyph already says "charging", and two of them reads as noise.
        info.minutesRemaining?.let { mins ->
            Spacer(Modifier.width(4.dp))
            Text(
                if (mins >= 60) "%dh%02dm".format(mins / 60, mins % 60) else "${mins}m",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.65f),
            )
        }
    }
}

/** Wall clock, following the device's 12/24-hour setting. Re-reads every 10s. */
@Composable
fun ClockIndicator(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val pattern = remember {
        if (android.text.format.DateFormat.is24HourFormat(context)) "HH:mm" else "h:mm a"
    }
    val fmt = remember(pattern) { SimpleDateFormat(pattern, Locale.getDefault()) }
    var now by remember { mutableStateOf(fmt.format(Date())) }
    LaunchedEffect(fmt) {
        while (true) {
            now = fmt.format(Date())
            // 10s, not 1s: the display is minute-resolution, so this is the coarsest tick that
            // still turns over within a few seconds of the real minute boundary.
            delay(10_000)
        }
    }
    Text(
        now,
        modifier = modifier,
        style = MaterialTheme.typography.labelMedium,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.9f),
    )
}

/**
 * Clock + battery for the library toolbar's action row.
 *
 * Landscape stacks them (clock above battery) — the wide bar has the height for it. Portrait lays
 * them side by side in a single compact row instead: the narrow bar crammed the two-line stack
 * between the title and the buttons (#Isshin, S24 Ultra portrait), and one short line reads cleaner
 * and leaves the buttons room.
 */
@Composable
fun LibraryStatusCluster(modifier: Modifier = Modifier, compact: Boolean = false) {
    if (compact) {
        Row(
            modifier,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ClockIndicator()
            BatteryIndicator()
        }
    } else {
        Column(
            modifier,
            horizontalAlignment = Alignment.End,
            verticalArrangement = Arrangement.Center,
        ) {
            ClockIndicator()
            Spacer(Modifier.height(1.dp))
            BatteryIndicator()
        }
    }
}
