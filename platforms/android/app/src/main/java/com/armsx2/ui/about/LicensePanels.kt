package com.armsx2.ui.about

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.ui.common.PadModal
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** ARMSX2's copyright line. Names and years, so the same in every language. */
internal const val ArmsCopyright = "Copyright © 2002-2026 PCSX2 Dev Team and ARMSX2 contributors"

/** The GNU GPL, from the About screen's License card. */
@Composable
internal fun GplPanel(onClose: () -> Unit) {
    val context = LocalContext.current
    val text by produceState<String?>(null) {
        value = withContext(Dispatchers.IO) { runCatching { LicenseDocs.gpl(context) }.getOrNull() }
    }
    LicenseTextPanel(
        key = "about.license",
        title = str("about.license.name"),
        subtitle = ArmsCopyright,
        text = text,
        monospace = false,
        onClose = onClose,
    )
}

/**
 * Every open-source component in the app, from the About screen's Open-source licenses card:
 * the Android app's own, then PCSX2's. A component opens its license over the list, and closing
 * that comes back to the same row.
 */
@Composable
internal fun OpenSourceLicensesPanel(onClose: () -> Unit) {
    val context = LocalContext.current
    val components by produceState<Pair<List<LicenseEntry>, List<LicenseEntry>>?>(null) {
        value = withContext(Dispatchers.IO) { runCatching { LicenseDocs.components(context) }.getOrNull() }
    }
    var open by remember { mutableStateOf<LicenseEntry?>(null) }
    val scroll = rememberScrollState()
    LicenseModal(
        key = "about.oss",
        title = str("about.oss.title"),
        subtitle = null,
        scroll = scroll,
        onClose = onClose,
    ) {
        val (android, pcsx2) = components ?: run {
            Text("…", style = MaterialTheme.typography.bodyMedium)
            return@LicenseModal
        }
        SectionTitle(str("about.oss.android"))
        android.forEach { entry -> ComponentRow("android", entry) { open = entry } }
        Spacer(Modifier.size(6.dp))
        SectionTitle(str("about.oss.pcsx2"))
        pcsx2.forEach { entry -> ComponentRow("pcsx2", entry) { open = entry } }
    }
    open?.let { entry ->
        LicenseTextPanel(
            key = "about.oss.text",
            title = entry.name,
            subtitle = entry.url,
            text = entry.text,
            monospace = entry.preformatted,
            onClose = { open = null },
        )
    }
}

@Composable
private fun LicenseTextPanel(
    key: String,
    title: String,
    subtitle: String?,
    text: String?,
    monospace: Boolean,
    onClose: () -> Unit,
) {
    val scroll = rememberScrollState()
    LicenseModal(key = key, title = title, subtitle = subtitle, scroll = scroll, onClose = onClose) {
        Text(
            text ?: "…",
            style = if (monospace) {
                MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 11.sp, lineHeight = 15.sp)
            } else {
                MaterialTheme.typography.bodyMedium
            },
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

/**
 * The panel both views share: a title and Close above a body that scrolls. With a controller,
 * Up and Down scroll the body once there is no row to move to, and B closes.
 */
@Composable
private fun LicenseModal(
    key: String,
    title: String,
    subtitle: String?,
    scroll: ScrollState,
    onClose: () -> Unit,
    body: @Composable () -> Unit,
) {
    val closeId = "$key.close"
    PadModal(key = key, onDismiss = onClose, initialFocusId = closeId, scrollState = scroll) {
        Surface(
            modifier = Modifier.padding(16.dp).widthIn(max = 760.dp).fillMaxWidth(),
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier
                    .padding(horizontal = 20.dp, vertical = 16.dp)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.86f).dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                        if (subtitle != null) {
                            Text(
                                subtitle,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.primary,
                            )
                        }
                    }
                    TextButton(
                        onClick = onClose,
                        modifier = Modifier.controllerFocusable(closeId, onConfirm = onClose),
                    ) { Text(str("action.close")) }
                }
                Spacer(Modifier.size(10.dp))
                Column(
                    Modifier.fillMaxWidth().weight(1f, fill = false).verticalScroll(scroll),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    body()
                }
            }
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.Bold,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 4.dp, bottom = 2.dp),
    )
}

@Composable
private fun ComponentRow(group: String, entry: LicenseEntry, onOpen: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Surface(
        onClick = onOpen,
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable("about.oss.$group.${entry.id}", shape, onConfirm = onOpen),
        shape = shape,
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.35f)),
    ) {
        Row(Modifier.padding(horizontal = 14.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(entry.name, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.SemiBold)
                entry.url?.let {
                    Text(
                        it.removePrefix("https://").removePrefix("http://"),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
            }
            Text("›", color = MaterialTheme.colorScheme.primary, fontSize = 22.sp)
        }
    }
}
