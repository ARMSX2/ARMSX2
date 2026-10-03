// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.common

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit

/**
 * [picture] drawn in [glyph]'s place and box: the glyph is laid out but not drawn, and the picture
 * is fitted to it. A row with a picture then lines up with the rows beside it that show an emoji at
 * the same size, on any font, which a fixed dp size cannot promise.
 */
@Composable
fun GlyphSizedImage(glyph: String, fontSize: TextUnit, picture: Painter, fontWeight: FontWeight? = null) {
    Box {
        Text(glyph, fontSize = fontSize, fontWeight = fontWeight, modifier = Modifier.alpha(0f).clearAndSetSemantics {})
        Image(picture, contentDescription = null, modifier = Modifier.matchParentSize())
    }
}
