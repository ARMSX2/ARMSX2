// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu executor's minimal wrong-fast geometry shader: transform a raw GSVertex into
// clip space using the per-draw screen->NDC transform (the HW tfx VertexScale/VertexOffset)
// and pass its colour straight through. No texturing, blending, or fog yet -- those arrive with
// the fixed-function state in 1.4. The transform mirrors tfx.glsl's vertex main exactly, so a
// draw lands at the same pixels the shipping renderer would put it.
//
// The per-draw transform is NOT a push constant: the executor submits with
// vkCmdDrawIndexedIndirect, so there is no per-draw command to push against. Instead every draw's
// state row lives in a storage buffer, and the indirect draw's first_instance selects it --
// first_instance arrives as gl_InstanceIndex (instanceCount is 1, so gl_InstanceID is 0). A
// single push constant, base_row, rebases into this frame's slice of the ring buffer.

#ifdef VERTEX_SHADER

layout(location = 0) in uvec2 a_xy;   // raw 12.4 fixed-point screen XY
layout(location = 1) in uint a_z;     // raw integer depth (24- or 32-bit)
layout(location = 2) in vec4 a_color; // RGBA, normalised

layout(location = 0) out vec4 v_color;

// Matches the executor's StateRow byte-for-byte (std430, 32 bytes). Only the transform is read
// here; the z enables are pipeline state, carried for layout parity, not consumed.
struct StateRow
{
	vec2 vertex_scale;
	vec2 vertex_offset;
	uint z_write;
	uint z_test;
	uint pad0;
	uint pad1;
};

layout(std430, set = 0, binding = 0) readonly buffer StateTable
{
	StateRow state_rows[];
};

layout(push_constant) uniform cb
{
	uint base_row; // this frame's first state row in the ring buffer
};

void main()
{
	StateRow sr = state_rows[base_row + uint(gl_InstanceIndex)];

	vec2 p = vec2(a_xy);
	gl_Position = vec4(p, float(a_z), 1.0f) - vec4(0.05f, 0.05f, 0.0f, 0.0f);
	gl_Position.xy = gl_Position.xy * vec2(sr.vertex_scale.x, -sr.vertex_scale.y) - vec2(sr.vertex_offset.x, -sr.vertex_offset.y);
	gl_Position.z *= exp2(-32.0f); // integer depth -> float, monotonic; D32_SFLOAT keeps precision
	gl_Position.y = -gl_Position.y;
	if (gl_Position.z == gl_Position.w)
		gl_Position.z *= 0.999999f;
	v_color = a_color;
	// Point topology reads gl_PointSize; a GS point covers one pixel. Ignored for line/triangle.
	gl_PointSize = 1.0f;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	// PS2 vertex colour is 8-bit with 128 as the 1.0 reference (the modulation unit); untextured
	// here, so scale it into display range and force opaque. The 2x is the 255/128 convention,
	// approximated -- brightness only, exactness is a 1.4 concern once the texture function lands.
	o_color = vec4(v_color.rgb * 2.0f, 1.0f);
}

#endif
