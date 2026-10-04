#version 450

/*
    Scanlines Sine Absolute Value
    An ultra light scanline shader
    by RiskyJumps
	license: public domain
*/

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OriginalSize;
	vec4 OutputSize;
	uint FrameCount;
	float amp;
	float phase;
	float lines_black;
	float lines_white;
} params;

#pragma parameter amp          "Amplitude"      1.2500  0.000 2.000 0.05
#pragma parameter phase        "Phase"          0.5000  0.000 2.000 0.05
#pragma parameter lines_black  "Lines Blacks"   0.0000  0.000 1.000 0.05
#pragma parameter lines_white  "Lines Whites"   1.0000  0.000 2.000 0.05

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#define freq             0.500000
#define offset           0.000000
#define pi               3.141592654

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out float angle;

void main()
{
   gl_Position = global.MVP * Position;
   vTexCoord = TexCoord;
   
   float omega = 2.0 * pi * freq;              // Angular frequency
   angle = vTexCoord.y * omega * params.SourceSize.y + params.phase;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in float angle;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    vec3 color = texture(Source, vTexCoord).xyz;
    float grid;
 
    float lines;
 
    lines = sin(angle);
    lines *= params.amp;
    lines += offset;
    lines = abs(lines);
    lines *= params.lines_white - params.lines_black;
    lines += params.lines_black;
    color *= lines;
 
    FragColor = vec4(color.xyz, 1.0);
}