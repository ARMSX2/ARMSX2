package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.config.Dev9HostMapping
import com.armsx2.i18n.str
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import java.net.NetworkInterface

/**
 * DEV9 networking/HDD settings brought over from OG ARMSX2's SettingsActivity.
 *
 * Android's useful backend is PCSX2's socket backend. PCAP options are kept
 * visible for parity/debugging, but normal users should leave the API on
 * Sockets and the adapter on Auto. DEV9 is initialized at VM boot, so these
 * settings are persisted immediately and take effect on the next game/BIOS
 * launch.
 */
@Composable
fun NetworkTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)
    val adapters = remember { enumerateAdapters() }
    val context = androidx.compose.ui.platform.LocalContext.current
    /** This device's own LAN IPv4s, shown to the host so it can read one out to the guests. */
    val localAddresses = remember { enumerateLocalIPv4() }
    /** Stable per-device guest id. The peer id decides the emulated console's MAC and IP, so two
     *  devices sharing one cannot connect — deriving it removes both the collision and the need to
     *  ask the user for a number they have no way to choose sensibly. */
    val derivedPeerId = remember { derivePeerId(context) }
    // "Local Link" is deliberately NOT offered here — the Network mode control below owns it, so
    // there is exactly one way into LAN play. Indices still line up with NetApi in Config.h.
    val apiValues = listOf("Unset", "PCAP Bridged", "PCAP Switched", "TAP", "Sockets")
    val apiLabels = listOf("Unset", "PCAP Br.", "PCAP Sw.", "TAP", "Sockets")
    val apiIndex = apiValues.indexOf(s.dev9EthApi).let { if (it >= 0) it else apiValues.lastIndex }
    // 0 Online / 1 Host / 2 Join — derived from the stored settings rather than kept as its own
    // field, so there is a single source of truth and no way for the two to disagree.
    val netMode = if (s.dev9EthApi != "Local Link") 0 else if (s.localLinkHost) 1 else 2
    val dnsModes = listOf("Manual", "Auto", "Internal")
    val dns1Index = dnsModes.indexOf(s.dev9ModeDns1).let { if (it >= 0) it else 1 }
    val dns2Index = dnsModes.indexOf(s.dev9ModeDns2).let { if (it >= 0) it else 1 }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth(),
    ) {
        Text(
            str("network.dev9.description"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        HelpText(str("network.dev9.help"))

        ToggleRow(str("network.enableDev9Ethernet"), s.dev9EthEnable) {
            val currentDevice = s.dev9EthDevice.ifEmpty { "Auto" }
            apply(
                s.copy(
                    dev9EthEnable = it,
                    dev9EthApi = s.dev9EthApi.ifEmpty { "Sockets" },
                    dev9EthDevice = currentDevice,
                )
            )
        }
        SettingsDivider()
        // Network mode is the FIRST thing under the enable toggle, because "am I playing online or
        // on a LAN with the person next to me?" is the top-level question. LAN play used to be
        // buried as one entry in the Ethernet API picker, where nobody would ever find it.
        SegmentedRow(
            label = str("network.mode.label"),
            options = listOf(
                str("network.mode.online"),
                str("network.mode.host"),
                str("network.mode.join"),
            ),
            selectedIndex = netMode,
            description = str("network.mode.description"),
            onChange = { mode ->
                // A room code outside 4-12 chars makes LocalLinkAdapter's constructor bail, which
                // deletes the adapter and clears EthEnable — so the GAME reports "network adapter
                // not connected" and nothing points at the room code. It defaulted to empty, so the
                // feature was unusable until you happened to type one. Seed a code when switching
                // into a LAN mode; the host reads it out and guests retype it, same as the address.
                val seededCode = s.localLinkRoomCode.takeIf { it.length in 4..12 } ?: generateRoomCode()
                apply(
                    when (mode) {
                        // Host is peer 1 by protocol; guests take the derived id so two devices
                        // never share one. Set here (not during composition) so it is a plain edit.
                        1 -> s.copy(dev9EthApi = "Local Link", localLinkHost = true, localLinkPeerId = 1, localLinkRoomCode = seededCode)
                        2 -> s.copy(dev9EthApi = "Local Link", localLinkHost = false, localLinkPeerId = derivedPeerId, localLinkRoomCode = seededCode)
                        else -> s.copy(dev9EthApi = "Sockets", localLinkHost = false)
                    }
                )
            },
        )
        SettingsDivider()

        if (netMode == 0) {
            SegmentedRow(
                label = str("network.ethernetApi"),
                options = apiLabels,
                selectedIndex = apiIndex,
                onChange = { apply(s.copy(dev9EthApi = apiValues[it])) },
            )
            SettingsDivider()
            DeviceChooser(
                selected = s.dev9EthDevice.ifEmpty { "Auto" },
                adapters = adapters,
                onChange = { apply(s.copy(dev9EthDevice = it.ifEmpty { "Auto" })) },
            )
            SettingsDivider()
        } else {
            // Ordered to match what players actually do: get the devices on one network, host reads
            // out its address, guests type it in, then everyone matches port and room code.
            HelpText(str("network.localLink.help"))
            // First row in the section, because "which of my games can even do this?" is the first
            // question and the honest answer is a list we should not try to maintain ourselves.
            ActionRow(
                controllerId = "network.localLink.gamesList",
                label = str("network.localLink.gamesList"),
                description = str("network.localLink.gamesList.desc"),
                context = context,
            ) {
                android.content.Intent(
                    android.content.Intent.ACTION_VIEW,
                    android.net.Uri.parse(LAN_GAMES_URL),
                )
            }
            SettingsDivider()
            ActionRow(
                controllerId = "network.localLink.wifi",
                label = str("network.localLink.wifiSettings"),
                description = str("network.localLink.wifiSettings.desc"),
                context = context,
            ) {
                android.content.Intent(android.provider.Settings.ACTION_WIFI_SETTINGS)
            }
            SettingsDivider()
            if (netMode == 1) {
                ReadOnlyRow(
                    label = str("network.localLink.hostAddress"),
                    value = localAddresses.joinToString(", ").ifEmpty { str("network.localLink.noAddress") },
                    description = str("network.localLink.hostAddress.desc"),
                )
            } else {
                LocalLinkRow(
                    controllerId = "network.localLink.address",
                    label = str("network.localLink.address"),
                    value = s.localLinkAddress,
                    description = str("network.localLink.address.desc"),
                    fieldLabel = str("network.address"),
                ) { apply(s.copy(localLinkAddress = it)) }
            }
            SettingsDivider()
            LocalLinkRow(
                controllerId = "network.localLink.port",
                label = str("network.localLink.port"),
                value = s.localLinkPort.toString(),
                description = str("network.localLink.port.desc"),
                fieldLabel = str("network.localLink.port"),
            ) { apply(s.copy(localLinkPort = it.toIntOrNull()?.coerceIn(1024, 65535) ?: 19072)) }
            SettingsDivider()
            LocalLinkRow(
                controllerId = "network.localLink.roomCode",
                label = str("network.localLink.roomCode"),
                value = s.localLinkRoomCode,
                description = str("network.localLink.roomCode.desc"),
                fieldLabel = str("network.localLink.roomCode"),
                onGenerate = { apply(s.copy(localLinkRoomCode = generateRoomCode())) },
            ) {
                // Uppercased to match the native side, which normalises before deriving the key.
                // An out-of-range code disables DEV9 entirely, so refuse it rather than storing it.
                val cleaned = it.uppercase().filter { c -> c.isLetterOrDigit() }.take(12)
                apply(s.copy(localLinkRoomCode = cleaned.ifEmpty { s.localLinkRoomCode }))
            }
            // Loud, visible warning instead of the silent "no network adapter" the game reports.
            if (s.localLinkRoomCode.length !in 4..12)
                HelpText(str("network.localLink.roomCode.invalid"))
            SettingsDivider()
            ReadOnlyRow(
                label = str("network.localLink.peerId"),
                value = (if (netMode == 1) 1 else s.localLinkPeerId).toString(),
                description = str("network.localLink.peerId.desc"),
            )
            HelpText(str("network.localLink.limits"))
            Spacer(Modifier.height(8.dp))
        }

        // Everything below is Online-only: DHCP interception, the PS2's own IP/mask/gateway, DNS
        // modes and host mappings all describe how the emulated adapter talks to the internet.
        // Local Link assigns those itself from the peer id, so showing them in LAN mode would be
        // presenting settings that silently do nothing.
        if (netMode == 0) {
        ToggleRow(str("network.interceptDhcp"), s.dev9InterceptDhcp) {
            apply(s.copy(dev9InterceptDhcp = it))
        }
        SettingsDivider()
        ToggleRow(str("network.autoSubnetMask"), s.dev9AutoMask) {
            apply(s.copy(dev9AutoMask = it))
        }
        SettingsDivider()
        ToggleRow(str("network.autoGateway"), s.dev9AutoGateway) {
            apply(s.copy(dev9AutoGateway = it))
        }
        SettingsDivider()
        SegmentedRow(
            label = str("network.primaryDns"),
            options = dnsModes,
            selectedIndex = dns1Index,
            onChange = { apply(s.copy(dev9ModeDns1 = dnsModes[it])) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("network.secondaryDns"),
            options = dnsModes,
            selectedIndex = dns2Index,
            onChange = { apply(s.copy(dev9ModeDns2 = dnsModes[it])) },
        )
        SettingsDivider()
        EditableTextRow(str("network.ps2Ip"), s.dev9Ps2Ip) {
            apply(s.copy(dev9Ps2Ip = it.ifEmpty { "0.0.0.0" }))
        }
        SettingsDivider()
        EditableTextRow(str("network.subnetMask"), s.dev9Mask) {
            apply(s.copy(dev9Mask = it.ifEmpty { "0.0.0.0" }))
        }
        SettingsDivider()
        EditableTextRow(str("network.gateway"), s.dev9Gateway) {
            apply(s.copy(dev9Gateway = it.ifEmpty { "0.0.0.0" }))
        }
        SettingsDivider()
        EditableTextRow(str("network.dns1"), s.dev9Dns1) {
            apply(s.copy(dev9Dns1 = it.ifEmpty { "0.0.0.0" }))
        }
        SettingsDivider()
        EditableTextRow(str("network.dns2"), s.dev9Dns2) {
            apply(s.copy(dev9Dns2 = it.ifEmpty { "0.0.0.0" }))
        }
        SettingsDivider()
        HelpText(str("network.hostMappings.help"))
        run {
            val hosts = s.dev9EthHosts
            for (i in 0..hosts.size) {
                val entry = hosts.getOrNull(i)
                EditableTextRow(if (entry == null) str("network.addHost") else "${str("network.host")} ${i + 1}", entry?.url ?: "") { newUrl ->
                    val list = hosts.toMutableList()
                    if (i >= list.size) {
                        if (newUrl.isNotBlank())
                            list.add(Dev9HostMapping(url = newUrl.trim(), ip = "0.0.0.0", enabled = true))
                    } else if (newUrl.isBlank()) {
                        list.removeAt(i)
                    } else {
                        list[i] = list[i].copy(url = newUrl.trim())
                    }
                    apply(s.copy(dev9EthHosts = list))
                }
                if (entry != null) {
                    EditableTextRow("   ↳ " + str("network.mapsToIp"), entry.ip) { newIp ->
                        val list = hosts.toMutableList()
                        list[i] = list[i].copy(ip = newIp.trim().ifEmpty { "0.0.0.0" })
                        apply(s.copy(dev9EthHosts = list))
                    }
                }
                SettingsDivider()
            }
        }
        } // end Online-only block
        ToggleRow(str("network.logDhcp"), s.dev9EthLogDhcp) {
            apply(s.copy(dev9EthLogDhcp = it))
        }
        SettingsDivider()
        ToggleRow(str("network.logDns"), s.dev9EthLogDns) {
            apply(s.copy(dev9EthLogDns = it))
        }
        SettingsDivider()
        ToggleRow(str("network.enableDev9VirtualHdd"), s.dev9HddEnable) {
            apply(s.copy(dev9HddEnable = it, dev9HddFile = s.dev9HddFile.ifEmpty { "DEV9hdd.raw" }))
        }
        SettingsDivider()
        HddFileRow(
            fileName = s.dev9HddFile.ifEmpty { "DEV9hdd.raw" },
            onChange = { apply(s.copy(dev9HddFile = it.ifEmpty { "DEV9hdd.raw" })) },
            onReset = { apply(s.copy(dev9HddFile = "DEV9hdd.raw")) },
        )
        HelpText(str("network.hddImage.help"))

        Spacer(Modifier.height(16.dp))
        Text(
            str("network.usb.header"),
            color = Colors.pasx2_blue,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 4.dp),
        )
        ToggleRow(str("network.emulateUsbKeyboard"), s.usbKeyboard) {
            apply(s.copy(usbKeyboard = it))
        }
        HelpText(str("network.usbKeyboard.help"))
    }
}

/** This device's own LAN IPv4 addresses, so a host can read one out to the guests instead of being
 *  told to go hunting in Android's settings. Hotspot interfaces are included on purpose — a hotspot
 *  is the most reliable way to get two handhelds onto one network. */
private fun enumerateLocalIPv4(): List<String> {
    val out = linkedSetOf<String>()
    runCatching {
        val interfaces = NetworkInterface.getNetworkInterfaces() ?: return@runCatching
        for (iface in interfaces.toList()) {
            val usable = runCatching { iface.isUp && !iface.isLoopback }.getOrDefault(false)
            if (!usable) continue
            for (addr in iface.inetAddresses.toList()) {
                if (addr is java.net.Inet4Address && !addr.isLoopbackAddress)
                    addr.hostAddress?.let { out.add(it) }
            }
        }
    }
    return out.toList()
}

/** A fresh 8-character room code. Seeded automatically when LAN mode is first selected, because an
 *  empty code silently disables DEV9 (see the Network mode onChange for the full failure chain).
 *  Uppercase alphanumerics only, matching what the native side normalises to. */
private fun generateRoomCode(): String {
    val alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" // no I/O/0/1 — these get read aloud
    return (1..8).map { alphabet[kotlin.random.Random.nextInt(alphabet.length)] }.joinToString("")
}

/** A stable guest peer id in 2..65533, derived from ANDROID_ID so it differs per device and never
 *  needs to be chosen by hand. 1 is reserved for the host by the wire protocol. */
private fun derivePeerId(context: android.content.Context): Int {
    val seed = runCatching {
        android.provider.Settings.Secure.getString(
            context.contentResolver,
            android.provider.Settings.Secure.ANDROID_ID,
        )
    }.getOrNull().orEmpty().ifEmpty { android.os.Build.FINGERPRINT }
    // 65532 slots starting at 2; abs() on the hash, guarding Int.MIN_VALUE.
    val h = seed.hashCode()
    val positive = if (h == Int.MIN_VALUE) 0 else if (h < 0) -h else h
    return 2 + (positive % 65532)
}

/** A tappable label+subtitle row that fires an Intent. Used for the Wi-Fi shortcut (the devices have
 *  to be on one network before any of this works, and that is the step people miss) and for the
 *  supported-games list. Registers with the pad-nav registry — without that the whole Local Link
 *  section was unreachable on a controller, since only the shared ToggleRow/SegmentedRow widgets
 *  self-register and every custom row here was skipped. */
@Composable
private fun ActionRow(
    controllerId: String,
    label: String,
    description: String,
    context: android.content.Context,
    intent: () -> android.content.Intent,
) {
    val fire = {
        // runCatching: no ACTION_VIEW handler (no browser) or a blocked settings intent must not
        // take the settings screen down with it.
        runCatching {
            context.startActivity(intent().addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
        }
        Unit
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable(onClick = fire)
            .controllerFocusable(controllerId, onConfirm = fire)
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                description,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 12.sp,
            )
        }
    }
}

/** Wikipedia's LAN-games section: the authoritative answer to "which games can I actually use this
 *  with?", which is the first thing anyone asks. Kept as a link rather than a baked-in list so it
 *  can't go stale in our strings. */
private const val LAN_GAMES_URL =
    "https://en.wikipedia.org/wiki/List_of_PlayStation_2_online_games#LAN_Games"

/** A non-editable value row (host address, local peer id) — same shape as the editable rows so the
 *  section reads consistently, but with no tap target, because these are computed, not chosen. */
@Composable
private fun ReadOnlyRow(label: String, value: String, description: String) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 8.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.weight(1f))
            Text(
                value,
                color = Color(0xFFCCCCCC),
                fontSize = 14.sp,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Text(
            description,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 3.dp),
        )
    }
}

/**
 * A Local Link field: same look as [EditableTextRow], but it shows a description under the label
 * and does not assume the value is an IP address. [EditableTextRow] prefills AND displays
 * "0.0.0.0" for an empty value and hardcodes the edit dialog's field label to "Address" — correct
 * for the DNS/gateway rows it serves, wrong for a room code, a port or a peer id. Kept separate
 * rather than adding switches to that one, which has a dozen existing call sites.
 *
 * onChange receives the trimmed text; callers do their own validation/coercion.
 */
@Composable
private fun LocalLinkRow(
    controllerId: String,
    label: String,
    value: String,
    description: String,
    fieldLabel: String,
    /** When supplied, adds a Generate action (button + D-pad Right) that fills the field. Used for
     *  the room code, which has validity rules a person shouldn't have to remember. */
    onGenerate: (() -> Unit)? = null,
    onChange: (String) -> Unit,
) {
    // Text entry goes through LibraryKeyboard, NOT an AlertDialog. A Compose dialog takes its own
    // focused window and swallows gamepad keys, so a pad user could open it and then be stuck with
    // no way to type or dismiss. LibraryKeyboard is D-pad navigable by design, and it also honours
    // the "Use system keyboard" preference for touch users.
    val edit = {
        com.armsx2.ui.home.LibraryKeyboard.open(value, onChange, fieldLabel)
    }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable(onClick = edit)
            .controllerFocusable(
                controllerId,
                onConfirm = edit,
                // D-pad Right regenerates, matching how other rows use left/right to adjust.
                onRight = onGenerate,
            )
            .padding(horizontal = 6.dp, vertical = 8.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.weight(1f))
            Text(
                value.ifEmpty { str("network.localLink.notSet") },
                color = Color(0xFFCCCCCC),
                fontSize = 14.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (onGenerate != null) {
                // One-tap valid code, without opening the editor. Its own clickable consumes the
                // tap, so it does not also open the row's edit dialog.
                TextButton(onClick = onGenerate) { Text(str("network.localLink.generate")) }
            }
        }
        Text(
            description,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 3.dp),
        )
    }
}

@Composable
private fun EditableTextRow(label: String, value: String, onChange: (String) -> Unit) {
    var editing by remember(label) { mutableStateOf(false) }
    var draft by remember(label, value) { mutableStateOf(value.ifEmpty { "0.0.0.0" }) }
    if (editing) {
        AlertDialog(
            onDismissRequest = { editing = false },
            title = { Text(label) },
            text = {
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = it },
                    singleLine = true,
                    label = { Text(str("network.address")) },
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onChange(draft.trim())
                    editing = false
                }) { Text(str("action.save")) }
            },
            dismissButton = {
                TextButton(onClick = { editing = false }) { Text(str("action.cancel")) }
            },
        )
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable { editing = true }
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.weight(1f))
        Text(
            value.ifEmpty { "0.0.0.0" },
            color = Color(0xFFCCCCCC),
            fontSize = 14.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun DeviceChooser(
    selected: String,
    adapters: List<String>,
    onChange: (String) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 4.dp),
    ) {
        Text(str("network.ethernetDevice"), color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(4.dp))
        adapters.forEach { adapter ->
            val active = adapter == selected
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(56.dp)
                    .clickable { onChange(adapter) }
                    .padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    adapter,
                    color = if (active) Colors.pasx2_blue else Color(0xFFCCCCCC),
                    fontSize = 15.sp,
                    fontWeight = if (active) FontWeight.Bold else FontWeight.Normal,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.weight(1f))
                if (active) {
                    Text(str("network.selected"), color = Colors.pasx2_blue, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

@Composable
private fun HddFileRow(fileName: String, onChange: (String) -> Unit, onReset: () -> Unit) {
    var editing by remember { mutableStateOf(false) }
    var draft by remember(fileName) { mutableStateOf(fileName) }
    if (editing) {
        AlertDialog(
            onDismissRequest = { editing = false },
            title = { Text(str("network.hddImage.title")) },
            text = {
                Column {
                    Text(
                        str("network.hddImage.dialogHint"),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        fontSize = 14.sp,
                        modifier = Modifier.padding(bottom = 6.dp),
                    )
                    OutlinedTextField(
                        value = draft,
                        onValueChange = { draft = it },
                        singleLine = true,
                        label = { Text(str("network.hddImage.fieldLabel")) },
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    onChange(draft.trim())
                    editing = false
                }) { Text(str("action.save")) }
            },
            dismissButton = {
                TextButton(onClick = { editing = false }) { Text(str("action.cancel")) }
            },
        )
    }
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable { draft = fileName; editing = true }
            .padding(horizontal = 6.dp, vertical = 4.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(str("network.hddImage.title"), color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                Text(fileName, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text(
                str("action.reset"),
                color = Colors.pasx2_blue,
                fontSize = 15.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.clickable { onReset() }.padding(start = 8.dp),
            )
        }
    }
}

private fun enumerateAdapters(): List<String> {
    val out = linkedSetOf("Auto")
    runCatching {
        val interfaces = NetworkInterface.getNetworkInterfaces() ?: return@runCatching
        interfaces.toList()
            .filter { iface ->
                runCatching {
                    iface.isUp && !iface.isLoopback && !iface.isVirtual
                }.getOrDefault(false)
            }
            .mapTo(out) { it.name }
    }
    return out.toList()
}
