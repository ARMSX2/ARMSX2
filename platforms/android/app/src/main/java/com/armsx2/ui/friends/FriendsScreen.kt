package com.armsx2.ui.friends

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.armsx2.DiscordPresence
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.controllerFocusable

/**
 * Who else is in ARMSX2 right now, via Discord.
 *
 * There is no ARMSX2 account here and no ARMSX2 server. Discord already knows who your friends are
 * and what they are playing, so linking an account is the entire feature — we publish what you are
 * running and read back the friends Discord says are in this same app.
 */
@Composable
fun FriendsScreen(onBack: () -> Unit) {
    val status by DiscordPresence.status
    val friends by DiscordPresence.friends
    val error by DiscordPresence.error

    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = str("friends.title"),
                leading = { RoundAction("←", str("action.back"), onClick = onBack) },
            )

            Column(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (!DiscordPresence.available()) {
                    // The SDK is optional at build time, so a build without it says so plainly
                    // rather than offering a button that cannot work.
                    GlassPanel(Modifier.fillMaxWidth()) {
                        Text(str("friends.unavailable"), style = MaterialTheme.typography.bodySmall)
                    }
                    return@Column
                }

                GlassPanel(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(
                            str("friends.explain"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        when (status) {
                            DiscordPresence.CONNECTED -> Row(
                                Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(str("friends.connected"), style = MaterialTheme.typography.bodySmall)
                                TextButton(
                                    onClick = { DiscordPresence.signOut() },
                                    modifier = Modifier.controllerFocusable(
                                        "friends.signout",
                                        onConfirm = { DiscordPresence.signOut() },
                                    ),
                                ) { Text(str("friends.disconnect")) }
                            }

                            DiscordPresence.AUTHORIZING, DiscordPresence.CONNECTING -> Row(
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                                Spacer(Modifier.size(8.dp))
                                Text(str("friends.connecting"), style = MaterialTheme.typography.bodySmall)
                            }

                            else -> Button(
                                onClick = { DiscordPresence.authorize() },
                                modifier = Modifier.controllerFocusable(
                                    "friends.connect",
                                    onConfirm = { DiscordPresence.authorize() },
                                ),
                            ) { Text(str("friends.connect")) }
                        }
                        error?.let {
                            Text(
                                it,
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                }

                if (status == DiscordPresence.CONNECTED) {
                    GlassPanel(Modifier.fillMaxWidth()) {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(
                                str("friends.playingNow"),
                                style = MaterialTheme.typography.titleSmall,
                                fontWeight = FontWeight.SemiBold,
                            )
                            if (friends.isEmpty()) {
                                // Empty is a real answer, not a failure — say which one it is, so
                                // nobody reads a blank list as the feature being broken.
                                Text(
                                    str("friends.nobody"),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            } else {
                                friends.forEach { name ->
                                    Text(name, style = MaterialTheme.typography.bodyMedium)
                                }
                            }
                        }
                    }
                }
                Spacer(Modifier.height(12.dp))
            }
        }
    }
}
