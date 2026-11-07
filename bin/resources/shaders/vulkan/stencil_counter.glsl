// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The stencil counter's resolve (GSDeviceVK::DrawStencilCounter, GS/Renderers/Common/GSStencilCounter.h):
// writes (Cd + step * count) mod 256 into each colour channel the pipeline's write mask enables,
// with Cd read from a copy of the target and the count from the stencil aspect, both at this pixel.

#ifdef VERTEX_SHADER

void main()
{
	// One triangle over the whole viewport; the scissor limits it to the draw area.
	const vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) out vec4 o_col0;

layout(set = 0, binding = 0) uniform sampler2D samp_target;
layout(set = 0, binding = 1) uniform usampler2D samp_count;

layout(push_constant) uniform PushConstants
{
	uvec4 step;
};

void main()
{
	const ivec2 coord = ivec2(gl_FragCoord.xy);
	const vec4 cd = texelFetch(samp_target, coord, 0);
	const uint count = texelFetch(samp_count, coord, 0).r;
	const uvec3 sum = (uvec3(cd.rgb * 255.0f + 0.5f) + step.rgb * count) & uvec3(0xFFu);
	o_col0 = vec4(vec3(sum) / 255.0f, cd.a);
}

#endif
