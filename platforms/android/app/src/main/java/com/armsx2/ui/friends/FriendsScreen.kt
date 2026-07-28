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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import com.armsx2.DiscordFriend
import com.armsx2.DiscordPresence
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.SettingSwitchRow
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
    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = str("friends.title"),
                leading = { RoundAction("←", str("action.back"), onClick = onBack) },
            )
            FriendsPanel(Modifier.padding(horizontal = 8.dp))
        }
    }
}

/**
 * The connect/status/friends block, without any screen chrome.
 *
 * Shared verbatim with the in-game menu's Friends tab rather than reimplemented there: the state
 * lives in [DiscordPresence], so two copies would be two things to keep in step for no gain.
 */
@Composable
fun FriendsPanel(modifier: Modifier = Modifier) {
    val status by DiscordPresence.status
    val friends by DiscordPresence.friends
    val error by DiscordPresence.error
    val notify by DiscordPresence.notifyState

    Column(
        modifier.fillMaxWidth(),
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
                        // Whose account this is, not just that some account is linked — on a
                        // shared device "Discord connected" does not answer the question.
                        val me = DiscordPresence.self.value
                        if (me != null) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                if (me.avatarUrl.isNotBlank()) {
                                    AsyncImage(
                                        model = me.avatarUrl,
                                        contentDescription = me.name,
                                        modifier = Modifier.size(28.dp).clip(CircleShape),
                                        contentScale = ContentScale.Crop,
                                    )
                                    Spacer(Modifier.width(8.dp))
                                }
                                Column {
                                    Text(
                                        me.name,
                                        style = MaterialTheme.typography.bodyMedium,
                                        fontWeight = FontWeight.SemiBold,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                    Text(
                                        str("friends.connected"),
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                        } else {
                            Text(str("friends.connected"), style = MaterialTheme.typography.bodySmall)
                        }
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
                SettingSwitchRow(
                    title = str("friends.notify"),
                    description = str("friends.notify.desc"),
                    checked = notify,
                    onCheckedChange = { DiscordPresence.notifyInGame = it },
                )
            }

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
                        friends.forEach { friend -> FriendRow(friend) }
                    }
                }
            }
        }
    Spacer(Modifier.height(12.dp))
    }
}

/**
 * One friend: who they are, and what they are playing.
 *
 * The cover is the point — a row of names does not tell you whether it is worth joining them. When
 * they are in the library there is no cover to show, so their Discord avatar stands in rather than
 * leaving a hole the width of a box art.
 */
@Composable
private fun FriendRow(friend: DiscordFriend) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Avatar AND cover, never one instead of the other: the avatar is who they are and the
        // cover is what they are doing, so swapping the face out for box art the moment somebody
        // starts a game loses the identity exactly when the row gets interesting.
        if (friend.avatarUrl.isNotBlank()) {
            AsyncImage(
                model = friend.avatarUrl,
                contentDescription = friend.name,
                modifier = Modifier.size(36.dp).clip(CircleShape),
                contentScale = ContentScale.Crop,
            )
        } else {
            Spacer(Modifier.width(36.dp))
        }

        Spacer(Modifier.width(10.dp))
        friend.coverUrl?.let { cover ->
            AsyncImage(
                model = cover,
                contentDescription = friend.game,
                modifier = Modifier.size(width = 26.dp, height = 36.dp)
                    .clip(RoundedCornerShape(3.dp)),
                contentScale = ContentScale.Crop,
            )
            Spacer(Modifier.width(10.dp))
        }
        Column(Modifier.fillMaxWidth()) {
            Text(
                friend.name,
                style = MaterialTheme.typography.bodyMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                // A game we could not name still is not the library — say "playing" rather than
                // claiming they are idle, which would be the one genuinely wrong answer here.
                if (friend.inLibrary) str("friends.inLibrary")
                else friend.game.ifBlank { str("friends.playing") },
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}


/**
 * How many friends are in ARMSX2, as a Discord-style red pip.
 *
 * Shown wherever the Friends entry point is, because the entry point is the only place it can do
 * its job: the whole reason it exists is to say "somebody is on" to a person who is not currently
 * looking at the friends list.
 *
 * Renders nothing at zero — an empty badge is noise, and a "0" is worse than no badge at all.
 */
@Composable
fun FriendsCountBadge(modifier: Modifier = Modifier) {
    val friends by DiscordPresence.friends
    val status by DiscordPresence.status
    if (status != DiscordPresence.CONNECTED || friends.isEmpty()) return

    Box(
        modifier.size(17.dp).clip(CircleShape).background(Color(0xFFE0393E)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            if (friends.size > 9) "9+" else friends.size.toString(),
            color = Color.White,
            fontSize = 9.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
        )
    }
}

/** The 👥 glyph with [FriendsCountBadge] pinned to its corner. */
@Composable
fun FriendsGlyphWithBadge(color: Color, glyphSize: androidx.compose.ui.unit.TextUnit = 22.sp) {
    Box {
        Text("\uD83D\uDC65", color = color, fontSize = glyphSize, fontWeight = FontWeight.Bold)
        FriendsCountBadge(Modifier.align(Alignment.TopEnd).offset(x = 7.dp, y = (-6).dp))
    }
}


/**
 * "<name> is now online", over the library, with their avatar.
 *
 * The library equivalent of the in-game OSD message. It has to exist separately because the OSD is
 * drawn by the emulator: with no game running there is nothing rendering it, so an OSD call outside
 * a game goes nowhere. A Toast was the obvious substitute and the wrong one — custom toast views
 * have been blocked since Android 12, so a Toast could never show the avatar.
 *
 * Self-dismissing. Nothing here is worth a button to get rid of.
 */
@Composable
fun FriendOnlineBanner() {
    val friend by DiscordPresence.justOnline

    // Keyed on the friend, so a second arrival during the first banner restarts the clock rather
    // than inheriting the remains of the previous one's.
    LaunchedEffect(friend) {
        if (friend != null) {
            kotlinx.coroutines.delay(4200)
            DiscordPresence.justOnline.value = null
        }
    }

    AnimatedVisibility(
        visible = friend != null,
        enter = fadeIn(tween(180)) + slideInVertically(tween(240)) { -it },
        exit = fadeOut(tween(160)) + slideOutVertically(tween(200)) { -it },
    ) {
        // Held separately so the exit animation still has something to draw after the state clears.
        val shown = remember(friend) { friend }
        Surface(
            modifier = Modifier
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top))
                .padding(horizontal = 14.dp, vertical = 10.dp),
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.surfaceVariant,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.55f)),
            shadowElevation = 14.dp,
        ) {
            Row(
                Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (!shown?.avatarUrl.isNullOrBlank()) {
                    AsyncImage(
                        model = shown?.avatarUrl,
                        contentDescription = null,
                        modifier = Modifier.size(32.dp).clip(CircleShape),
                        contentScale = ContentScale.Crop,
                    )
                    Spacer(Modifier.width(10.dp))
                }
                Text(
                    shown?.name.orEmpty(),
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                Spacer(Modifier.width(5.dp))
                Text(
                    str("friends.nowOnline"),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                )
            }
        }
    }
}


/**
 * The signed-in account, compact: avatar and name only.
 *
 * For the in-game panel header, which has a title and a close button and a gap between them that
 * should say who you are rather than nothing. Renders nothing when not connected.
 */
@Composable
fun SelfChip(modifier: Modifier = Modifier) {
    val me by DiscordPresence.self
    val status by DiscordPresence.status
    if (status != DiscordPresence.CONNECTED || me == null) return

    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        me?.avatarUrl?.takeIf { it.isNotBlank() }?.let { avatar ->
            AsyncImage(
                model = avatar,
                contentDescription = me?.name,
                modifier = Modifier.size(24.dp).clip(CircleShape),
                contentScale = ContentScale.Crop,
            )
            Spacer(Modifier.width(7.dp))
        }
        Text(
            me?.name.orEmpty(),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
