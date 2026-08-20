// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu expand: indices + a palette -> the colour image a paletted draw samples. Second stage of
// rule 3's paletted road. tilegpu_materialise.glsl wrote the window's palette INDICES into an image
// (the palette is deliberately not in the source cache's key, so one index build serves every
// palette a game cycles through it); this turns one such image, times one palette, into the RGBA8
// the hardware sampler reads.
//
// ⚠️ This is GSDevice::TileExpandPalette's pass in the TileGpu idiom, and the arithmetic below is
// ps_tile_expand_palette's (tile_convert.glsl) unchanged -- same index decode, same unfiltered
// palette fetch. It exists as a separate program only because of HOW it must be issued, not what it
// computes: the Tile renderer draws immediately and can call the device's utility path, while this
// renderer records draws and executes them at VSync, so the expansion has to be one more op in the
// stream it will eventually run, recorded with raw commands beside the materialise and the seed.
// Calling the utility path from inside that recording was tried and is wrong: it binds descriptors
// through the device's own state cache and its own pipeline layout, which disturbs the set bindings
// the executor established for the passes around it (measured: 20 validation errors on one SotC
// frame, from draws whose set 0 and set 1 the expand's push had invalidated).
//
// A full-viewport triangle from the vertex index, no attributes, no varyings -- the fragment stage
// addresses by gl_FragCoord, so the two stages share no interface at all. The destination is an
// RGBA8 render target the size of the index image, one fragment per texel.

#ifdef VERTEX_SHADER

void main()
{
	// Full-viewport triangle from the vertex index; no attributes.
	const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D samp_index;   // the materialised index image
layout(set = 0, binding = 1) uniform sampler2D samp_palette; // the palette, N x 1

void main()
{
	const ivec2 xy = ivec2(gl_FragCoord.xy);
	// The UNORM byte back to an integer, exactly as ps_tile_expand_palette does it: the materialise
	// wrote float(index)/255 into every channel, and .a is the one both conventions agree on (an R8
	// view of a one-byte source replicates the byte, and a reinterpreted RGBA source carries it
	// there). k/255 * 255.5 lands inside [k, k+1), so the truncation recovers k from either of the
	// two unorm->float conversions a texture unit is allowed to use.
	const uint idx = uint(texelFetch(samp_index, xy, 0).a * 255.5f);
	// Unfiltered, and clamped only so a fetch stays in bounds if a caller ever paired a 4-bit index
	// image with a 16-entry palette it does not belong to.
	const ivec2 pxy = clamp(ivec2(int(idx), 0), ivec2(0), textureSize(samp_palette, 0) - 1);
	// Straight through: the palette texel IS the CLUT word the byte road reads out of the frame's
	// palette stream, byte for byte (both come from the same GSClut::Read32 expansion), so a draw
	// moving from the byte road to this image lands on the same colour. No arithmetic here at all,
	// which is also why the header's fma-pinning rule has nothing to bind.
	o_color = texelFetch(samp_palette, pxy, 0);
}

#endif
