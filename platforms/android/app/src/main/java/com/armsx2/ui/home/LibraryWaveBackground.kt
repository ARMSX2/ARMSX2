package com.armsx2.ui.home

import androidx.compose.animation.core.withInfiniteAnimationFrameNanos
import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import com.armsx2.ui.theme.LibraryBackgroundColorPreferences
import kotlin.math.PI
import kotlin.math.sin

/**
 * Animated library background for devices where the GLES3 [XmbGlView] can't run — older Mali
 * without float-texture filtering, or any EGL failure. It used to fall back to a fixed looping GIF
 * (R.raw.library_fallback) that ignored the colour picker entirely, so Mali users had a background
 * they couldn't recolour. This is a PPSSPP-style procedural background instead: soft flowing waves
 * plus a few drifting PlayStation glyphs, drawn with the hardware 2D Canvas (Skia) rather than GLES3
 * — so it runs on ANY GPU, and it reads the SAME [LibraryBackgroundColorPreferences] the GL wave
 * does, so the colour swatches and the RGB hue-cycle finally do something on a Mali device.
 *
 * Deliberately cheap: one vertical gradient, four stroked sine paths, and ten small glyphs per
 * frame — a few hundred segments, nothing a weak tiler struggles with. No textures, no blur, no
 * offscreen passes. The readability scrim is applied by HomeScreen on top, same as for the GL wave.
 */
@Composable
fun LibraryWaveBackground(modifier: Modifier = Modifier) {
    // Colour picker + RGB toggle, read as Compose state so a live change recolours immediately.
    val colorArgb by LibraryBackgroundColorPreferences.color
    val rgbCycle by LibraryBackgroundColorPreferences.rgbCycle

    // Elapsed seconds, ticked once per frame. Driven off the animation clock (not a recomposition
    // loop) so only the Canvas draw re-runs each frame, not the whole tree.
    val timeSec = remember { mutableFloatStateOf(0f) }
    LaunchedEffect(Unit) {
        var start = 0L
        withInfiniteAnimationFrameNanos { start = it }
        while (true) {
            withInfiniteAnimationFrameNanos { now ->
                timeSec.floatValue = (now - start) / 1_000_000_000f
            }
        }
    }

    Canvas(modifier) {
        val t = timeSec.floatValue
        // Base colour: the RGB cycle sweeps the hue wheel (~28s/turn, matching the GL peripheral
        // vibe); otherwise the picked colour, or the built-in royal blue when unset.
        val base: Color = when {
            rgbCycle -> Color.hsv(((t / 28f) * 360f) % 360f, 0.72f, 0.96f)
            colorArgb == 0 -> DEFAULT_WAVE
            else -> Color(colorArgb)
        }
        drawWaveScene(t, base)
    }
}

/** The built-in wave colour — matches XmbGlView.BG_BOT / LibraryBackgroundColorPreferences default. */
private val DEFAULT_WAVE = Color(0xFF2E75F5)

private fun DrawScope.drawWaveScene(t: Float, base: Color) {
    val w = size.width
    val h = size.height

    // 1) Vertical gradient — near-black anchor at the top (where the content/grid sits), deepening
    //    to the chosen colour at the bottom. Same 0.20 top/bottom ratio the GL path uses.
    drawRect(
        Brush.verticalGradient(
            0.0f to base.scaleRgb(0.10f),
            0.55f to base.scaleRgb(0.35f),
            1.0f to base.scaleRgb(0.85f),
        ),
    )

    // 2) Soft flowing wave BANDS — filled translucent sheets that glow just under the crest and
    //    fade downward, layered back-to-front. Reads as flowing light, not thin squiggly lines.
    val samples = 64
    val step = w / samples
    val twoPi = 2f * PI.toFloat()
    for (layer in 0 until WAVE_LAYERS) {
        val f = layer.toFloat() / (WAVE_LAYERS - 1)
        val baseY = h * (0.40f + 0.16f * f)                 // deeper layers sit lower
        val amp = h * (0.05f + 0.028f * (1f - f))           // gentle undulation
        val len = 1.05f + 0.5f * f
        val speed = 0.26f + 0.14f * f
        val phase = t * speed + layer * 2.2f
        fun waveY(nx: Float): Float =
            baseY + amp * sin(nx * len * twoPi + phase) +
                amp * 0.34f * sin(nx * len * 2.1f * twoPi - phase * 1.35f + layer)
        // Filled sheet from the crest curve down past the bottom edge.
        val body = Path().apply {
            moveTo(0f, h + 2f)
            var i = 0
            while (i <= samples) { lineTo(i * step, waveY(i.toFloat() / samples)); i++ }
            lineTo(w, h + 2f)
            close()
        }
        val tint = base.lighten(0.30f + 0.22f * f)
        drawPath(
            path = body,
            brush = Brush.verticalGradient(
                0.00f to tint.copy(alpha = 0f),
                0.05f to tint.copy(alpha = 0.10f + 0.05f * f),   // soft glow under the crest
                0.55f to base.scaleRgb(1.06f).copy(alpha = 0.04f + 0.03f * f),
                1.00f to base.scaleRgb(0.75f).copy(alpha = 0f),
                startY = baseY - amp * 1.6f,
                endY = h,
            ),
        )
        // Faint, wide, soft crest — enough to define the wave without reading as a hard line.
        val crest = Path().apply {
            var i = 0
            while (i <= samples) {
                val x = i * step; val y = waveY(i.toFloat() / samples)
                if (i == 0) moveTo(x, y) else lineTo(x, y); i++
            }
        }
        drawPath(
            path = crest,
            color = base.lighten(0.62f).copy(alpha = 0.07f + 0.07f * f),
            style = Stroke(width = size.minDimension * (0.008f + 0.004f * f), cap = StrokeCap.Round),
        )
    }

    // 3) Drifting PlayStation glyphs — a faint, slow parallax layer, the PPSSPP "floating symbols"
    //    flavour. Fixed pseudo-random spots (deterministic, no RNG per frame) rising and looping.
    val glyphColor = base.lighten(0.6f).copy(alpha = 0.06f)
    for (i in GLYPH_SPOTS.indices) {
        val (sx, sy, kind, scale) = GLYPH_SPOTS[i]
        val drift = (t * (0.012f + 0.006f * (i % 3))) + sy
        val y = h * (1.1f - (drift % 1.2f))                 // rise from below, loop past the top
        val x = w * ((sx + 0.02f * sin(t * 0.2f + i)) % 1f)
        val r = size.minDimension * 0.04f * scale
        drawGlyph(kind, Offset(x, y), r, glyphColor, t + i)
    }
}

private const val WAVE_LAYERS = 4

/** (xFrac, yPhase, kind 0..3 = △○✕□, scale). Deterministic so nothing allocates per frame. */
private val GLYPH_SPOTS: List<Glyph> = listOf(
    Glyph(0.10f, 0.05f, 0, 1.1f), Glyph(0.24f, 0.55f, 1, 0.8f), Glyph(0.38f, 0.30f, 2, 1.0f),
    Glyph(0.52f, 0.80f, 3, 0.9f), Glyph(0.63f, 0.15f, 1, 1.2f), Glyph(0.71f, 0.62f, 0, 0.75f),
    Glyph(0.82f, 0.40f, 3, 1.05f), Glyph(0.90f, 0.90f, 2, 0.85f), Glyph(0.46f, 0.05f, 0, 0.7f),
    Glyph(0.16f, 0.72f, 3, 0.95f),
)

private data class Glyph(val x: Float, val y: Float, val kind: Int, val scale: Float)

private fun DrawScope.drawGlyph(kind: Int, c: Offset, r: Float, color: Color, spin: Float) {
    val stroke = Stroke(width = r * 0.14f, cap = StrokeCap.Round)
    when (kind) {
        1 -> drawCircle(color, radius = r * 0.82f, center = c, style = stroke)      // ○
        3 -> rotate(spin * 6f, pivot = c) {                                          // □
            val s = r * 1.35f
            drawRect(color, topLeft = Offset(c.x - s / 2, c.y - s / 2),
                size = androidx.compose.ui.geometry.Size(s, s), style = stroke)
        }
        2 -> {                                                                       // ✕
            val s = r * 0.7f
            drawLine(color, Offset(c.x - s, c.y - s), Offset(c.x + s, c.y + s), strokeWidth = r * 0.16f, cap = StrokeCap.Round)
            drawLine(color, Offset(c.x - s, c.y + s), Offset(c.x + s, c.y - s), strokeWidth = r * 0.16f, cap = StrokeCap.Round)
        }
        else -> {                                                                    // △
            val p = Path().apply {
                moveTo(c.x, c.y - r)
                lineTo(c.x + r * 0.87f, c.y + r * 0.5f)
                lineTo(c.x - r * 0.87f, c.y + r * 0.5f)
                close()
            }
            drawPath(p, color, style = stroke)
        }
    }
}

// ---- small colour helpers -------------------------------------------------
/** Scale RGB by [v] (toward black for v<1, a touch brighter for v>1), clamped to valid range. */
private fun Color.scaleRgb(v: Float) = Color(
    (red * v).coerceIn(0f, 1f), (green * v).coerceIn(0f, 1f), (blue * v).coerceIn(0f, 1f), alpha,
)
/** Blend toward white by [amount] — the ribbon/glyph highlight tint. */
private fun Color.lighten(amount: Float) = Color(
    red + (1f - red) * amount, green + (1f - green) * amount, blue + (1f - blue) * amount, alpha,
)
