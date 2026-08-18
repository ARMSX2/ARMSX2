#version 450
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Written for ARMSX2 rather than lifted from slang-shaders so the bundled tracer carries
// no licence question. It is deliberately boring: it exists to prove the Metal handoff,
// not to look good.

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
	vec4 SourceSize;
	vec4 OutputSize;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
	vec3 colour = texture(Source, vTexCoord).rgb;
	// Rows are counted in the render target, not the source, so a wrong viewport shows up
	// as banding at the wrong pitch instead of passing silently.
	float odd_row = mod(floor(gl_FragCoord.y), 2.0);
	FragColor = vec4(colour * mix(1.0, 0.45, odd_row), 1.0);
}
