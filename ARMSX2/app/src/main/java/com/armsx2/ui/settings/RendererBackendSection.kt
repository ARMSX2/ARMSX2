package com.armsx2.ui.settings

import android.content.Context
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.CustomDriver
import com.armsx2.Main
import com.armsx2.ui.Colors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Renderer-backend chooser for the in-game settings Renderer tab. Ports the
 * renderer + custom-driver picker that used to live on the first-run setup
 * page (SetupImpl.SetupRendererContent) into the settings overlay, since that
 * setup screen was removed.
 *
 * - "Graphics API" selects the host display backend: Auto (emucore's
 *   GSUtil::GetPreferredRenderer), OpenGL, or Vulkan. Written to
 *   [Main.renderer] + prefs, applied by [Main.applyRendererPrefs] on the next
 *   VM (re)start.
 * - When Vulkan is selected, a GPU Driver list lets the user replace the
 *   system Vulkan ICD with an AdrenoToolsDrivers pack (Mesa Turnip & friends),
 *   exactly like the old setup page: pick Default, an installed driver, import
 *   a local .zip, or browse the K11MCH1/AdrenoToolsDrivers GitHub releases.
 *
 * Renderer / driver changes only take effect on a renderer (re)open, so the
 * section ends with an "Apply & Restart" action that relaunches the current
 * game (mirrors the toolbar RestartButton).
 */
@Composable
fun RendererBackendSection() {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()

    val rendererIds = listOf("auto", "opengl", "vulkan", "software")
    val rendererLabels = listOf("Auto", "OpenGL", "Vulkan", "Software")
    val currentRenderer = Main.renderer.value
    val selIdx = rendererIds.indexOf(currentRenderer).coerceAtLeast(0)

    SegmentedRow(
        label = "Graphics API",
        options = rendererLabels,
        selectedIndex = selIdx,
        onChange = { idx ->
            val pick = rendererIds[idx]
            if (pick != Main.renderer.value) {
                Main.renderer.value = pick
                Main.prefs.edit().putString("renderer", pick).apply()
                // A custom driver is only meaningful for Vulkan — drop the
                // pick on the OGL/Auto path so it doesn't linger as a stale
                // selection if the user toggles back.
                if (pick != "vulkan") {
                    Main.customDriverId.value = null
                    Main.prefs.edit().putString("customDriverId", "").apply()
                }
            }
        },
    )

    if (Main.renderer.value == "vulkan") {
        SettingsDivider()
        GpuDriverSection(context, scope)
    }

    SettingsDivider()
    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text(
                "Graphics API / driver changes apply on the next renderer start.",
                color = Color(0xFFAAAAAA),
                fontSize = 11.sp,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            PillButton(text = "Apply & Restart", accent = true) { Main.restart() }
        }
    }
}

@Composable
private fun GpuDriverSection(context: Context, scope: kotlinx.coroutines.CoroutineScope) {
    val installed = remember { mutableStateListOf<CustomDriver.InstalledDriver>() }
    var showBrowser by remember { mutableStateOf(false) }
    var remote by remember { mutableStateOf<List<CustomDriver.RemoteDriver>?>(null) }
    var installingId by remember { mutableStateOf<String?>(null) }
    var error by remember { mutableStateOf<String?>(null) }

    fun refresh() {
        installed.clear()
        installed.addAll(CustomDriver.listInstalled(context))
    }
    LaunchedEffect(Unit) { refresh() }

    fun setDriver(id: String?) {
        Main.customDriverId.value = id
        Main.prefs.edit().putString("customDriverId", id.orEmpty()).apply()
    }

    // SAF picker for a local AdrenoToolsDrivers .zip. octet-stream is included
    // because Drive / Files-by-Google report .zip downloads as octet-stream.
    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        installingId = "__import__"
        error = null
        scope.launch {
            val result = withContext(Dispatchers.IO) { CustomDriver.installFromUri(context, uri) }
            installingId = null
            if (result != null) {
                refresh()
                setDriver(result.id)
            } else {
                error = "Couldn't import that file. It needs an AdrenoToolsDrivers-style .zip (meta.json + libvulkan_freedreno.so at the root)."
            }
        }
    }

    Spacer(Modifier.height(6.dp))
    Text("GPU Driver", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(horizontal = 6.dp))
    Spacer(Modifier.height(2.dp))
    Text(
        "Replace the system Vulkan driver with Mesa Turnip or another Adreno driver. Recommended for Adreno-6XX devices on stale OEM drivers.",
        color = Color(0xFFAAAAAA),
        fontSize = 11.sp,
        modifier = Modifier.padding(horizontal = 6.dp),
    )
    Spacer(Modifier.height(6.dp))

    DriverRow(
        name = "Default",
        sub = "System Vulkan driver",
        selected = Main.customDriverId.value == null,
        busy = false,
        onSelect = { setDriver(null) },
        onDelete = null,
    )
    installed.forEach { drv ->
        val sub = buildString {
            if (drv.vendor.isNotEmpty()) append(drv.vendor)
            if (drv.version.isNotEmpty()) {
                if (isNotEmpty()) append(" · ")
                append(drv.version)
            }
            if (isEmpty() && drv.author.isNotEmpty()) append(drv.author)
            if (isEmpty()) append("Installed")
        }
        DriverRow(
            name = drv.name,
            sub = sub,
            selected = Main.customDriverId.value == drv.id,
            busy = false,
            onSelect = { setDriver(drv.id) },
            onDelete = {
                if (Main.customDriverId.value == drv.id) setDriver(null)
                CustomDriver.delete(drv)
                refresh()
            },
        )
    }

    Spacer(Modifier.height(6.dp))
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        PillButton(
            text = if (installingId == "__import__") "Importing…" else "Import .zip",
            accent = false,
        ) {
            if (installingId == null) {
                importLauncher.launch(arrayOf("application/zip", "application/octet-stream", "*/*"))
            }
        }
        PillButton(text = if (showBrowser) "Hide online" else "Browse online", accent = false) {
            showBrowser = !showBrowser
        }
    }

    error?.let {
        Spacer(Modifier.height(4.dp))
        Text(it, color = Color(0xFFFF8B8B), fontSize = 11.sp, modifier = Modifier.padding(horizontal = 6.dp))
    }

    if (showBrowser) {
        LaunchedEffect(showBrowser) {
            if (remote == null) {
                error = null
                val fetched = withContext(Dispatchers.IO) { CustomDriver.fetchRemote() }
                remote = fetched
                if (fetched.isEmpty()) {
                    error = "Couldn't reach github.com/K11MCH1/AdrenoToolsDrivers. Check your connection and try again."
                }
            }
        }
        val list = remote
        Spacer(Modifier.height(6.dp))
        if (list == null) {
            Text("Loading driver list…", color = Color(0xFFAAAAAA), fontSize = 11.sp,
                modifier = Modifier.padding(horizontal = 6.dp))
        } else {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(max = 220.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                list.forEach { rd ->
                    val isInstalled = installed.any { it.id == rd.id }
                    DriverBrowserRow(
                        remote = rd,
                        installed = isInstalled,
                        installing = installingId == rd.id,
                        onInstall = {
                            if (installingId != null) return@DriverBrowserRow
                            installingId = rd.id
                            error = null
                            scope.launch {
                                val result = withContext(Dispatchers.IO) {
                                    CustomDriver.download(context, rd)
                                }
                                installingId = null
                                if (result != null) {
                                    refresh()
                                    setDriver(result.id)
                                    showBrowser = false
                                } else {
                                    error = "Install failed for ${rd.assetName}. The download or extract step errored — try again."
                                }
                            }
                        },
                        onSelect = {
                            setDriver(rd.id)
                            showBrowser = false
                        },
                    )
                }
            }
        }
    }
}

/** One installed/Default driver row: tap to select, optional trash to delete. */
@Composable
private fun DriverRow(
    name: String,
    sub: String,
    selected: Boolean,
    busy: Boolean,
    onSelect: () -> Unit,
    onDelete: (() -> Unit)?,
) {
    val border = if (selected) Colors.pasx2_blue else Color.White.copy(alpha = 0.08f)
    val bg = if (selected) Colors.pasx2_blue.copy(alpha = 0.18f) else Color(0xFF1F2123).copy(alpha = 0.5f)
    Box(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 6.dp, vertical = 3.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(bg)
            .border(1.dp, border, RoundedCornerShape(8.dp))
            .clickable(enabled = !busy, onClick = onSelect)
            .controllerFocusable(
                controllerId = "driver:$name",
                shape = RoundedCornerShape(8.dp),
                onConfirm = { if (!busy) onSelect() },
            )
            .padding(horizontal = 10.dp, vertical = 6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(name, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(sub, color = Color(0xFFAAAAAA), fontSize = 11.sp, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
            if (selected) {
                Text("Active", color = Colors.pasx2_blue, fontSize = 11.sp, fontWeight = FontWeight.Bold)
            }
            if (onDelete != null) {
                Spacer(Modifier.width(10.dp))
                Box(
                    Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(Color(0xFF3A1A1A))
                        .clickable(onClick = onDelete)
                        .padding(horizontal = 8.dp, vertical = 3.dp),
                ) {
                    Text("Delete", color = Color(0xFFFF6B6B), fontSize = 11.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

/** One remote (downloadable) driver row from the GitHub release list. */
@Composable
private fun DriverBrowserRow(
    remote: CustomDriver.RemoteDriver,
    installed: Boolean,
    installing: Boolean,
    onInstall: () -> Unit,
    onSelect: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 6.dp, vertical = 3.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFF1F2123).copy(alpha = 0.5f))
            .border(1.dp, Color.White.copy(alpha = 0.08f), RoundedCornerShape(8.dp))
            .padding(horizontal = 10.dp, vertical = 6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(remote.assetName, color = Color.White, fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(remote.releaseName, color = Color(0xFFAAAAAA), fontSize = 11.sp,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Spacer(Modifier.width(10.dp))
            when {
                installing -> Text("Installing…", color = Color(0xFFAACCFF), fontSize = 11.sp,
                    fontWeight = FontWeight.Bold)
                installed -> PillButton(text = "Use", accent = false, onClick = onSelect)
                else -> PillButton(text = "Install", accent = true, onClick = onInstall)
            }
        }
    }
}

@Composable
private fun PillButton(text: String, accent: Boolean, onClick: () -> Unit) {
    val bg = if (accent) Colors.pasx2_blue else Color(0xFF2A2A2A)
    val fg = if (accent) Color.White else Color(0xFFDDDDDD)
    Box(
        Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(bg)
            .clickable(onClick = onClick)
            .controllerFocusable(
                controllerId = "button:$text",
                shape = RoundedCornerShape(6.dp),
                onConfirm = onClick,
            )
            .padding(horizontal = 12.dp, vertical = 6.dp),
    ) {
        Text(text, color = fg, fontSize = 11.sp, fontWeight = FontWeight.Bold)
    }
}
