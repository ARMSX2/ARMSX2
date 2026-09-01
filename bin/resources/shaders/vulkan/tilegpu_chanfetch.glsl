// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu channel fetch: the HLE substitute for a palette cycle (channel shuffle).
//
// A family of games splits a frame buffer into its channels by re-reading it as a PSMT8 texture
// through a CLUT the game renders itself, applies a ramp to each channel, and puts the pieces back.
// Gran Turismo 4 spends 85% of a frame doing that for screen brightness. The renderer recognises
// the run (GSTilePaletteCycleRun), drops its draws, and issues one of these instead -- which is
// exactly what Classic's GSC_PolyphonyDigitalGames does, and the arithmetic here is its
// tfx.glsl fetch_rgb / fetch_red / fetch_green / fetch_blue, unchanged:
//
//   RGB : out = (palette[src.r].r, palette[src.g].g, palette[src.b].b)   colour write mask RGB
//   R/G/B: out = palette[src.<channel>]                                  colour write mask A
//
// The source is a COPY of the destination taken by the op that issues this, not the destination
// itself: the substitute is in place (Classic's HLE draw has the same texture as render target and
// as source, and takes the framebuffer-fetch road for it), and a render pass may not sample the
// image it is writing. The copy costs one full-target blit per substitute -- one a frame on
// gt4opb -- against a run of a thousand draws, so nothing here is worth being clever about.
//
// A full-viewport triangle from the vertex index, no attributes, no varyings: the fragment stage
// addresses by gl_FragCoord and the viewport is set to the substitute's rect, so the source copy
// and the destination are sampled and written at the same pixel.

#define TILEGPU_CHANFETCH_RGB 0
#define TILEGPU_CHANFETCH_RED 1
#define TILEGPU_CHANFETCH_GREEN 2
#define TILEGPU_CHANFETCH_BLUE 3

#ifdef VERTEX_SHADER

void main()
{
	const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D samp_source;  // the destination's own pixels, copied
layout(set = 0, binding = 1) uniform sampler2D samp_palette; // the gathered palette, N x 1

layout(push_constant) uniform cb
{
	uvec4 chan; // .x = which fetch (TILEGPU_CHANFETCH_*), .yzw unused
};

// The palette entry a channel byte selects. The byte comes back from a UNORM8 fetch as k/255, which
// is not the same float as k * (1/255) -- so recover the integer the way every other road here does
// (k/255 * 255.5 lands inside [k, k+1)) rather than trusting a multiply to land on the level.
vec4 tilegpu_pal(float channel, int entries)
{
	const int idx = clamp(int(channel * 255.5f), 0, entries - 1);
	return texelFetch(samp_palette, ivec2(idx, 0), 0);
}

void main()
{
	const ivec2 xy = ivec2(gl_FragCoord.xy);
	const vec4 s = texelFetch(samp_source, xy, 0);
	const int entries = textureSize(samp_palette, 0).x;

	if (chan.x == uint(TILEGPU_CHANFETCH_RGB))
	{
		// Classic's fetch_rgb: each channel indexes the palette and keeps ITS OWN component of the
		// entry, so the three lookups are three different palette texels and one colour. Alpha is
		// not written (the pipeline masks it), and 1.0 is what tfx.glsl puts there.
		o_color = vec4(tilegpu_pal(s.r, entries).r, tilegpu_pal(s.g, entries).g, tilegpu_pal(s.b, entries).b, 1.0f);
	}
	else
	{
		// fetch_red / fetch_green / fetch_blue: ONE channel indexes the palette and the whole entry
		// is the fragment colour, of which the pipeline writes the alpha alone (Classic's
		// TFX_DECAL + TCC + colormask 8).
		const float c = (chan.x == uint(TILEGPU_CHANFETCH_RED)) ? s.r :
						((chan.x == uint(TILEGPU_CHANFETCH_GREEN)) ? s.g : s.b);
		o_color = tilegpu_pal(c, entries);
	}
}

#endif
