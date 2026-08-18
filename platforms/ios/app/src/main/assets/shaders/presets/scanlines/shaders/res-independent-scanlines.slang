#version 450

/*
    Resolution-Independent Scanlines
    based on
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
	float mask, mask_weight, fauxRes, autoscale;
} params;

#pragma parameter amp          "Amplitude"      1.2500  0.000 2.000 0.05
#pragma parameter phase        "Phase"          0.5000  0.000 2.000 0.05
#pragma parameter lines_black  "Lines Blacks"   0.0000  0.000 1.000 0.05
#pragma parameter lines_white  "Lines Whites"   1.0000  0.000 2.000 0.05

#pragma parameter mask "Mask Layout" 0.0 0.0 19.0 1.0
#pragma parameter mask_weight "Mask Weight" 0.5 0.0 1.0 0.01
#pragma parameter fauxRes "Simulated Image Height" 224.0 144.0 288.0 1.0
#pragma parameter autoscale "Automatic Scale" 0.0 0.0 1.0 1.0

#define freq             0.500000
#define offset           0.000000
#define pi               3.141592654

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#define pi 3.141592654
#include "../../include/subpixel_masks.h"

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out float omega;

void main()
{
   gl_Position = global.MVP * Position;
   vTexCoord = TexCoord;
   
   omega = 2.0 * pi * freq;              // Angular frequency
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in float omega;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    float scale = params.fauxRes; if (params.autoscale == 1.0) scale = params.OriginalSize.y; 
    float angle = (gl_FragCoord.y * params.OutputSize.w) * omega * scale + params.phase;
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
   FragColor.rgb *= mask_weights(gl_FragCoord.xy, params.mask_weight, int(params.mask));
}

