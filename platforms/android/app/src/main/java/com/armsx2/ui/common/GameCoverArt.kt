package com.armsx2.ui.common

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.CustomCovers
import com.armsx2.GameInfo

@Composable
fun GameCoverArt(game: GameInfo, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val customCover = remember(game.uri, CustomCovers.version.value) {
        CustomCovers.fileFor(context, game)
    }
    // Subscribe to the Cover Region choice so a change re-resolves the URL here too — game.coverUrl
    // hides the dependency inside a plain getter, which Compose cannot see.
    com.armsx2.CoverRegionIndex.region.intValue
    com.armsx2.CoverRegionIndex.perGameGeneration.intValue
    SubcomposeAsyncImage(
        model = ImageRequest.Builder(context)
            .data(customCover ?: game.coverUrl)
            .crossfade(true)
            .build(),
        contentDescription = game.title,
        modifier = modifier.clip(RoundedCornerShape(14.dp)),
        contentScale = ContentScale.Crop,
        loading = { GameCoverPlaceholder(game.title, game.serial) },
        error = {
            // Cover Region can point at a release the art repo has no cover for; falling straight
            // to the placeholder would BLANK a cover the user already had (reported for the in-game
            // menu, which uses this component). Retry with this disc's own serial first.
            val discUrl = if (customCover == null) game.discCoverUrl else null
            if (discUrl != null && discUrl != game.coverUrl) {
                SubcomposeAsyncImage(
                    model = discUrl,
                    contentDescription = game.title,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                    loading = { GameCoverPlaceholder(game.title, game.serial) },
                    error = { GameCoverPlaceholder(game.title, game.serial) },
                )
            } else {
                GameCoverPlaceholder(game.title, game.serial)
            }
        },
    )
}

@Composable
fun GameCoverPlaceholder(title: String, serial: String? = null, modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxSize()
            .background(
                Brush.linearGradient(
                    listOf(
                        MaterialTheme.colorScheme.primaryContainer,
                        MaterialTheme.colorScheme.surfaceVariant,
                    ),
                ),
            ),
        contentAlignment = Alignment.Center,
    ) {
        Column(Modifier.padding(10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
                textAlign = TextAlign.Center,
                maxLines = 4,
                overflow = TextOverflow.Ellipsis,
            )
            if (!serial.isNullOrBlank()) {
                Text(
                    text = serial,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.68f),
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
