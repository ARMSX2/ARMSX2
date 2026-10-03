package com.armsx2.ui.textures

import android.graphics.Bitmap
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.armsx2.R
import com.armsx2.TextureCatalog
import com.armsx2.TexturePackInstallState.InstallAction
import com.armsx2.TexturePackLinks
import com.armsx2.i18n.str
import com.armsx2.ui.common.PadModal
import com.armsx2.ui.settings.controllerFocusable

/**
 * A texture pack's window, opened by its Get button. A banner in its creator's colours says what the
 * pack is; a row of tiles in the shape of the save state ones, larger, holds the creator (their
 * profile picture), the pack's source, and the creator's tip and socials pages; then the pack's
 * description, with Download last.
 *
 * The creator's colour is picked from their picture once it loads, and from their name until then
 * (and for good when they have none), so every creator's window looks like theirs. Depth comes from
 * gradients and borders only: no shadows and no blur, which some Adreno and Mali drivers draw as
 * solid boxes (see controllerFocusable).
 */
@Composable
internal fun PackWindow(
    pack: TextureCatalog.Pack,
    links: TexturePackLinks.Links?,
    creator: String,
    sizeText: String,
    action: InstallAction,
    anyBusy: Boolean,
    openUrl: (String) -> Unit,
    onDownload: () -> Unit,
    onClose: () -> Unit,
) {
    val layer = "tex-pack"
    val bodyScroll = rememberScrollState()
    // One install at a time, as before; and an installed pack that is up to date has nothing to get.
    val canDownload = !anyBusy && action != InstallAction.INSTALLED
    var picked by remember(links?.avatar) { mutableStateOf<Color?>(null) }
    val accent by animateColorAsState(
        picked ?: nameColor(creator.ifEmpty { pack.name }),
        label = "packAccent",
    )
    PadModal(
        key = layer,
        onDismiss = onClose,
        initialFocusId = if (canDownload) "$layer.download" else "$layer.close",
        scrollState = bodyScroll,
    ) {
        Surface(
            modifier = Modifier
                .padding(16.dp)
                .widthIn(max = 880.dp),
            shape = RoundedCornerShape(28.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, accent.copy(alpha = 0.55f)),
            tonalElevation = 6.dp,
        ) {
            Column {
                // Banner, tiles and description scroll together, so a short landscape screen keeps
                // Download in view and the pad's Up/Down reach the rest.
                Column(Modifier.weight(1f, fill = false).verticalScroll(bodyScroll)) {
                    Banner(pack, links, creator, accent, sizeText)
                    Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 18.dp)) {
                        Tiles(
                            layer = layer,
                            pack = pack,
                            links = links,
                            creator = creator,
                            accent = accent,
                            openUrl = openUrl,
                            onPicked = { picked = it },
                        )
                        if (pack.description.isNotBlank()) {
                            Spacer(Modifier.height(16.dp))
                            Description(pack.description, accent)
                        }
                    }
                }
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(start = 20.dp, end = 20.dp, top = 16.dp, bottom = 18.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    PillButton(
                        label = str("action.close"),
                        id = "$layer.close",
                        onClick = onClose,
                        container = MaterialTheme.colorScheme.surfaceVariant,
                        content = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(10.dp))
                    PillButton(
                        label = when (action) {
                            InstallAction.INSTALLED -> str("textures.online.installed")
                            InstallAction.CONFLICT, InstallAction.UPDATE -> str("textures.online.update")
                            InstallAction.INSTALL -> str("textures.pack.download")
                        },
                        id = "$layer.download",
                        onClick = onDownload,
                        container = accent,
                        content = Color.White,
                        icon = R.drawable.ic_download,
                        enabled = canDownload,
                    )
                }
            }
        }
    }
}

/** The pack's name and facts on the creator's colours, with their picture fading in at the right. */
@Composable
private fun Banner(
    pack: TextureCatalog.Pack,
    links: TexturePackLinks.Links?,
    creator: String,
    accent: Color,
    sizeText: String,
) {
    val deep = lerp(accent, Color.Black, 0.62f)
    Box(
        Modifier
            .fillMaxWidth()
            .background(deep)
            // A glow of the creator's colour from the top left.
            .background(
                Brush.radialGradient(
                    listOf(accent.copy(alpha = 0.75f), Color.Transparent),
                    center = androidx.compose.ui.geometry.Offset(0f, 0f),
                    radius = 900f,
                ),
            ),
    ) {
        Box(Modifier.matchParentSize()) {
            val avatar = links?.avatar
            if (avatar != null) {
                // Square, at the banner's own height: the whole picture, never stretched across it.
                Box(
                    Modifier
                        .align(Alignment.CenterEnd)
                        .fillMaxHeight()
                        .aspectRatio(1f),
                ) {
                    AsyncImage(
                        model = avatarRequest(avatar),
                        contentDescription = null,
                        contentScale = ContentScale.Crop,
                        alpha = 0.75f,
                        modifier = Modifier.fillMaxSize(),
                    )
                    // Faded into the banner on its left, so it reads as part of it.
                    Box(
                        Modifier
                            .fillMaxSize()
                            .background(Brush.horizontalGradient(listOf(deep, deep.copy(alpha = 0.4f), Color.Transparent))),
                    )
                }
            } else if (creator.isNotEmpty()) {
                Text(
                    initials(creator),
                    modifier = Modifier
                        .align(Alignment.CenterEnd)
                        .padding(end = 24.dp),
                    color = Color.White.copy(alpha = 0.10f),
                    fontSize = 92.sp,
                    fontWeight = FontWeight.Black,
                    maxLines = 1,
                )
            }
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(2.dp)
                    .background(Brush.horizontalGradient(listOf(accent, accent.copy(alpha = 0f)))),
            )
        }
        Column(
            Modifier
                .fillMaxWidth(0.74f)
                .padding(start = 22.dp, end = 8.dp, top = 20.dp, bottom = 20.dp),
        ) {
            Text(
                pack.name,
                color = Color.White,
                fontSize = 22.sp,
                lineHeight = 27.sp,
                fontWeight = FontWeight.ExtraBold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                listOf(pack.gameTitle, pack.serials.joinToString(", ")).filter { it.isNotBlank() }.joinToString(" · "),
                color = Color.White.copy(alpha = 0.78f),
                fontSize = 13.sp,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(14.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                links?.type?.let { Fact(str("textures.pack.type"), typeLabel(it)) }
                links?.status?.let { Fact(str("textures.pack.status"), statusLabel(it), statusColor(it)) }
                Fact(str("textures.pack.size"), sizeText)
            }
        }
    }
}

/** One fact on the banner: a small caption over its value, on frosted glass. */
@Composable
private fun Fact(label: String, value: String, dot: Color? = null) {
    Column(
        Modifier
            .clip(RoundedCornerShape(14.dp))
            .background(Color.White.copy(alpha = 0.12f))
            .border(1.dp, Color.White.copy(alpha = 0.16f), RoundedCornerShape(14.dp))
            .padding(horizontal = 12.dp, vertical = 7.dp),
    ) {
        Text(label.uppercase(), color = Color.White.copy(alpha = 0.62f), fontSize = 10.sp,
            fontWeight = FontWeight.SemiBold, letterSpacing = 0.8.sp, maxLines = 1)
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (dot != null) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
                Spacer(Modifier.width(6.dp))
            }
            Text(value, color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
        }
    }
}

/** The creator, the pack's source, and the creator's tip and socials pages, as one row of tiles that
 *  wraps two to a row on a narrow screen. */
@Composable
private fun Tiles(
    layer: String,
    pack: TextureCatalog.Pack,
    links: TexturePackLinks.Links?,
    creator: String,
    accent: Color,
    openUrl: (String) -> Unit,
    onPicked: (Color) -> Unit,
) {
    val source = links?.source ?: pack.sourceUrl.ifEmpty { null }
    val tip = links?.tip
    val socials = links?.socials
    val count = 1 + listOfNotNull(source, tip, socials).size
    // Larger than a save state tile where there is room; smaller on a short landscape screen, so the
    // banner, the tiles and Download still fit without scrolling.
    val cap = if (LocalConfiguration.current.screenHeightDp < 600) 160.dp else 200.dp
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val gap = 12.dp
        var size = (maxWidth - gap * (count - 1)) / count
        if (size < 124.dp) size = (maxWidth - gap) / 2
        size = minOf(size, cap)
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(gap),
            verticalArrangement = Arrangement.spacedBy(gap),
        ) {
            CreatorTile(
                id = "$layer.creator",
                size = size,
                name = creator,
                avatar = links?.avatar,
                accent = accent,
                onPicked = onPicked,
                onClick = links?.creatorPage?.let { page -> { openUrl(page) } },
            )
            source?.let {
                LinkTile("$layer.source", size, R.drawable.ic_link_out, str("textures.online.source"), it,
                    sourceColors(it), openUrl)
            }
            tip?.let {
                LinkTile("$layer.tip", size, R.drawable.ic_heart, str("textures.pack.tip"), it,
                    listOf(Color(0xFFD83A72), Color(0xFFFF9B54)), openUrl)
            }
            socials?.let {
                LinkTile("$layer.socials", size, R.drawable.ic_people, str("textures.pack.socials"), it,
                    socialsColors(it), openUrl)
            }
        }
    }
}

@Composable
private fun CreatorTile(
    id: String,
    size: Dp,
    name: String,
    avatar: String?,
    accent: Color,
    onPicked: (Color) -> Unit,
    onClick: (() -> Unit)?,
) {
    TileFrame(id = id, size = size, borderColor = accent.copy(alpha = 0.9f), onClick = onClick) {
        // Initials first: they stay if the picture is missing or will not load.
        Box(
            Modifier
                .fillMaxSize()
                .background(Brush.linearGradient(listOf(lerp(accent, Color.White, 0.18f), lerp(accent, Color.Black, 0.5f)))),
        )
        val initialsSize = with(LocalDensity.current) { (size * 0.3f).toSp() }
        Text(
            initials(name.ifEmpty { "?" }),
            modifier = Modifier
                .align(Alignment.Center)
                .padding(bottom = 22.dp),
            color = Color.White.copy(alpha = 0.92f),
            fontSize = initialsSize,
            fontWeight = FontWeight.Black,
            maxLines = 1,
        )
        if (avatar != null) {
            AsyncImage(
                model = avatarRequest(avatar),
                contentDescription = name,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize(),
                onSuccess = { state -> accentOf(state.result.drawable)?.let(onPicked) },
            )
        }
        TileLabel(
            title = name.ifEmpty { str("textures.creators.unknown") },
            subtitle = str("textures.pack.creator") + if (onClick != null) " ↗" else "",
        )
    }
}

@Composable
private fun LinkTile(
    id: String,
    size: Dp,
    icon: Int,
    label: String,
    url: String,
    colors: List<Color>,
    openUrl: (String) -> Unit,
) {
    TileFrame(id = id, size = size, borderColor = Color.White.copy(alpha = 0.14f), onClick = { openUrl(url) }) {
        Box(Modifier.fillMaxSize().background(Brush.linearGradient(colors)))
        // A soft disc of light in the corner, for depth without a shadow.
        Box(
            Modifier
                .size(size)
                .offset(x = size * 0.42f, y = -size * 0.46f)
                .clip(CircleShape)
                .background(Color.White.copy(alpha = 0.10f)),
        )
        Image(
            painterResource(icon),
            contentDescription = null,
            colorFilter = ColorFilter.tint(Color.White),
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(16.dp)
                .size(size * 0.26f),
        )
        TileLabel(title = label, subtitle = host(url))
    }
}

@Composable
internal fun TileFrame(
    id: String,
    size: Dp,
    borderColor: Color,
    onClick: (() -> Unit)?,
    content: @Composable BoxScope.() -> Unit,
) {
    val shape = RoundedCornerShape(20.dp)
    Box(
        Modifier
            .size(size)
            // Only a tile that does something is a stop for the pad.
            .controllerFocusable(if (onClick != null) id else null, shape, onConfirm = onClick)
            .clip(shape)
            .border(1.dp, borderColor, shape)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier),
        content = content,
    )
}

/** A tile's name over a dark fade, as on the save state tiles. */
@Composable
internal fun BoxScope.TileLabel(title: String, subtitle: String) {
    Box(
        Modifier
            .align(Alignment.BottomStart)
            .fillMaxWidth()
            .background(Brush.verticalGradient(0f to Color.Transparent, 1f to Color.Black.copy(alpha = 0.82f)))
            .padding(start = 12.dp, end = 12.dp, top = 20.dp, bottom = 10.dp),
    ) {
        Column {
            Text(title, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Bold,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            Spacer(Modifier.height(2.dp))
            Text(subtitle, color = Color(0xFFCFE0FF), fontSize = 11.5.sp,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun Description(text: String, accent: Color) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .clip(RoundedCornerShape(18.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f)),
    ) {
        Box(Modifier.width(4.dp).fillMaxHeight().background(accent))
        Text(
            text,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun PillButton(
    label: String,
    id: String,
    onClick: () -> Unit,
    container: Color,
    content: Color,
    icon: Int? = null,
    enabled: Boolean = true,
) {
    val shape = RoundedCornerShape(16.dp)
    Surface(
        onClick = onClick,
        enabled = enabled,
        // A button that cannot act is not a stop for the pad either.
        modifier = if (enabled) Modifier.controllerFocusable(controllerId = id, shape = shape, onConfirm = onClick)
        else Modifier,
        shape = shape,
        color = if (enabled) container else container.copy(alpha = 0.35f),
    ) {
        Row(
            Modifier.padding(horizontal = 20.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val tint = if (enabled) content else content.copy(alpha = 0.55f)
            if (icon != null) {
                Image(painterResource(icon), contentDescription = null, colorFilter = ColorFilter.tint(tint),
                    modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
            }
            Text(label, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold, color = tint)
        }
    }
}

/** The archive's texture types, in the app's language; one it adds later shows as it words it. */
@Composable
private fun typeLabel(raw: String): String = when (raw.lowercase()) {
    "ai upscale" -> str("textures.pack.type.aiUpscale")
    "handcrafted" -> str("textures.pack.type.handcrafted")
    "mixed" -> str("textures.pack.type.mixed")
    "port" -> str("textures.pack.type.port")
    "button replacement" -> str("textures.pack.type.buttons")
    else -> raw
}

/** The archive's pack statuses, in the app's language; one it adds later shows as it words it. */
@Composable
private fun statusLabel(raw: String): String = when (raw.lowercase()) {
    "complete" -> str("textures.pack.status.complete")
    "in-progress" -> str("textures.pack.status.inProgress")
    "incomplete" -> str("textures.pack.status.incomplete")
    "partial" -> str("textures.pack.status.partial")
    else -> raw
}

private fun statusColor(raw: String): Color = when (raw.lowercase()) {
    "complete" -> Color(0xFF5BE49B)
    "in-progress" -> Color(0xFFFFC857)
    "incomplete" -> Color(0xFFFF8A5B)
    "partial" -> Color(0xFF7FB3FF)
    else -> Color.White.copy(alpha = 0.6f)
}

private fun host(url: String): String =
    runCatching { java.net.URI(url).host.orEmpty().removePrefix("www.") }.getOrDefault("")

/** Each place a pack can come from in its own colours: plain gradients, no one's logo. */
private fun sourceColors(url: String): List<Color> {
    val h = host(url)
    return when {
        h.endsWith("gbatemp.net") -> listOf(Color(0xFF1B4FA8), Color(0xFF4F8EF7))
        h.endsWith("patreon.com") -> listOf(Color(0xFFC92F45), Color(0xFFFF7A59))
        h.endsWith("mediafire.com") -> listOf(Color(0xFF0D6FC4), Color(0xFF4DB5FF))
        h.endsWith("github.com") -> listOf(Color(0xFF1F2328), Color(0xFF5A6470))
        h.endsWith("archive.org") -> listOf(Color(0xFF3A3A3A), Color(0xFF8C8C8C))
        h.endsWith("moddb.com") -> listOf(Color(0xFF8E1B1B), Color(0xFFD9483B))
        else -> listOf(Color(0xFF16736B), Color(0xFF43C2AE))
    }
}

private fun socialsColors(url: String): List<Color> {
    val h = host(url)
    return if (h.endsWith("youtube.com") || h == "youtu.be") listOf(Color(0xFFB3221C), Color(0xFFFF5A4E))
    else listOf(Color(0xFF5B3FE0), Color(0xFF43A8FF))
}

@Composable
internal fun avatarRequest(url: String): ImageRequest {
    val context = LocalContext.current
    // Software bitmaps, so the creator's colour can be read out of the picture.
    return remember(url) { ImageRequest.Builder(context).data(url).allowHardware(false).crossfade(true).build() }
}

/** "Panda_Venom" -> "PV", "Bl4ckH4nd" -> "BH", "mvp899" -> "M". */
internal fun initials(name: String): String {
    val parts = name.split(Regex("[\\s_.|,-]+")).filter { it.isNotEmpty() }
    val letters = when {
        parts.isEmpty() -> "?"
        parts.size >= 2 -> "${parts[0].first()}${parts[1].first()}"
        else -> parts[0].first().toString() + (parts[0].drop(1).firstOrNull { it.isUpperCase() }?.toString() ?: "")
    }
    return letters.uppercase()
}

/** A creator's colour from their name alone: stable, and different from one creator to the next. */
internal fun nameColor(name: String): Color =
    readable(Color.hsl(((name.hashCode() % 360 + 360) % 360).toFloat(), 0.55f, 0.47f))

/** Darkened until white text on it reads well: a yellow or a lime would not, as picked. */
internal fun readable(color: Color): Color {
    var c = color
    repeat(8) { if (c.luminance() > 0.3f) c = lerp(c, Color.Black, 0.12f) }
    return c
}

/** The picture is read at this many pixels a side for its colour. */
private const val SAMPLE = 24

/**
 * The creator's colour from their picture: its vivid pixels averaged, then kept bright enough to carry
 * white text. A picture with almost no colour in it (a grey photo) gives null, and the name colour
 * stays, rather than a grey stretched into an arbitrary hue.
 */
private fun accentOf(drawable: Drawable): Color? {
    val bitmap = (drawable as? BitmapDrawable)?.bitmap ?: return null
    if (bitmap.config == Bitmap.Config.HARDWARE) return null
    val small = Bitmap.createScaledBitmap(bitmap, SAMPLE, SAMPLE, true)
    val hsv = FloatArray(3)
    var r = 0.0
    var g = 0.0
    var b = 0.0
    var weight = 0.0
    for (y in 0 until SAMPLE) for (x in 0 until SAMPLE) {
        val c = small.getPixel(x, y)
        android.graphics.Color.colorToHSV(c, hsv)
        val w = (hsv[1] * hsv[2]).toDouble().let { it * it }
        r += android.graphics.Color.red(c) * w
        g += android.graphics.Color.green(c) * w
        b += android.graphics.Color.blue(c) * w
        weight += w
    }
    if (small !== bitmap) small.recycle()
    if (weight / (SAMPLE * SAMPLE) < 0.02) return null
    val mixed = android.graphics.Color.rgb((r / weight).toInt(), (g / weight).toInt(), (b / weight).toInt())
    android.graphics.Color.colorToHSV(mixed, hsv)
    return readable(Color.hsv(hsv[0], hsv[1].coerceIn(0.45f, 0.85f), hsv[2].coerceIn(0.55f, 0.78f)))
}
