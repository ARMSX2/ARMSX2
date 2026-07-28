package com.armsx2.ui.news

import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.News
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.controllerFocusable

/**
 * Release notes, newest first.
 *
 * A plain Column rather than a LazyColumn on purpose: controllerFocusable only registers items that
 * are actually composed, so a lazy list hands the pad a nav registry with holes in it. Twenty
 * releases of text is nothing to measure.
 */
@Composable
fun NewsScreen(onBack: () -> Unit, viewModel: NewsViewModel = viewModel()) {
    val state = viewModel.state.value
    LaunchedEffect(Unit) { viewModel.load() }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = str("news.title"),
                leading = { RoundAction("←", str("action.back"), onBack) },
                actions = {
                    // Named, not a trailing lambda: RoundAction's last parameter is glyphColor, so
                    // a trailing lambda binds there rather than to onClick.
                    RoundAction("⟳", str("news.refresh"), onClick = { viewModel.load(force = true) })
                },
            )

            Column(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                when {
                    state.loading && state.items.isEmpty() -> GlassPanel(Modifier.fillMaxWidth()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Spacer(Modifier.size(10.dp))
                            Text(str("news.loading"), style = MaterialTheme.typography.bodySmall)
                        }
                    }

                    state.items.isEmpty() -> GlassPanel(Modifier.fillMaxWidth()) {
                        Text(str("news.unavailable"), style = MaterialTheme.typography.bodySmall)
                    }

                    else -> {
                        // Say so rather than presenting stale notes as live — same rule the texture
                        // catalogue follows when its mirrors are unreachable.
                        if (state.fromCache) {
                            GlassPanel(Modifier.fillMaxWidth()) {
                                Text(str("news.offline"), style = MaterialTheme.typography.bodySmall)
                            }
                        }
                        // Newest open, the rest collapsed: the page should be scannable, not a wall
                        // of every changelog we have ever shipped.
                        state.items.forEachIndexed { index, item ->
                            ReleaseCard(item, initiallyExpanded = index == 0)
                        }
                    }
                }
                Spacer(Modifier.height(12.dp))
            }
        }
    }
}

@Composable
private fun ReleaseCard(item: News.Item, initiallyExpanded: Boolean) {
    // Keyed on the tag so it survives rotation.
    var expanded by rememberSaveable(item.tag) { mutableStateOf(initiallyExpanded) }
    val toggle = { expanded = !expanded }

    GlassPanel(Modifier.fillMaxWidth()) {
        Column(
            Modifier
                .fillMaxWidth()
                // clickable AND controllerFocusable: the latter only wires the pad's confirm
                // button into the nav registry, it does not make anything respond to a finger.
                // Shipping only the focusable left "Show more" dead to touch, which is how every
                // phone user meets this screen.
                .clickable(onClick = toggle)
                .controllerFocusable("news.${item.tag}", onConfirm = toggle),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.padding(end = 8.dp)) {
                    Text(
                        item.title,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        buildString {
                            append(item.tag)
                            if (item.published.isNotBlank()) append(" · ").append(item.published)
                        },
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (item.prerelease) {
                    Surface(
                        shape = RoundedCornerShape(6.dp),
                        color = MaterialTheme.colorScheme.secondaryContainer,
                    ) {
                        Text(
                            str("news.prerelease"),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSecondaryContainer,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                        )
                    }
                }
            }

            if (item.notes.isBlank()) {
                Text(str("news.noNotes"), style = MaterialTheme.typography.bodySmall)
            } else {
                Text(
                    item.notes,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = if (expanded) Int.MAX_VALUE else 6,
                )
                Text(
                    str(if (expanded) "news.showLess" else "news.showMore"),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary,
                    // Padded so it is a real touch target rather than a line of 11sp text, and
                    // clickable in its own right even though the whole card toggles too — it looks
                    // like a link, so it has to behave like one.
                    modifier = Modifier
                        .clickable(onClick = toggle)
                        .padding(vertical = 4.dp),
                )
            }
        }
    }
}
