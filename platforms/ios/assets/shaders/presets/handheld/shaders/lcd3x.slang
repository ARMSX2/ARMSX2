#version 450

/*
   Author: Gigaherz
   License: Public domain
*/

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OriginalSize;
	vec4 OutputSize;
	uint FrameCount;
	float brighten_scanlines;
	float brighten_lcd;
} params;

#pragma parameter brighten_scanlines "Brighten Scanlines" 16.0 1.0 32.0 0.5
#define brighten_scanlines params.brighten_scanlines
#pragma parameter brighten_lcd "Brighten LCD" 4.0 1.0 12.0 0.1
#define brighten_lcd params.brighten_lcd

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

vec2 omega = vec2(3.141592654) * vec2(2.0) * params.OriginalSize.xy;
const vec3 offsets = vec3(3.141592654) * vec3(1.0/2,1.0/2 - 2.0/3,1.0/2-4.0/3);

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
    vec3 res = texture(Source, vTexCoord).xyz;

    vec2 angle = vTexCoord * omega;
	
    float yfactor = (brighten_scanlines + sin(angle.y)) / (brighten_scanlines + 1);
    vec3 xfactors = (brighten_lcd + sin(angle.x + offsets)) / (brighten_lcd + 1);
   
    vec3 color = yfactor * xfactors * res;
   
   FragColor = vec4(color.x, color.y, color.z, 1.0);
}