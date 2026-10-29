// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.SegmentedRow
import com.armsx2.ui.settings.ToggleRow
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.delay
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface

/**
 * Remote play from the library: join a friend as player 2, on the same Wi-Fi or with the invite
 * code they sent. Hosting is in the in-game menu (Friends), since only a running game can be hosted.
 */
@Composable
fun RemotePlayScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var hosts by remember { mutableStateOf(emptyList<RemotePlayHostInfo>()) }
    var address by remember { mutableStateOf("") }
    var invite by remember { mutableStateOf("") }
    var inviteError by remember { mutableStateOf(false) }
    var scanning by remember { mutableStateOf(false) }
    var exitMode by remember { mutableStateOf(RemotePlayPrefs.exitButton(context)) }
    var touchMode by remember { mutableStateOf(RemotePlayPrefs.touchControls(context)) }
    var statsOn by remember { mutableStateOf(RemotePlayPrefs.streamStats(context)) }
    var qosOn by remember { mutableStateOf(RemotePlayPrefs.wifiQos(context)) }

    // Android 17: finding and joining a host on the same Wi-Fi needs the local network permission
    // (RemotePlayLocalNetwork). Set once the player refused it, so the screen says why nothing comes up.
    val localNetwork = rememberLocalNetworkAccess()
    var localNetworkRefused by remember { mutableStateOf(false) }

    // Scan only when asked, and for a minute at most: browsing holds a Wi-Fi multicast lock and
    // keeps the radio busy, which costs battery.
    DisposableEffect(scanning) {
        if (!scanning) return@DisposableEffect onDispose {}
        val browser = HostBrowser(context) { hosts = it }
        browser.start()
        onDispose { browser.stop() }
    }
    LaunchedEffect(scanning) {
        if (scanning) {
            delay(60_000)
            scanning = false
        }
    }

    // A six-character code from the lobby service, or a whole pasted invite. Online works without
    // the local network, so this asks for it (a host in the same house needs it) but goes ahead
    // either way.
    val joinInvite: () -> Unit = {
        val lobbyCode = RemotePlayLobby.normalize(invite)
        val offer = if (lobbyCode == null) RemotePlayProtocol.decodeCode(invite) else null
        when {
            lobbyCode != null -> {
                inviteError = false
                invite = ""
                localNetwork { RemotePlayViewerActivity.startWithLobby(context, lobbyCode) }
            }
            offer != null && offer.optString("t") == "offer" -> {
                inviteError = false
                invite = ""
                localNetwork { RemotePlayViewerActivity.startWithInvite(context, offer) }
            }
            else -> inviteError = true
        }
    }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = str("remoteplay.title"),
                // Reachable by the pad: it is the only control above the code field, so Up from
                // there lands here and the page scrolls back to its top instead of sticking.
                leading = {
                    RoundAction("←", str("action.back"), onClick = onBack, controllerId = "remoteplay.top.back")
                },
            )
            Column(
                Modifier.padding(horizontal = 8.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                GlassPanel(Modifier.fillMaxWidth()) {
                    Text(
                        str("remoteplay.explain"),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                GlassPanel(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SectionTitle(str("remoteplay.join.code"))
                        Text(
                            str("remoteplay.join.codeHint"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        // A controller types the code on ARMSX2's own keyboard: the system one cannot
                        // be driven with a D-pad here.
                        val typeCode = {
                            com.armsx2.ui.home.LibraryKeyboard.open(
                                invite,
                                {
                                    invite = it
                                    inviteError = false
                                },
                                I18n.get("remoteplay.join.codeField"),
                            )
                        }
                        CodeField(
                            value = invite,
                            onValueChange = {
                                invite = it
                                inviteError = false
                            },
                            label = str("remoteplay.join.codeField"),
                            modifier = Modifier.controllerFocusable("remoteplay.join.codeField", onConfirm = typeCode),
                        )
                        if (inviteError) ErrorText(str("remoteplay.join.badCode"))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            val paste: () -> Unit = {
                                clipboardText(context)?.let {
                                    invite = it
                                    inviteError = false
                                }
                            }
                            OutlinedButton(
                                onClick = paste,
                                modifier = Modifier.controllerFocusable("remoteplay.join.paste", onConfirm = paste),
                            ) { Text(str("remoteplay.code.paste")) }
                            Button(
                                onClick = joinInvite,
                                enabled = invite.isNotBlank(),
                                modifier = Modifier.controllerFocusable("remoteplay.join.invite", onConfirm = joinInvite),
                            ) { Text(str("remoteplay.join.connect")) }
                        }
                    }
                }

                GlassPanel(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SectionTitle(str("remoteplay.join.title"))
                        if (localNetworkRefused) ErrorText(str("remoteplay.permission.join"))
                        val toggleScan = {
                            if (scanning) {
                                scanning = false
                            } else {
                                localNetwork { allowed ->
                                    localNetworkRefused = !allowed
                                    scanning = allowed
                                }
                            }
                        }
                        if (scanning) {
                            if (hosts.isEmpty()) {
                                Busy(str("remoteplay.join.searching"))
                                Text(
                                    str("remoteplay.join.howToHost"),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            OutlinedButton(
                                onClick = toggleScan,
                                modifier = Modifier.controllerFocusable("remoteplay.join.scan", onConfirm = toggleScan),
                            ) { Text(str("remoteplay.join.scanStop")) }
                        } else {
                            Text(
                                str("remoteplay.join.scanHint"),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Button(
                                onClick = toggleScan,
                                modifier = Modifier.controllerFocusable("remoteplay.join.scan", onConfirm = toggleScan),
                            ) { Text(str("remoteplay.join.scan")) }
                        }
                        hosts.forEach { host ->
                            HostRow(host) {
                                localNetwork { allowed ->
                                    if (allowed) {
                                        RemotePlayViewerActivity.start(context, host.address, host.port, host.name)
                                    } else {
                                        localNetworkRefused = true
                                    }
                                }
                            }
                        }
                    }
                }

                GlassPanel(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        SectionTitle(str("remoteplay.screen.title"))
                        Text(
                            str("remoteplay.screen.note"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        val choices = listOf(str("remoteplay.screen.tap"), str("remoteplay.screen.on"), str("remoteplay.screen.off"))
                        val modes = RevealMode.entries
                        SegmentedRow(
                            label = str("remoteplay.screen.exit"),
                            options = choices,
                            selectedIndex = modes.indexOf(exitMode),
                            description = str("remoteplay.screen.exitDesc"),
                        ) { index ->
                            exitMode = modes[index]
                            RemotePlayPrefs.setExitButton(context, exitMode)
                        }
                        SegmentedRow(
                            label = str("remoteplay.screen.touch"),
                            options = choices,
                            selectedIndex = modes.indexOf(touchMode),
                            description = str("remoteplay.screen.touchDesc"),
                        ) { index ->
                            touchMode = modes[index]
                            RemotePlayPrefs.setTouchControls(context, touchMode)
                        }
                        ToggleRow(
                            label = str("remoteplay.screen.stats"),
                            value = statsOn,
                            description = str("remoteplay.screen.statsDesc"),
                        ) { on ->
                            statsOn = on
                            RemotePlayPrefs.setStreamStats(context, on)
                        }
                        ToggleRow(
                            label = str("remoteplay.screen.qos"),
                            value = qosOn,
                            description = str("remoteplay.screen.qosDesc"),
                        ) { on ->
                            qosOn = on
                            RemotePlayPrefs.setWifiQos(context, on)
                        }
                    }
                }

                GlassPanel(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SectionTitle(str("remoteplay.join.manual"))
                        if (localNetworkRefused) ErrorText(str("remoteplay.permission.join"))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            val typeAddress = {
                                com.armsx2.ui.home.LibraryKeyboard.open(
                                    address,
                                    { address = it.trim() },
                                    I18n.get("remoteplay.join.address"),
                                )
                            }
                            // A placeholder, not a label: a label reserves room above the box, and the
                            // pad's highlight, which follows the field's bounds, then sat above the
                            // outline instead of on it.
                            OutlinedTextField(
                                value = address,
                                onValueChange = { address = it.trim() },
                                singleLine = true,
                                placeholder = { Text(str("remoteplay.join.address")) },
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                                modifier = Modifier
                                    .weight(1f)
                                    .controllerFocusable("remoteplay.join.addressField", onConfirm = typeAddress),
                            )
                            Spacer(Modifier.width(8.dp))
                            val join = {
                                localNetwork { allowed ->
                                    if (allowed) joinAddress(context, address) else localNetworkRefused = true
                                }
                            }
                            Button(
                                onClick = join,
                                enabled = address.isNotBlank(),
                                modifier = Modifier.controllerFocusable("remoteplay.join.address", onConfirm = join),
                            ) { Text(str("remoteplay.join.connect")) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun HostRow(host: RemotePlayHostInfo, onJoin: () -> Unit) {
    Row(
        Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                host.name,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (host.game.isNotBlank()) {
                Text(
                    host.game,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Spacer(Modifier.width(8.dp))
        Button(
            onClick = onJoin,
            modifier = Modifier.controllerFocusable("remoteplay.host.${host.name}", onConfirm = onJoin),
        ) { Text(str("remoteplay.join.connect")) }
    }
}

/** "192.168.1.20" or "192.168.1.20:38433". */
private fun joinAddress(context: Context, text: String) {
    val value = text.trim()
    if (value.isEmpty()) return
    val colon = value.lastIndexOf(':')
    // A port out of range is no port: the address is tried on the usual one.
    val port = if (colon > 0) value.substring(colon + 1).toIntOrNull()?.takeIf { it in 1..65535 } else null
    val host = if (port != null) value.substring(0, colon) else value
    RemotePlayViewerActivity.start(context, host, port ?: RemotePlayProtocol.PORT, host)
}

/** Hosting, in the in-game menu's Friends overlay. */
@Composable
fun RemotePlayHostPanel(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val status by RemotePlayHost.status
    // Android 17: hosting on the same Wi-Fi needs the local network permission. Online works
    // without it, so an invite asks too (for the router port and a friend in the same house) but
    // goes ahead either way.
    val localNetwork = rememberLocalNetworkAccess()
    val hostWifi = {
        localNetwork { allowed ->
            if (allowed) RemotePlayHost.startWifi(context) else RemotePlayHost.refuseLocalNetwork()
        }
    }
    val invite = { localNetwork { RemotePlayHost.createInvite(context) } }
    val stop = { RemotePlayHost.stop() }

    GlassPanel(modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            SectionTitle(str("remoteplay.title"))
            when (val current = status) {
                RemotePlayHost.Status.Off -> {
                    Text(
                        str("remoteplay.host.explain"),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(
                            onClick = hostWifi,
                            modifier = Modifier.controllerFocusable("remoteplay.host.wifi", onConfirm = hostWifi),
                        ) { Text(str("remoteplay.host.wifi")) }
                        Button(
                            onClick = invite,
                            modifier = Modifier.controllerFocusable("remoteplay.host.invite", onConfirm = invite),
                        ) { Text(str("remoteplay.host.invite")) }
                    }
                }

                is RemotePlayHost.Status.Waiting -> {
                    Busy(str("remoteplay.host.waiting"))
                    current.address?.let {
                        Text(
                            str("remoteplay.host.address").format(it),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(
                            onClick = invite,
                            modifier = Modifier.controllerFocusable("remoteplay.host.invite", onConfirm = invite),
                        ) { Text(str("remoteplay.host.invite")) }
                        StopButton(stop)
                    }
                }

                is RemotePlayHost.Status.Inviting -> {
                    val lobbyCode = current.lobbyCode
                    if (lobbyCode != null) LobbyInvitePanel(lobbyCode, stop) else InvitePanel(current.code, stop)
                }

                is RemotePlayHost.Status.Connecting -> {
                    Busy(str("remoteplay.host.connecting").format(current.viewer))
                    StopButton(stop)
                }

                is RemotePlayHost.Status.Asking -> AskPanel(current.viewer)

                is RemotePlayHost.Status.Playing -> {
                    Text(str("remoteplay.host.playing").format(current.viewer), style = MaterialTheme.typography.bodySmall)
                    StopButton(stop)
                }

                is RemotePlayHost.Status.Failed -> {
                    ErrorText(current.message)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(
                            onClick = hostWifi,
                            modifier = Modifier.controllerFocusable("remoteplay.host.wifi", onConfirm = hostWifi),
                        ) { Text(str("remoteplay.host.wifi")) }
                        Button(
                            onClick = invite,
                            modifier = Modifier.controllerFocusable("remoteplay.host.invite", onConfirm = invite),
                        ) { Text(str("remoteplay.host.invite")) }
                    }
                }
            }
        }
    }
}

/** An online invite through the lobby service: one short code, and the friend's side connects by itself. */
@Composable
private fun LobbyInvitePanel(code: String, cancel: () -> Unit) {
    val context = LocalContext.current
    val shown = RemotePlayLobby.display(code)
    val share = { shareText(context, I18n.get("remoteplay.invite.lobbyShare").format(shown)) }
    val copy = { copyText(context, shown) }

    Text(str("remoteplay.invite.lobbyCode"), style = MaterialTheme.typography.bodySmall)
    Text(
        shown,
        style = MaterialTheme.typography.headlineMedium,
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.Bold,
        letterSpacing = 2.sp,
    )
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(
            onClick = share,
            modifier = Modifier.controllerFocusable("remoteplay.invite.share", onConfirm = share),
        ) { Text(str("remoteplay.code.share")) }
        OutlinedButton(
            onClick = copy,
            modifier = Modifier.controllerFocusable("remoteplay.invite.copy", onConfirm = copy),
        ) { Text(str("remoteplay.code.copy")) }
    }
    Busy(str("remoteplay.invite.lobbyWaiting"))
    StopButton(cancel, str("remoteplay.invite.cancel"))
}

/** An online invite without the lobby service: send the code, then paste the friend's reply. */
@Composable
private fun InvitePanel(code: String?, cancel: () -> Unit) {
    val context = LocalContext.current
    var reply by remember { mutableStateOf("") }
    var replyError by remember { mutableStateOf(false) }

    if (code == null) {
        Busy(str("remoteplay.invite.preparing"))
        StopButton(cancel, str("remoteplay.invite.cancel"))
        return
    }

    val share = { shareText(context, I18n.get("remoteplay.invite.share").format(code)) }
    val copy = { copyText(context, code) }
    val paste: () -> Unit = {
        clipboardText(context)?.let {
            reply = it
            replyError = false
        }
    }
    val connect: () -> Unit = {
        if (!RemotePlayHost.acceptReply(reply)) replyError = true
    }

    Text(str("remoteplay.invite.step1"), style = MaterialTheme.typography.bodySmall)
    Text(
        code.take(32) + "...",
        style = MaterialTheme.typography.labelSmall,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(
            onClick = share,
            modifier = Modifier.controllerFocusable("remoteplay.invite.share", onConfirm = share),
        ) { Text(str("remoteplay.code.share")) }
        OutlinedButton(
            onClick = copy,
            modifier = Modifier.controllerFocusable("remoteplay.invite.copy", onConfirm = copy),
        ) { Text(str("remoteplay.code.copy")) }
    }

    Text(str("remoteplay.invite.step2"), style = MaterialTheme.typography.bodySmall)
    CodeField(
        value = reply,
        onValueChange = {
            reply = it
            replyError = false
        },
        label = str("remoteplay.invite.reply"),
    )
    if (replyError) ErrorText(str("remoteplay.invite.badReply"))
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(
            onClick = paste,
            modifier = Modifier.controllerFocusable("remoteplay.invite.paste", onConfirm = paste),
        ) { Text(str("remoteplay.code.paste")) }
        Button(
            onClick = connect,
            enabled = reply.isNotBlank(),
            modifier = Modifier.controllerFocusable("remoteplay.invite.connect", onConfirm = connect),
        ) { Text(str("remoteplay.invite.connect")) }
        StopButton(cancel, str("remoteplay.invite.cancel"))
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
}

@Composable
private fun Busy(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Spacer(Modifier.width(8.dp))
        Text(text, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun ErrorText(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
}

/** A pasted code is long, so the field shows its start and keeps it on a few lines. */
@Composable
private fun CodeField(value: String, onValueChange: (String) -> Unit, label: String, modifier: Modifier = Modifier) {
    // A placeholder, not a label, for the reason given at the address field above.
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        placeholder = { Text(label) },
        maxLines = 3,
        textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
        modifier = modifier.fillMaxWidth(),
    )
}

@Composable
private fun StopButton(stop: () -> Unit, label: String = str("remoteplay.host.stop")) {
    OutlinedButton(
        onClick = stop,
        modifier = Modifier.controllerFocusable("remoteplay.host.stop", onConfirm = stop),
    ) { Text(label) }
}

/** A friend is connected and waits to be let in: the host accepts or declines them. */
@Composable
private fun AskPanel(viewer: String) {
    // The menu may have opened by itself over the game, so the buttons wait a moment before they
    // take a press: one still coming from play must not answer for the host.
    var armed by remember(viewer) { mutableStateOf(false) }
    LaunchedEffect(viewer) {
        delay(700)
        armed = true
    }
    val accept = { if (armed) RemotePlayHost.accept() }
    val decline = { if (armed) RemotePlayHost.decline() }

    Text(
        str("remoteplay.request.title").format(viewer),
        style = MaterialTheme.typography.bodyMedium,
        fontWeight = FontWeight.SemiBold,
    )
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(
            onClick = accept,
            enabled = armed,
            modifier = Modifier.controllerFocusable("remoteplay.request.accept", onConfirm = accept),
        ) { Text(str("remoteplay.request.accept")) }
        OutlinedButton(
            onClick = decline,
            enabled = armed,
            modifier = Modifier.controllerFocusable("remoteplay.request.decline", onConfirm = decline),
        ) { Text(str("remoteplay.request.decline")) }
    }
}

/** Opens the share sheet, so the player can send a code over Discord or any chat. */
internal fun shareText(context: Context, text: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text)
    val chooser = Intent.createChooser(send, null)
    if (context !is Activity) chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    runCatching { context.startActivity(chooser) }
}

internal fun copyText(context: Context, text: String) {
    val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText("ARMSX2", text))
    // Android 13 and later confirm a copy themselves.
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
        Toast.makeText(context, I18n.get("remoteplay.code.copied"), Toast.LENGTH_SHORT).show()
    }
}

/** Pasting with a button, since a long-press paste is awkward with a controller. */
internal fun clipboardText(context: Context): String? {
    val clip = context.getSystemService(ClipboardManager::class.java)?.primaryClip ?: return null
    if (clip.itemCount == 0) return null
    return clip.getItemAt(0).coerceToText(context)?.toString()
}

/**
 * "<name> joined as player 2" or "<name> left", at the top of the screen over the game, for the host:
 * they are playing, with the in-game menu's hosting status out of sight. At the Activity root next to
 * the Discord friend banner, and the same shape. Gone again by itself.
 */
@Composable
fun RemotePlayNoticeBanner() {
    val notice by RemotePlayHost.notice

    // Keyed on the notice, so a second one during the first restarts the clock.
    LaunchedEffect(notice) {
        val shown = notice ?: return@LaunchedEffect
        kotlinx.coroutines.delay(3_500)
        RemotePlayHost.clearNotice(shown)
    }

    // The last notice, for the exit animation, which still draws after the state clears. Not state:
    // keeping it must not compose again.
    val last = remember { arrayOfNulls<RemotePlayHost.Notice>(1) }
    notice?.let { last[0] = it }

    AnimatedVisibility(
        visible = notice != null,
        enter = fadeIn(tween(180)) + slideInVertically(tween(240)) { -it },
        exit = fadeOut(tween(160)) + slideOutVertically(tween(200)) { -it },
    ) {
        val shown = last[0] ?: return@AnimatedVisibility
        Surface(
            modifier = Modifier
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top))
                .padding(horizontal = 14.dp, vertical = 10.dp),
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.surfaceVariant,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.55f)),
            shadowElevation = 14.dp,
        ) {
            Text(
                str(if (shown.joined) "remoteplay.notice.joined" else "remoteplay.notice.left").format(shown.name),
                modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}
